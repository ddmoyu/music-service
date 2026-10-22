#include "downloads/metadata_support.hpp"

#include <cstdint>
#include <algorithm>

namespace music_service::downloads {
namespace {
std::uint32_t be32(std::string_view bytes, std::size_t at) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value = (value << 8) | static_cast<unsigned char>(bytes[at + i]);
    }
    return value;
}

// 限制每段 UTF-8 字节数；sanitize 仍负责保留名和所有路径字符。
std::string component(std::string_view value, const std::function<std::string(std::string_view)>& sanitize) {
    auto size = std::min<std::size_t>(value.size(), 60);
    while (size < value.size() && size > 0 && (static_cast<unsigned char>(value[size]) & 0xc0U) == 0x80U) {
        --size;
    }
    return sanitize(value.substr(0, size));
}
} // namespace

std::optional<std::string> picture_mime(std::string_view b) {
    if (b.starts_with(std::string_view{"\x89PNG\r\n\x1a\n", 8})) {
        bool header = false;
        bool data = false;
        std::size_t pos = 8;
        while (pos < b.size()) {
            if (b.size() - pos < 12) return {};
            const auto size = be32(b, pos);
            if (size > b.size() - pos - 12) return {};
            const auto kind = b.substr(pos + 4, 4);
            if (!header) {
                if (kind != "IHDR" || size != 13 || be32(b, pos + 8) == 0 || be32(b, pos + 12) == 0) return {};
                header = true;
            } else if (kind == "IHDR") {
                return {};
            }
            if (kind == "IDAT" && size > 0) data = true;
            pos += static_cast<std::size_t>(size) + 12;
            if (kind == "IEND") {
                return size == 0 && data && pos == b.size() ? std::optional<std::string>{"image/png"} : std::nullopt;
            }
        }
        return {};
    }
    if (b.size() >= 4 && static_cast<unsigned char>(b[0]) == 0xff && static_cast<unsigned char>(b[1]) == 0xd8 &&
        static_cast<unsigned char>(b[b.size() - 2]) == 0xff && static_cast<unsigned char>(b.back()) == 0xd9) {
        std::size_t pos = 2;
        bool frame = false;
        while (pos + 4 <= b.size()) {
            if (static_cast<unsigned char>(b[pos++]) != 0xff) return {};
            while (pos < b.size() && static_cast<unsigned char>(b[pos]) == 0xff) ++pos;
            if (pos >= b.size()) return {};
            const auto marker = static_cast<unsigned char>(b[pos++]);
            if (pos + 2 > b.size()) return {};
            const auto size = (static_cast<unsigned char>(b[pos]) << 8) | static_cast<unsigned char>(b[pos + 1]);
            if (size < 2 || static_cast<std::size_t>(size) > b.size() - pos) return {};
            if ((marker >= 0xc0 && marker <= 0xc3) || (marker >= 0xc5 && marker <= 0xc7) ||
                (marker >= 0xc9 && marker <= 0xcb) || (marker >= 0xcd && marker <= 0xcf)) {
                if (size < 8 || (b[pos + 3] == 0 && b[pos + 4] == 0) || (b[pos + 5] == 0 && b[pos + 6] == 0)) return {};
                frame = true;
            }
            if (marker == 0xda) return frame ? std::optional<std::string>{"image/jpeg"} : std::nullopt;
            pos += static_cast<std::size_t>(size);
        }
    }
    return {};
}

std::string embedded_lyrics(const domain::TrackLyrics& lyrics) {
    if (lyrics.unavailable || !lyrics.lrc) return {};
    std::string text = *lyrics.lrc;
    // 保留两份时间轴，翻译独立成段，不按行号强行合并。
    if (lyrics.translated_lrc && !lyrics.translated_lrc->empty()) {
        text += "\n\n[translation]\n" + *lyrics.translated_lrc;
    }
    return text;
}

std::string output_name(const domain::Track& track, const domain::OutputOptions& options,
                        const std::function<std::string(std::string_view)>& sanitize) {
    std::string artists;
    for (const auto& artist : track.artists) {
        if (!artist.name.empty()) artists += (artists.empty() ? "" : ", ") + artist.name;
    }
    const auto fallback = track.ref.source.value() + "-" + track.ref.id.value();
    auto stem = track.title.empty() ? fallback : track.title;
    if (!track.title.empty() && !artists.empty()) {
        if (options.name_format == "artist_title") stem = artists + " - " + track.title;
        if (options.name_format == "title_artist") stem = track.title + " - " + artists;
    }
    std::string path;
    if (options.folder_layout != "flat") {
        path = component(artists.empty() ? "未知歌手" : artists, sanitize) + "/";
        if (options.folder_layout == "artist_album") {
            path += component(track.album && !track.album->title.empty() ? track.album->title : "未知专辑", sanitize) + "/";
        }
    }
    return path + component(stem, sanitize);
}

} // namespace music_service::downloads
