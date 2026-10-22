#pragma once

#include "application/ports/transport/http_transport.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// 5sing request construction. The search and song APIs only exist as plaintext HTTP:
// on 2026-10-22 their HTTPS ports presented a certificate for other names. They are
// registered as plaintext endpoints (exact host, port 80, path and operation) and carry
// no cookie or other secret. Audio comes from HTTPS CDN addresses.
namespace music_service::providers::fivesing::protocol {

inline constexpr std::string_view source = "fivesing";
inline constexpr std::string_view user_agent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/134.0.0.0 "
    "Safari/537.36";

// The search API answers a fixed number of songs per page.
inline constexpr std::uint32_t search_page_size = 10;

// Every plaintext endpoint this package calls; the bundle registers the same list.
std::vector<application::ports::PlaintextEndpoint> plaintext_endpoints();

// GET http://search.5sing.kugou.com/home/json: page is 1-based; all work types.
application::ports::HttpRequest search_tracks(std::string_view keyword, std::uint32_t page);

// GET http://mobileapi.5sing.kugou.com/song/newget: title, uploader and lyrics of one
// work. operation is the capability asking ("track_detail" or "lyrics").
application::ports::HttpRequest song_info(std::string_view type, std::string_view id, std::string_view operation);

// GET http://mobileapi.5sing.kugou.com/song/getSongUrl: the audio addresses of one work,
// a primary and a backup for each of the sq, hq and lq tiers.
application::ports::HttpRequest song_urls(std::string_view type, std::string_view id);

// HEAD of an audio address, to check it answers with the declared file before it is
// handed to the download.
application::ports::HttpRequest audio_probe(std::string url);

} // namespace music_service::providers::fivesing::protocol
