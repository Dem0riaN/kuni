//
// Created by alex2772 on 5/9/26.
//

#include "image.h"

#include "prompts.h"
#include "video.h"
#include "AUI/IO/AFileInputStream.h"
#include "AUI/Util/kAUI.h"

static constexpr auto LOG_TAG = "llmui::image";

AFuture<AString> llmui::image(std::span<const IOpenAIChat::Message> temporaryContext, IOpenAIChat& openAI, AStringView pathToImage, AStringView xmlTag) {
    try
    {
        // Route video formats to the video pipeline
        static const AVector<AString> VIDEO_EXTS = {"webm", "mp4", "mkv", "avi", "mov", "m4v", "flv", "3gp"};
        if (VIDEO_EXTS.contains(AString(APath(pathToImage).extension()).lowercase())) {
            co_return co_await llmui::video(temporaryContext, openAI, pathToImage, xmlTag);
        }

        // Load and embed the image directly
        auto image = AImage::fromFile(pathToImage);
        if (image == nullptr) {
            co_return "<{} description>\nThis media type is not supported\n</{}>"_format(xmlTag, xmlTag);
        }

        // Return the embedded image directly wrapped in XML tags for vision model processing
        auto embedded = IOpenAIChat::embedImage(*image);
        co_return "<{}>{}</{}>"_format(xmlTag, embedded, xmlTag);
    } catch (const AException& e)
    {
        ALogger::err(LOG_TAG) << "Can't embed image"  << e;
        co_return "<{} description>\nThis media type is not supported\n</{}>"_format(xmlTag, xmlTag);
    }
}