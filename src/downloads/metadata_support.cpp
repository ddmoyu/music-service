#include "downloads/metadata_support.hpp"

#include <cstdint>
#include <algorithm>
#include <charconv>
#include <format>
#include <map>

namespace music_service::downloads {

domain::Result<std::optional<domain::Quality>> verified_quality(std::optional<domain::Quality> declared,
    const application::ports::AudioFacts& facts) {
    if (!declared) return declared;
    std::optional<domain::Quality> measured;
    if (facts.container == "flac") {
        measured = facts.sample_rate_hz.value_or(0) >= 48000 && facts.bits_per_sample.value_or(0) >= 24
            ? domain::Quality::hires : domain::Quality::lossless;
    } else if (facts.container == "mp3" && facts.bitrate_bps) {
        measured = *facts.bitrate_bps >= 300000 ? domain::Quality::high : domain::Quality::standard;
    }
    if (!measured && *declared >= domain::Quality::high)
        return domain::Error{domain::ErrorCode::quality_unavailable, "audio bytes do not verify the declared quality", false, {}};
    return measured && *measured < *declared ? measured : declared;
}
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
    // 咪咕封面使用静态 WebP；校验 RIFF 长度、块边界及图像头，不解码或转码。
    // https://developers.google.com/speed/webp/docs/riff_container
    if (b.size() >= 12 && b.substr(0, 4) == "RIFF" && b.substr(8, 4) == "WEBP") {
        const auto le = [&](std::size_t at, std::size_t bytes) {
            std::uint32_t value = 0;
            for (std::size_t i = 0; i < bytes; ++i) value |= static_cast<std::uint32_t>(static_cast<unsigned char>(b[at+i])) << (8*i);
            return value;
        };
        if (le(4, 4) != b.size() - 8) return {};
        bool image = false, extended = false;
        std::size_t pos = 12;
        while (pos < b.size()) {
            if (b.size() - pos < 8) return {};
            auto kind = b.substr(pos, 4);
            const auto size = le(pos + 4, 4);
            pos += 8;
            if (size > b.size() - pos) return {};
            if (kind == "VP8X") {
                if (extended || image || pos != 20 || size != 10 || (le(pos, 1) & 0xc3U) || le(pos+1, 3)) return {};
                extended = true; // 不接受动画标记或保留位。
            } else if (kind == "ANIM" || kind == "ANMF") return {};
            else if (kind == "VP8 ") {
                if (image || size <= 10 || (le(pos, 1) & 1U) || b.substr(pos+3, 3) != std::string_view{"\x9d\x01\x2a",3} ||
                    (le(pos+6, 2) & 0x3fffU) == 0 || (le(pos+8, 2) & 0x3fffU) == 0) return {};
                image = true;
            } else if (kind == "VP8L") {
                if (image || size <= 5 || le(pos, 1) != 0x2fU || (le(pos+4, 1) & 0xe0U)) return {};
                image = true;
            } else if (!extended) return {};
            pos += size;
            if (size & 1U) { if (pos == b.size() || b[pos] != '\0') return {}; ++pos; }
        }
        return image ? std::optional<std::string>{"image/webp"} : std::nullopt;
    }
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

std::string embedded_lyrics(const domain::TrackLyrics& lyrics, std::string_view variant) {
    if (lyrics.unavailable) return {};
    const auto stamp = [](std::int64_t ms) { return std::format("{:02}:{:02}.{:03}", ms / 60000, ms / 1000 % 60, ms % 1000); };
    std::string text, translation = lyrics.translated_lrc.value_or("");
    if ((variant == "verbatim" || !lyrics.lrc) && !lyrics.karaoke.empty() &&
        !(variant == "romanized" && lyrics.romanized_lrc)) {
        // Match translation timestamps exactly, never pair lines by index or invent word timing.
        std::multimap<std::int64_t, std::string> timed;
        std::string unmatched;
        std::string_view rest{translation};
        while (!rest.empty()) {
            const auto nl = rest.find('\n');
            const auto line = rest.substr(0, nl);
            rest = nl == rest.npos ? std::string_view{} : rest.substr(nl + 1);
            const auto colon = line.find(':'), dot = line.find('.'), close = line.find(']');
            std::int64_t minute{}, second{}, fraction{};
            const auto number = [](std::string_view s, std::int64_t& n) {
                auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
                return !s.empty() && ec == std::errc{} && end == s.data() + s.size() && n >= 0;
            };
            bool valid = line.starts_with('[') && colon < dot && dot < close && close < line.size() &&
                close - dot <= 4 && number(line.substr(1, colon - 1), minute) && minute < 1440 &&
                number(line.substr(colon + 1, dot - colon - 1), second) && second < 60 &&
                number(line.substr(dot + 1, close - dot - 1), fraction);
            if (valid) {
                for (std::size_t digits = close - dot - 1; digits < 3; ++digits) fraction *= 10;
                timed.emplace(minute * 60000 + second * 1000 + fraction, std::string{line});
            } else if (!line.empty()) unmatched += std::string{line} + "\n";
        }
        for (const auto& line : lyrics.karaoke) {
            text += "[" + stamp(line.start_ms) + "]";
            for (const auto& word : line.words) text += "<" + stamp(word.start_ms) + ">" + word.text;
            text += "<" + stamp(line.start_ms + line.duration_ms) + ">\n";
            const auto range = timed.equal_range(line.start_ms);
            for (auto i = range.first; i != range.second; ++i) text += i->second + "\n";
            timed.erase(range.first, range.second);
        }
        for (const auto& [time, line] : timed) { (void)time; unmatched += line + "\n"; }
        translation = std::move(unmatched);
    } else if (variant == "romanized" && lyrics.romanized_lrc) {
        text = *lyrics.romanized_lrc;
    } else {
        text = lyrics.lrc.value_or("");
    }
    if (!translation.empty()) text += "\n\n[translation]\n" + translation;
    if (text.empty() && lyrics.romanized_lrc) text = *lyrics.romanized_lrc;
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
