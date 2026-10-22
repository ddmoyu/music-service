#pragma once

#include "domain/catalog/collections.hpp"
#include "domain/catalog/track.hpp"
#include "domain/result.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Kuwo collection parsing (T23): playlist and album search, playlist, album and chart
// pages, categories, category and recommended playlists and the chart tree. Pure
// functions; a changed body is UPSTREAM_FAILED with {"protocol", reason}, an unknown
// entity NOT_FOUND. Unknown fields stay empty; counts are the platform's claims.
namespace music_service::providers::kuwo::parsing {

// Collection ids (playlists, albums, charts, categories): 1..18 decimal digits.
[[nodiscard]] bool is_valid_collection_id(std::string_view id) noexcept;

// The legacy search route answers JSON with single-quoted strings. Rewrites it as JSON:
// quotes inside a single-quoted string are escaped, \' becomes ', other escapes pass
// through unchanged. Unterminated strings are a protocol error.
domain::Result<std::string> legacy_json(std::string_view body);

template <class Item> struct ListPage {
    std::vector<Item> items;
    std::uint64_t total{0};
};

// search.kuwo.cn/r.s, ft=playlist (abslist) and ft=album (albumlist). page is the PN
// the answer reports; another page than requested is a protocol error.
domain::Result<ListPage<domain::Playlist>> parse_playlist_search(std::string_view body, std::uint32_t page);
domain::Result<ListPage<domain::Album>> parse_album_search(std::string_view body, std::uint32_t page);

// pl.svc getlistinfo. Kuwo answers an unknown pid with another, empty playlist: an
// answer whose id differs from the request is NOT_FOUND.
struct PlaylistPage {
    domain::Playlist playlist;
    std::vector<domain::Track> tracks;
};
domain::Result<PlaylistPage> parse_playlist_page(std::string_view body, std::string_view id);

// r.s stype=albuminfo. An unknown album is an empty musiclist without albumid.
struct AlbumPage {
    domain::Album album;
    std::vector<domain::Track> tracks;
};
domain::Result<AlbumPage> parse_album_page(std::string_view body, std::string_view id);

// getTagList: the playlist tags (digest 10000) with their group name; zones are left out.
domain::Result<std::vector<domain::Category>> parse_categories(std::string_view body);

// getTagPlayList and getRcmPlayList: one page of playlists.
domain::Result<ListPage<domain::Playlist>> parse_playlist_list(std::string_view body);

// bang/list: the charts at the tree's leaves, keyed by the id ksong.s takes.
domain::Result<std::vector<domain::Chart>> parse_charts(std::string_view body);

// ksong.s: a chart and one page of its songs. An unknown chart answers {}.
struct ChartPage {
    domain::Chart chart;
    std::vector<domain::Track> tracks;
};
domain::Result<ChartPage> parse_chart_page(std::string_view body, std::string_view id);

} // namespace music_service::providers::kuwo::parsing
