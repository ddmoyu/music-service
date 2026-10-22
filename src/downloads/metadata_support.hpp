#pragma once

#include "domain/catalog/collections.hpp"
#include "domain/tasks/download_intent.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace music_service::downloads {

// 检查 PNG／JPEG 的头部、段边界和尾标记；不解码像素、不做转码。
std::optional<std::string> picture_mime(std::string_view bytes);
std::string embedded_lyrics(const domain::TrackLyrics& lyrics);
std::string output_name(const domain::Track& track, const domain::OutputOptions& options,
                        const std::function<std::string(std::string_view)>& sanitize);

} // namespace music_service::downloads
