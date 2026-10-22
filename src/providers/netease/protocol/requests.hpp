#pragma once

#include "application/ports/transport/http_transport.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// NetEase Cloud Music request construction. Every call is a weapi POST to
// https://music.163.com/weapi/<path>, confirmed anonymously over HTTPS on 2026-10-22
// (see the N07 record); no plaintext endpoint is registered. A user's cookie never
// goes into these builders: the caller adds it as a secret header (with_cookie).
namespace music_service::providers::netease::protocol {

inline constexpr std::string_view source = "netease";
inline constexpr std::string_view user_agent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/134.0.0.0 "
    "Safari/537.36";

// Largest page the service asks cloudsearch for.
inline constexpr std::uint32_t max_search_page_size = 50;

// Every builder takes the weapi secret key (protocol::random_secret_key() in
// production, a fixed one in tests) and the csrf token from the user's cookie, empty
// for anonymous calls.
struct Call {
    std::string secret_key;
    std::string csrf;
};

// cloudsearch/pc, type=1 (songs): one page from offset.
application::ports::HttpRequest search_tracks(std::string_view keyword, std::uint32_t offset, std::uint32_t limit,
                                              const Call& call);

// v3/song/detail for one song id.
application::ports::HttpRequest song_detail(std::string_view id, const Call& call);
application::ports::HttpRequest song_details(const std::vector<std::string>& ids, const Call& call);
application::ports::HttpRequest search_collections(bool playlists, std::string_view keyword,
    std::uint32_t offset, std::uint32_t limit, const Call& call);
application::ports::HttpRequest playlist(std::string_view id, const Call& call);
application::ports::HttpRequest album(std::string_view id, const Call& call);
application::ports::HttpRequest categories(const Call& call);
application::ports::HttpRequest category_playlists(std::string_view category, std::uint32_t offset,
    std::uint32_t limit, const Call& call);
application::ports::HttpRequest recommendations(const Call& call);
application::ports::HttpRequest charts(const Call& call);

// song/enhance/player/url/v1 at the given level ("standard" is 128 kbit/s).
application::ports::HttpRequest song_url(std::string_view id, std::string_view level, const Call& call);

// song/lyric with original, karaoke and translated lyrics.
application::ports::HttpRequest lyric(std::string_view id, const Call& call);

// w/nuser/account/get: who the cookie belongs to (account null when it is nobody).
application::ports::HttpRequest account(const Call& call);

// Adds the user's cookie as a secret header. Only cookie pairs are passed through;
// the cookie travels only to music.163.com over HTTPS.
void with_cookie(application::ports::HttpRequest& request, std::string_view cookie);

// The __csrf value of a cookie header, if any.
[[nodiscard]] std::optional<std::string> csrf_of(std::string_view cookie);

// The MUSIC_U value of a cookie header, if any: the login session the platform checks.
[[nodiscard]] std::optional<std::string> music_u_of(std::string_view cookie);

} // namespace music_service::providers::netease::protocol
