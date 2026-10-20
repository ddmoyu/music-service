#pragma once

#include "application/ports/transport/http_transport.hpp"

#include <cstdint>
#include <string>
#include <string_view>

// Kuwo request construction. Every endpoint used here was confirmed over HTTPS on
// 2026-10-20 (see the T12 record), so no plaintext endpoint is registered. Requests
// carry no cookie: standard quality needs no account.
namespace music_service::providers::kuwo::protocol {

inline constexpr std::string_view source = "kuwo";
inline constexpr std::string_view user_agent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/134.0.0.0 "
    "Safari/537.36";

// Upstream page size limits for single-track search.
inline constexpr std::uint32_t max_search_page_size = 50;

// GET https://www.kuwo.cn/search/searchMusicBykeyWord: pn is the 0-based page.
application::ports::HttpRequest search_tracks(std::string_view keyword, std::uint32_t page, std::uint32_t page_size);

// GET https://www.kuwo.cn/play_detail/{rid}: the song page, read for track details.
application::ports::HttpRequest track_page(std::string_view rid);

// GET https://mobi.kuwo.cn/mobi.s with type=convert_url_with_sign and br=128kmp3: the
// standard-quality audio address. client_id is the per-request user value.
application::ports::HttpRequest standard_audio(std::string_view rid, std::string_view client_id);

} // namespace music_service::providers::kuwo::protocol
