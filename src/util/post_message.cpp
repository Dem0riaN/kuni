//
// Created by alex2772 on 5/9/26.
//

#include "post_message.h"

#include "config.h"
#include "is_accessible_from_lockdown.h"
#include "AUI/Image/jpg/JpgImageLoader.h"

static constexpr auto LOG_TAG = "util::post_message";

AFuture<td::td_api::object_ptr<td::td_api::message>> util::telegramPostMessage(
    ITelegramClient& telegram, int64_t chatId, AString text, AOptional<_<AImage>> photo, AOptional<APath> audioPath, int64_t replyTo) {
    ALOG_TRACE(LOG_TAG)
        << "telegramPostMessage: chat_id" << chatId << " text=" << text << " photo=" << photo
        << " audioPath=" << audioPath << " replyTo=" << replyTo;
    // Check lockdown mode - only allow PAPIK_CHAT_ID if lockdown is enabled
    if (! co_await util::isAccessibleFromLockdown(telegram, chatId)) {
        throw AException("Lockdown mode is enabled. You can only send messages to chat with ID {} (PAPIK_CHAT_ID)."_format(
            config().papikChatId));
    }

    co_return co_await telegram.sendQueryWithResult([&] {
        auto msg = td::td_api::make_object<td::td_api::sendMessage>();
        msg->chat_id_ = chatId;
        msg->input_message_content_ = [&]() -> td::td_api::object_ptr<td::td_api::InputMessageContent> {
            if (photo) {
                auto content = td::td_api::make_object<td::td_api::inputMessagePhoto>();
                content->caption_ = [&] {
                    auto t = td::td_api::make_object<td::td_api::formattedText>();
                    t->text_ = text;
                    return t;
                }();
                auto tempPath = "temp_{}.jpg"_format(std::chrono::system_clock::now().time_since_epoch().count());
                JpgImageLoader::save(AFileOutputStream(tempPath), **photo);

                // New TDLib API: inputPhoto now requires all parameters in constructor
                auto inputPhoto = td::td_api::make_object<td::td_api::inputPhoto>(
                    ITelegramClient::toPtr(td::td_api::inputFileLocal(tempPath)),  // photo_
                    nullptr,                                                         // thumbnail_
                    nullptr,                                                         // video_
                    std::vector<int32_t>(),                                         // added_sticker_file_ids_
                    photo->get()->width(),                                          // width_
                    photo->get()->height()                                          // height_
                );
                content->photo_ = std::move(inputPhoto);
                content->show_caption_above_media_ = false;
                content->self_destruct_type_ = nullptr;
                content->has_spoiler_ = false;
                return content;
            }

            if (audioPath) {
                auto content = td::td_api::make_object<td::td_api::inputMessageVoiceNote>();

                // In TDLib v1.8.67, voice_note_ expects inputVoiceNote, not inputFileLocal
                auto inputVoiceNote = td::td_api::make_object<td::td_api::inputVoiceNote>();
                inputVoiceNote->voice_note_ = ITelegramClient::toPtr(td::td_api::inputFileLocal(audioPath->absolute().toStdString()));
                inputVoiceNote->duration_ = 0;
                inputVoiceNote->waveform_ = "";
                content->voice_note_ = std::move(inputVoiceNote);

                if (!text.empty()) {
                    content->caption_ = [&] {
                        auto t = td::td_api::make_object<td::td_api::formattedText>();
                        t->text_ = text;
                        return t;
                    }();
                }
                return content;
            }

            auto content = td::td_api::make_object<td::td_api::inputMessageText>();
            content->text_ = [&] {
                auto t = td::td_api::make_object<td::td_api::formattedText>();
                t->text_ = text;
                return t;
            }();
            return content;
        }();
        if (replyTo != 0) {
            // In TDLib v1.8.67, inputMessageReplyToMessage requires 4 parameters:
            // message_id, quote (nullable), checklist_task_id, poll_option_id
            msg->reply_to_ = ITelegramClient::toPtr(td::td_api::inputMessageReplyToMessage(replyTo, nullptr, 0, ""));
        }
        return msg;
    }());
}