#pragma once
#include "ITelegramClient.h"
#include "IAuthHandler.h"

#include <td/telegram/Client.h>
#include <td/telegram/td_api.h>
#include <td/telegram/td_api.hpp>

#include "AUI/Common/AMap.h"
#include "AUI/Thread/AAsyncHolder.h"
#include "AUI/Thread/AFuture.h"
#include "AUI/Thread/AThread.h"
#include <atomic>


class ATimer;

/**
 * @brief Concrete implementation of ITelegramClient using TDLib.
 *
 * This implementation runs tdlib event loop in a dedicated thread for better performance.
 * All tdlib updates are processed asynchronously without blocking the main thread.
 */
class TelegramClientImpl: public ITelegramClient, public AObject {
public:
    struct StubHandler {
        void operator()(auto& v) const { ALOG_TRACE("TelegramClient") << "Stub: " << to_string(v); }
    };
    TelegramClientImpl();
    ~TelegramClientImpl() override;

    AFuture<Object> sendQuery(td::td_api::object_ptr<td::td_api::Function> f) override;

    /**
     * @brief Set custom authentication handler.
     * @param handler Custom auth handler. If nullptr, uses default stdin handler.
     */
    void setAuthHandler(_<IAuthHandler> handler);

    [[nodiscard]]
    const AFuture<>& waitForConnection() const noexcept override {
        return mWaitForConnection;
    }

    [[nodiscard]] int64_t myId() const override { return mMyId; }

private:
    AFuture<> mWaitForConnection;
    std::unique_ptr<td::ClientManager> mClientManager;
    td::ClientManager::ClientId mClientId{};
    AMap<std::uint64_t, std::function<void(Object)>> mHandlers;
    std::atomic<size_t> mQueryCountLastUpdate{};
    std::atomic<size_t> mCurrentQueryId{};
    std::atomic<int64_t> mMyId{};

    // Dedicated thread for tdlib event loop
    _<AThread> mTdlibThread;
    std::atomic<bool> mRunning{true};
    std::atomic<bool> mProxyConfigured{false};

    // Authentication handler
    _<IAuthHandler> mAuthHandler;

    // Last logged connection state to avoid spam
    ConnectionState mLastLoggedState = ConnectionState::INITIALIZING;
    std::mutex mStateMutex;

    void tdlibEventLoop();
    void initClientManager();
    void setupProxyIfNeeded();

    void commonHandler(td::tl::unique_ptr<td::td_api::Object> object);
    void processResponse(td::ClientManager::Response response);
};
