#pragma once

#include "domain/catalog/collections.hpp"
#include "domain/tasks/download_intent.hpp"
#include "application/ports/processing/audio_processor.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace music_service::downloads {

// 文件证据只能维持或降低平台声明，不能凭扩展名或采样率升级平台档位。
domain::Result<std::optional<domain::Quality>> verified_quality(std::optional<domain::Quality> declared,
    const application::ports::AudioFacts& facts);

// 检查 PNG／JPEG／静态 WebP 的头部和结构边界；不解码像素、不做转码。
std::optional<std::string> picture_mime(std::string_view bytes);
std::string embedded_lyrics(const domain::TrackLyrics& lyrics, std::string_view variant = "lrc");
std::string output_name(const domain::Track& track, const domain::OutputOptions& options,
                        const std::function<std::string(std::string_view)>& sanitize);

} // namespace music_service::downloads
