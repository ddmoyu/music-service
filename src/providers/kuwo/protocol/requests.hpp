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
application::ports::HttpRequest audio(std::string_view rid, std::string_view client_id, std::string_view bitrate);
application::ports::HttpRequest lyrics(std::string_view rid);

// Collections (T23), confirmed over HTTPS on 2026-10-22. page is 0-based unless noted.
// Every answer is requested as UTF-8; the legacy search route answers single-quoted
// pseudo JSON (parsing::legacy_json).

// GET https://search.kuwo.cn/r.s with ft=playlist or ft=album: collection search.
application::ports::HttpRequest search_collections(std::string_view kind, std::string_view keyword,
                                                   std::uint32_t page, std::uint32_t page_size,
                                                   std::string_view operation);

// GET https://nplserver.kuwo.cn/pl.svc?op=getlistinfo: a playlist and one page of its songs.
application::ports::HttpRequest playlist_page(std::string_view id, std::uint32_t page, std::uint32_t page_size,
                                              std::string_view operation);

// GET https://search.kuwo.cn/r.s?stype=albuminfo: an album and one page of its songs.
application::ports::HttpRequest album_page(std::string_view id, std::uint32_t page, std::uint32_t page_size,
                                           std::string_view operation);

// GET https://wapi.kuwo.cn/api/pc/classify/playlist/getTagList: playlist categories.
application::ports::HttpRequest playlist_tags();

// GET .../getTagPlayList (page is 1-based): playlists of a category.
application::ports::HttpRequest tag_playlists(std::string_view tag_id, std::uint32_t page, std::uint32_t page_size);

// GET .../getRcmPlayList?order=hot (page is 1-based): recommended playlists.
application::ports::HttpRequest recommended_playlists(std::uint32_t page, std::uint32_t page_size);

// GET https://wapi.kuwo.cn/api/pc/bang/list: the chart tree.
application::ports::HttpRequest chart_list();

// GET https://kbangserver.kuwo.cn/ksong.s?type=bang: a chart and one page of its songs.
application::ports::HttpRequest chart_page(std::string_view id, std::uint32_t page, std::uint32_t page_size,
                                           std::string_view operation);

} // namespace music_service::providers::kuwo::protocol
