//
// Created by alex2772 on 3/2/26.
//

#include "TelegramClientImpl.h"
#include "StdinAuthHandler.h"

#include "config.h"
#include "AUI/Util/kAUI.h"

using namespace std::chrono_literals;

namespace {
static constexpr auto LOG_TAG = "TelegramClient";
// Timeout for tdlib receive() - balance between responsiveness and CPU usage
static constexpr auto TDLIB_RECEIVE_TIMEOUT = 1.0; // 1 second
}   // namespace

TelegramClientImpl::TelegramClientImpl() : mAuthHandler(_new<StdinAuthHandler>()) {
    ALOG_TRACE(LOG_TAG) << "TelegramClientImpl::TelegramClientImpl";
    setSlotsCallsOnlyOnMyThread(true);

    td::ClientManager::execute(td::td_api::make_object<td::td_api::setLogVerbosityLevel>(0));
    initClientManager();

    // Start dedicated thread for tdlib event loop
    mTdlibThread = _new<AThread>([this] { tdlibEventLoop(); });
    mTdlibThread->start();
}

TelegramClientImpl::~TelegramClientImpl() {
    ALOG_TRACE(LOG_TAG) << "TelegramClientImpl::~TelegramClientImpl";
    mRunning = false;
    if (mTdlibThread) {
        mTdlibThread->join();
    }
}

void TelegramClientImpl::setAuthHandler(_<IAuthHandler> handler) {
    if (handler) {
        mAuthHandler = std::move(handler);
    } else {
        mAuthHandler = _new<StdinAuthHandler>();
    }
}

AFuture<ITelegramClient::Object> TelegramClientImpl::sendQuery(td::td_api::object_ptr<td::td_api::Function> f) {
    ALOG_TRACE(LOG_TAG) << "sendQuery " << td::td_api::to_string(f);

    // Throttling to prevent ban - reset counter periodically
    auto currentCount = mQueryCountLastUpdate.fetch_add(1, std::memory_order_relaxed);
    if (currentCount >= 20) {
        // Telegram is strict about using 3rdparty telegram clients. For this reason, we have to ensure that we wouldn't
        // trigger their security leading to ban of the account.
        ALogger::info(LOG_TAG) << "Too many calls to tdlib! Throttling...\n" << AStacktrace::capture(1, 8);
        co_await AThread::asyncSleep(1s);
    }

    auto query_id = mCurrentQueryId.fetch_add(1, std::memory_order_relaxed) + 1;
    AFuture<ITelegramClient::Object> result;
    mHandlers.emplace(query_id, [result](Object object) { result.supplyValue(std::move(object)); });
    mClientManager->send(mClientId, query_id, std::move(f));
    co_return co_await result;
}

void TelegramClientImpl::initClientManager() {
    ALOG_TRACE(LOG_TAG) << "initClientManager";
    ALogger::info(LOG_TAG) << "Initializing TDLib client manager";

    mClientManager = std::make_unique<td::ClientManager>();
    mClientId = mClientManager->create_client_id();

    ALogger::info(LOG_TAG) << "TDLib client created, ID: " << mClientId;

    // Store proxy configuration for later setup (after authorization)
    mProxyConfigured = false;

    sendQueryWithResult(td::td_api::make_object<td::td_api::getOption>("version"))
        .onSuccess([](const td::td_api::object_ptr<td::td_api::OptionValue>& object) {
            td::td_api::downcast_call(
                *const_cast<td::td_api::object_ptr<td::td_api::OptionValue>&>(object),
                aui::lambda_overloaded {
                  [](const td::td_api::optionValueString& u) {
                      ALogger::info(LOG_TAG) << "TDLib version: " << u.value_;
                  },
                  [](auto&) {} });
        })
        .onError([](const AException& error) {
            ALogger::err(LOG_TAG) << "Failed to get TDLib version: " << error.getMessage();
        });
}

void TelegramClientImpl::setupProxyIfNeeded() {
    if (mProxyConfigured || !config().telegramMtprotoProxyEnabled) {
        return;
    }

    if (config().telegramMtprotoProxyServer.empty() || config().telegramMtprotoProxySecret.empty()) {
        ALogger::warn(LOG_TAG) << "MTProto proxy is enabled but server or secret is not configured. Skipping proxy setup.";
        return;
    }

    ALogger::info(LOG_TAG) << "Configuring MTProto proxy: " << config().telegramMtprotoProxyServer
                           << ":" << config().telegramMtprotoProxyPort;

    auto proxy = td::td_api::make_object<td::td_api::proxy>(
        config().telegramMtprotoProxyServer.toStdString(),
        config().telegramMtprotoProxyPort,
        td::td_api::make_object<td::td_api::proxyTypeMtproto>(
            config().telegramMtprotoProxySecret.toStdString()
        )
    );

    sendQuery(
        td::td_api::make_object<td::td_api::addProxy>(
            std::move(proxy),
            true,  // enable proxy immediately
            ""     // comment (empty string)
        )
    ).onSuccess([this](const td::td_api::object_ptr<td::td_api::Object>& result) {
        ALogger::info(LOG_TAG) << "MTProto proxy configured successfully";
        mProxyConfigured = true;
    }).onError([](const AException& error) {
        ALogger::err(LOG_TAG) << "Failed to configure MTProto proxy: " << error.getMessage();
    });
}

void TelegramClientImpl::tdlibEventLoop() {
    ALOG_TRACE(LOG_TAG) << "tdlibEventLoop started";
    AThread::setName("TDLib-Loop");

    auto lastThrottleReset = std::chrono::steady_clock::now();

    while (mRunning) {
        // Reset query counter every second to allow throttling
        auto now = std::chrono::steady_clock::now();
        if (now - lastThrottleReset >= 1s) {
            mQueryCountLastUpdate.store(0, std::memory_order_relaxed);
            lastThrottleReset = now;
        }

        // Receive events with timeout to avoid busy-waiting
        auto response = mClientManager->receive(TDLIB_RECEIVE_TIMEOUT);
        if (!response.object) {
            continue;
        }

        // Process response in the main thread context to maintain thread safety
        // Wrap response in shared_ptr since it's not copyable
        auto sharedResponse = std::make_shared<td::ClientManager::Response>(std::move(response));
        getThread()->enqueue([this, sharedResponse]() {
            processResponse(std::move(*sharedResponse));
        });
    }

    ALOG_TRACE(LOG_TAG) << "tdlibEventLoop finished";
}

void TelegramClientImpl::processResponse(td::ClientManager::Response response) {
    ALOG_TRACE(LOG_TAG) << "processResponse";
    if (!response.object) {
        return;
    }

    if (auto c = mHandlers.contains(response.request_id)) {
        auto handler = std::move(c->second);
        mHandlers.erase(*c);
        handler(std::move(response.object));
        return;
    }

    commonHandler(std::move(response.object));
}

void TelegramClientImpl::commonHandler(td::tl::unique_ptr<td::td_api::Object> object) {
    ALOG_TRACE(LOG_TAG) << "commonHandler";
    // move the ownership from unique_ptr to shared_ptr
    auto objectShared = aui::ptr::manage_shared(object.release());

    emit onEvent(objectShared);
    td::td_api::downcast_call(
        *objectShared,
        aui::lambda_overloaded {
          [this](td::td_api::updateAuthorizationState& update_authorization_state) {
              td::td_api::downcast_call(
                  *update_authorization_state.authorization_state_,
                  aui::lambda_overloaded {
                    [this](td::td_api::authorizationStateWaitTdlibParameters& u) {
                        ALogger::info(LOG_TAG) << "[Initialization] Setting TDLib parameters";
                        auto parameters = td::td_api::make_object<td::td_api::setTdlibParameters>();
                        parameters->database_directory_ = "tdlib";
                        parameters->use_message_database_ = true;
                        parameters->use_secret_chats_ = true;

                        parameters->api_id_ = config().telegramApiId;
                        parameters->api_hash_ = config().telegramApiHash;
                        parameters->system_language_code_ = "en";
                        parameters->device_model_ = "Desktop";
                        parameters->application_version_ = AUI_PP_STRINGIZE(AUI_CMAKE_PROJECT_VERSION);

                        ALogger::info(LOG_TAG) << "[Initialization] API ID: " << parameters->api_id_;
                        sendQuery(std::move(parameters)).onSuccess([this](const auto&) {
                            ALogger::info(LOG_TAG) << "[Initialization] TDLib parameters set successfully";
                            // Setup proxy after TDLib is initialized
                            setupProxyIfNeeded();
                        }).onError([](const AException& error) {
                            ALogger::err(LOG_TAG) << "[Initialization] Failed to set TDLib parameters: " << error.getMessage();
                        });
                    },
                    [this](td::td_api::authorizationStateReady& u) {
                        ALogger::info(LOG_TAG) << "[Authentication] logged in.";
                        emit loggedIn;
                    },
                    [this](td::td_api::authorizationStateWaitPhoneNumber& s) {
                        ALogger::info(LOG_TAG) << "[Authentication] Phone number required";

                        // Request phone number asynchronously and execute callback in this object's thread
                        mAuthHandler->requestPhoneNumber().onSuccess([this](const AString& phoneNumber) {
                            // Execute in TelegramClientImpl's thread
                            getThread()->enqueue([this, phoneNumber]() {
                                ALogger::info(LOG_TAG) << "[Authentication] Phone number provided: " << phoneNumber;
                                auto params = td::td_api::make_object<td::td_api::setAuthenticationPhoneNumber>();
                                params->phone_number_ = phoneNumber.toStdString();
                                sendQuery(std::move(params)).onSuccess([this](const auto& result) {
                                    ALogger::info(LOG_TAG) << "[Authentication] Phone number sent successfully";
                                }).onError([](const AException& error) {
                                    ALogger::err(LOG_TAG) << "[Authentication] Failed to send phone number: " << error.getMessage();
                                });
                            });
                        }).onError([this](const AException& error) {
                            getThread()->enqueue([this, error]() {
                                ALogger::err(LOG_TAG) << "[Authentication] Failed to get phone number: " << error.getMessage();
                            });
                        });
                    },
                    [this](td::td_api::authorizationStateWaitPassword& s) {
                        ALogger::info(LOG_TAG) << "[Authentication] Cloud password required (2FA)";

                        // Request password asynchronously and execute callback in this object's thread
                        mAuthHandler->requestPassword().onSuccess([this](const AString& password) {
                            getThread()->enqueue([this, password]() {
                                ALogger::info(LOG_TAG) << "[Authentication] Password provided";
                                auto params = td::td_api::make_object<td::td_api::checkAuthenticationPassword>();
                                params->password_ = password.toStdString();
                                sendQuery(std::move(params)).onSuccess([this](const auto& result) {
                                    ALogger::info(LOG_TAG) << "[Authentication] Password sent successfully";
                                }).onError([](const AException& error) {
                                    ALogger::err(LOG_TAG) << "[Authentication] Failed to send password: " << error.getMessage();
                                });
                            });
                        }).onError([this](const AException& error) {
                            getThread()->enqueue([this, error]() {
                                ALogger::err(LOG_TAG) << "[Authentication] Failed to get password: " << error.getMessage();
                            });
                        });
                    },
                    [this](td::td_api::authorizationStateWaitCode& s) {
                        ALogger::info(LOG_TAG) << "[Authentication] Verification code required";

                        // Request verification code asynchronously and execute callback in this object's thread
                        mAuthHandler->requestVerificationCode().onSuccess([this](const AString& code) {
                            getThread()->enqueue([this, code]() {
                                ALogger::info(LOG_TAG) << "[Authentication] Verification code provided";
                                auto params = td::td_api::make_object<td::td_api::checkAuthenticationCode>();
                                params->code_ = code.toStdString();
                                sendQuery(std::move(params)).onSuccess([this](const auto& result) {
                                    ALogger::info(LOG_TAG) << "[Authentication] Verification code sent successfully";
                                }).onError([](const AException& error) {
                                    ALogger::err(LOG_TAG) << "[Authentication] Failed to send verification code: " << error.getMessage();
                                });
                            });
                        }).onError([this](const AException& error) {
                            getThread()->enqueue([this, error]() {
                                ALogger::err(LOG_TAG) << "[Authentication] Failed to get verification code: " << error.getMessage();
                            });
                        });
                    },
                    [this](td::td_api::authorizationStateClosed& u) {
                        ALogger::warn(LOG_TAG) << "[Authorization] TDLib closed, reinitializing...";
                        // Don't reinitialize - this causes infinite loop
                        // getThread()->enqueue([this] {
                        //     ALogger::info(LOG_TAG) << "[Authorization] Reinitializing client manager...";
                        //     initClientManager();
                        // });
                    },
                    [this](auto& v) { ALogger::info(LOG_TAG) << "Stub: " << td::td_api::to_string(v); },
                  });
          },

          [this](td::td_api::updateConnectionState& u) {
              ConnectionState newState = connectionState;
              td::td_api::downcast_call(
                  *u.state_,
                  aui::lambda_overloaded {
                    [&](td::td_api::connectionStateReady&) {
                        newState = ConnectionState::CONNECTED;
                        mWaitForConnection.supplyValue();
                    },
                    [&](td::td_api::connectionStateConnecting&) { newState = ConnectionState::CONNECTING; },
                    [&](td::td_api::connectionStateConnectingToProxy&) {
                        newState = ConnectionState::CONNECTING_TO_PROXY;
                    },
                    [&](td::td_api::connectionStateWaitingForNetwork&) {
                        newState = ConnectionState::WAITING_FOR_NETWORK;
                    },
                    [&](td::td_api::connectionStateUpdating&) { newState = ConnectionState::UPDATING; },
                  });

              // Only log state changes to avoid spam
              {
                  std::lock_guard lock(mStateMutex);
                  if (mLastLoggedState != newState) {
                      switch (newState) {
                          case ConnectionState::INITIALIZING:
                              ALogger::info(LOG_TAG) << "Connection state: initializing...";
                              break;
                          case ConnectionState::CONNECTED:
                              ALogger::info(LOG_TAG) << "Connection state: connected";
                              break;
                          case ConnectionState::CONNECTING:
                              ALogger::info(LOG_TAG) << "Connection state: connecting... (check VPN/proxy settings)";
                              break;
                          case ConnectionState::CONNECTING_TO_PROXY:
                              ALogger::info(LOG_TAG) << "Connection state: connecting to proxy...";
                              break;
                          case ConnectionState::UPDATING:
                              ALogger::info(LOG_TAG) << "Connection state: updating...";
                              break;
                          case ConnectionState::WAITING_FOR_NETWORK:
                              ALogger::info(LOG_TAG) << "Connection state: waiting for network...";
                              break;
                      }
                      mLastLoggedState = newState;
                  }
              }
              connectionState = newState;
          },
          [this](td::td_api::updateOption& u) {
              if (u.name_ == "my_id") {
                  td::td_api::downcast_call(
                      *u.value_,
                      aui::lambda_overloaded {
                        [&](td::td_api::optionValueInteger& i) { mMyId.store(i.value_, std::memory_order_relaxed); },
                        [&](auto&) {},
                      });
              }
          },
          // ── User cache updates ──────────────────────────────────────────
          [this](td::td_api::updateUser& u) {
              if (auto dst = mUserCache.contains(u.user_->id_)) {
                  dst->second->tg = std::move(*u.user_);
              }
          },
          [this](td::td_api::updateUserStatus& u) {
              if (auto dst = mUserCache.contains(u.user_id_)) {
                  dst->second->tg.status_ = std::move(u.status_);
              }
          },
          // ── Chat cache updates ───────────────────────────────────────────
          [this](td::td_api::updateChatTitle& u) {
              if (auto dst = mChatCache.contains(u.chat_id_)) {
                  dst->second->tg.title_ = std::move(u.title_);
              }
          },
          [this](td::td_api::updateChatPhoto& u) {
              if (auto dst = mChatCache.contains(u.chat_id_)) {
                  dst->second->tg.photo_ = std::move(u.photo_);
              }
          },
          [this](td::td_api::updateChatPermissions& u) {
              if (auto dst = mChatCache.contains(u.chat_id_)) {
                  dst->second->tg.permissions_ = std::move(u.permissions_);
              }
          },
          [this](td::td_api::updateChatLastMessage& u) {
              if (auto dst = mChatCache.contains(u.chat_id_)) {
                  dst->second->tg.last_message_ = std::move(u.last_message_);
              }
          },
          [this](td::td_api::updateChatReadInbox& u) {
              if (auto dst = mChatCache.contains(u.chat_id_)) {
                  dst->second->tg.last_read_inbox_message_id_ = u.last_read_inbox_message_id_;
                  dst->second->tg.unread_count_ = u.unread_count_;
              }
          },
          [this](td::td_api::updateChatReadOutbox& u) {
              if (auto dst = mChatCache.contains(u.chat_id_)) {
                  dst->second->tg.last_read_outbox_message_id_ = u.last_read_outbox_message_id_;
              }
          },
          [this](td::td_api::updateChatUnreadMentionCount& u) {
              if (auto dst = mChatCache.contains(u.chat_id_)) {
                  dst->second->tg.unread_mention_count_ = u.unread_mention_count_;
              }
          },
          [this](td::td_api::updateChatUnreadReactionCount& u) {
              if (auto dst = mChatCache.contains(u.chat_id_)) {
                  dst->second->tg.unread_reaction_count_ = u.unread_reaction_count_;
              }
          },
          [this](td::td_api::updateChatNotificationSettings& u) {
              if (auto dst = mChatCache.contains(u.chat_id_)) {
                  dst->second->tg.notification_settings_ = std::move(u.notification_settings_);
              }
          },
          [this](td::td_api::updateChatIsMarkedAsUnread& u) {
              if (auto dst = mChatCache.contains(u.chat_id_)) {
                  dst->second->tg.is_marked_as_unread_ = u.is_marked_as_unread_;
              }
          },
          [this](td::td_api::updateChatBlockList& u) {
              if (auto dst = mChatCache.contains(u.chat_id_)) {
                  dst->second->tg.block_list_ = std::move(u.block_list_);
              }
          },
          [this](td::td_api::updateChatHasScheduledMessages& u) {
              if (auto dst = mChatCache.contains(u.chat_id_)) {
                  dst->second->tg.has_scheduled_messages_ = u.has_scheduled_messages_;
              }
          },
          [this](td::td_api::updateChatDraftMessage& u) {
              if (auto dst = mChatCache.contains(u.chat_id_)) {
                  dst->second->tg.draft_message_ = std::move(u.draft_message_);
              }
          },
          [this](td::td_api::updateChatMessageAutoDeleteTime& u) {
              if (auto dst = mChatCache.contains(u.chat_id_)) {
                  dst->second->tg.message_auto_delete_time_ = u.message_auto_delete_time_;
              }
          },
          [this](td::td_api::updateChatEmojiStatus& u) {
              if (auto dst = mChatCache.contains(u.chat_id_)) {
                  dst->second->tg.emoji_status_ = std::move(u.emoji_status_);
              }
          },
          [this](td::td_api::updateChatBackground& u) {
              if (auto dst = mChatCache.contains(u.chat_id_)) {
                  dst->second->tg.background_ = std::move(u.background_);
              }
          },
          [this](td::td_api::updateChatTheme& u) {
              if (auto dst = mChatCache.contains(u.chat_id_)) {
                  dst->second->tg.theme_ = std::move(u.theme_);
              }
          },
          [this](td::td_api::updateChatReplyMarkup& u) {
              if (auto dst = mChatCache.contains(u.chat_id_)) {
                  dst->second->tg.reply_markup_message_id_ = u.reply_markup_message_->id_;
              }
          },
          [this](td::td_api::updateMessageSendSucceeded& u) {
              const auto oldId = u.old_message_id_;
              const auto newId = u.message_->id_;
              auto& cacheForThisChat = mMessageCache[u.message_->chat_id_];
              auto dst = cacheForThisChat[newId] = cacheForThisChat[oldId];
              if (dst == nullptr) {
                  dst = cacheForThisChat[newId] = cacheForThisChat[oldId] = _new<Cached<td::td_api::message>>();
              }
              dst->tg = std::move(*u.message_);
              if (!dst->populated.hasResult()) {
                  dst->populated.supplyValue();
              }
          },
          [this](td::td_api::updateChatPosition& u) {
              auto chat = getChat(u.chat_id_);
              if (!chat.hasValue()) {
                  return;
              }
              for (auto& i : (*chat)->positions_) {
                  if (i->list_->get_id() == u.position_->list_->get_id()) {
                      i = std::move(u.position_);
                      return;
                  }
              }
              (*chat)->positions_.push_back(std::move(u.position_));
          },
          [this](td::td_api::updateMessageSendFailed& u) {
              const auto oldId = u.old_message_id_;
              const auto newId = u.message_->id_;
              auto& cacheForThisChat = mMessageCache[u.message_->chat_id_];
              auto dst = cacheForThisChat[newId] = cacheForThisChat[oldId];
              if (dst == nullptr) {
                  dst = cacheForThisChat[newId] = cacheForThisChat[oldId] = _new<Cached<td::td_api::message>>();
              }
              dst->tg = std::move(*u.message_);
              if (!dst->populated.hasResult()) {
                  dst->populated.supplyValue();
              }
          },
          [&](auto& i) {},
        });
}
