#include "providers/kuwo/parsing/parsers.hpp"

#include <mbedtls/base64.h>
#include <zlib.h>
#include <charconv>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <iconv.h>
#endif

namespace music_service::providers::kuwo::parsing {
namespace {
constexpr std::size_t max_plain = 2 * 1024 * 1024;
domain::Error invalid(std::string reason) {
    return {domain::ErrorCode::upstream_failed, "kuwo lyric response could not be read", false,
        {{"protocol", std::move(reason)}}};
}
domain::Result<std::string> utf8(std::string_view input) {
    if (input.empty()) return std::string{};
#ifdef _WIN32
    const auto size = static_cast<int>(input.size());
    const auto n = MultiByteToWideChar(54936, MB_ERR_INVALID_CHARS, input.data(), size, nullptr, 0);
    if (n <= 0) return invalid("invalid GB18030 lyrics");
    std::wstring wide(static_cast<std::size_t>(n), L'\0');
    if (MultiByteToWideChar(54936, MB_ERR_INVALID_CHARS, input.data(), size, wide.data(), n) != n)
        return invalid("invalid GB18030 lyrics");
    const auto bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), n, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return invalid("invalid lyric Unicode");
    std::string out(static_cast<std::size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), n, out.data(), bytes, nullptr, nullptr) != bytes)
        return invalid("invalid lyric Unicode");
    return out;
#else
    auto converter = iconv_open("UTF-8", "GB18030");
    if (converter == reinterpret_cast<iconv_t>(-1)) return invalid("GB18030 converter unavailable");
    std::string out(input.size() * 4 + 4, '\0');
    auto* from = const_cast<char*>(input.data());
    auto* to = out.data();
    auto remaining = input.size();
    auto space = out.size();
    const auto converted = iconv(converter, &from, &remaining, &to, &space);
    iconv_close(converter);
    if (converted == static_cast<std::size_t>(-1) || remaining != 0) return invalid("invalid GB18030 lyrics");
    out.resize(out.size() - space);
    return out;
#endif
}
bool number(std::string_view s, bool negative = false) {
    int n{};
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
    return !s.empty() && ec == std::errc{} && end == s.data() + s.size() && (negative || n >= 0);
}
domain::Result<std::string> text_of(std::string_view line) {
    std::string text;
    while (!line.empty()) {
        if (line.front() == '<') {
            const auto end = line.find('>');
            if (end == std::string_view::npos || end > 30) return invalid("invalid word marker");
            const auto marker = line.substr(1, end - 1);
            const auto comma = marker.find(',');
            if (comma == std::string_view::npos || !number(marker.substr(0, comma), true) ||
                !number(marker.substr(comma + 1), true)) return invalid("invalid word marker");
            line.remove_prefix(end + 1);
        } else {
            text.push_back(line.front());
            line.remove_prefix(1);
        }
    }
    return text;
}
} // namespace

domain::Result<domain::TrackLyrics> parse_lyrics(std::string_view body) {
    if (body.size() > 512 * 1024) return invalid("compressed lyrics exceed limit");
    domain::TrackLyrics lyrics;
    if (body.starts_with("tp=none\r\n")) { lyrics.unavailable = true; return lyrics; }
    const auto boundary = body.find("\r\n\r\n");
    if (!body.starts_with("tp=content\r\n") || boundary == std::string_view::npos || boundary > 8192)
        return invalid("invalid lyric header");
    body.remove_prefix(boundary + 4);
    std::string inflated(max_plain, '\0');
    uLongf length = static_cast<uLongf>(inflated.size());
    uLong consumed = static_cast<uLong>(body.size());
    const auto status = uncompress2(reinterpret_cast<Bytef*>(inflated.data()), &length,
        reinterpret_cast<const Bytef*>(body.data()), &consumed);
    if (status != Z_OK || consumed != body.size()) return invalid("invalid or oversized zlib lyrics");
    inflated.resize(length);
    std::string encoded;
    for (const char c : inflated) if (c != ' ' && c != '\r' && c != '\n' && c != '\t') encoded.push_back(c);
    std::string raw(encoded.size(), '\0');
    std::size_t decoded{};
    if (mbedtls_base64_decode(reinterpret_cast<unsigned char*>(raw.data()), raw.size(), &decoded,
        reinterpret_cast<const unsigned char*>(encoded.data()), encoded.size()) != 0)
        return invalid("invalid lyric base64");
    raw.resize(decoded);
    constexpr std::string_view key = "yeelion";
    for (std::size_t i = 0; i < raw.size(); ++i) raw[i] ^= key[i % key.size()];
    auto unicode = utf8(raw);
    if (!unicode) return unicode.error();
    if (unicode->find('\0') != std::string::npos) return invalid("NUL in lyrics");
    std::string_view lines{*unicode};
    std::string lrc;
    std::size_t count{};
    bool timed{};
    while (!lines.empty()) {
        if (++count > 10000) return invalid("too many lyric lines");
        const auto end = lines.find('\n');
        auto line = lines.substr(0, end);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.size() >= 11 && line[0] == '[' && line[3] == ':' && line[6] == '.' && line[10] == ']') {
            if (!number(line.substr(1, 2)) || !number(line.substr(4, 2)) || !number(line.substr(7, 3)) ||
                line[4] > '5') return invalid("invalid lyric timestamp");
            auto text = text_of(line.substr(11));
            if (!text) return text.error();
            if (!text->empty()) { lrc.append(line.substr(0, 11)); lrc += *text + '\n'; timed = true; }
        } else if (line.starts_with("[ti:") || line.starts_with("[ar:") || line.starts_with("[al:") ||
                   line.starts_with("[by:") || line.starts_with("[offset:")) {
            lrc.append(line); lrc.push_back('\n');
        } else if (!line.empty() && !line.starts_with("[kuwo:") && !line.starts_with("[ver:")) {
            return invalid("unrecognized lyric line");
        }
        if (end == std::string_view::npos) break;
        lines.remove_prefix(end + 1);
    }
    if (timed) lyrics.lrc = std::move(lrc);
    else lyrics.unavailable = true;
    return lyrics;
}
} // namespace music_service::providers::kuwo::parsing
