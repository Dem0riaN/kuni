#pragma once
#include "IOpenAIChat.h"
#include "AUI/Thread/AFuture.h"

namespace llmui {

/**
 * @brief Loads image specified at pathToImage, and embeds it directly for vision models.
 * @param temporaryContext additional context to provide to the model (currently unused, kept for API compatibility).
 * @param pathToImage path to image to embed.
 * @param xmlTag xml tag to use.
 * @return embedded image with xml tags.
 */
AFuture<AString> image(std::span<const IOpenAIChat::Message> temporaryContext, IOpenAIChat& openAI, AStringView pathToImage, AStringView xmlTag = "photo");
}