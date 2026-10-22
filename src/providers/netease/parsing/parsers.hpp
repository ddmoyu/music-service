#pragma once

#include "domain/catalog/collections.hpp"
#include "domain/catalog/track.hpp"
#include "domain/result.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// NetEase response parsing. Pure functions over weapi response bodies (JSON with a
// top-level "code"). A changed or broken body is UPSTREAM_FAILED with details
// {"protocol", reason}, never an empty result. Top-level codes: 301 is
// CREDENTIALS_REQUIRED, 405 UPSTREAM_RATE_LIMITED (retryable), -460/-462 (risk
// control) and anything else a final UPSTREAM_FAILED carrying {"netease_code", code}.
namespace music_service::providers::netease::parsing {

// Song ids are 1..19 decimal digits without a leading zero.
[[nodiscard]] bool is_valid_song_id(std::string_view id) noexcept;

struct SearchPage {
    std::vector<domain::Track> tracks;
    std::uint64_t total{0}; // songCount; the platform's estimate changes between pages
};

// cloudsearch/pc type=1. A result with songCount 0 and no songs is an empty page.
domain::Result<SearchPage> parse_search(std::string_view body);

// v3/song/detail for id. An empty song list is NOT_FOUND; a song with another id is a
// protocol error.
domain::Result<domain::Track> parse_detail(std::string_view body, std::string_view id);

inline constexpr std::size_t max_playlist_ids = 10000;
inline constexpr std::size_t max_album_tracks = 2000;
struct PlaylistData {
    domain::Playlist detail;
    std::vector<std::string> ids;
    std::string revision; // 更新标记及有序 ID 清单的指纹，用于检测翻页期间变化
};
struct AlbumData {
    domain::Album detail;
    std::vector<domain::Track> tracks;
    std::string revision;
};
template<class T> struct CollectionSearch {
    std::vector<T> items;
    std::uint64_t total{};
};
domain::Result<PlaylistData> parse_playlist(std::string_view body, std::string_view id);
domain::Result<AlbumData> parse_album(std::string_view body, std::string_view id);
domain::Result<std::vector<domain::Track>> parse_tracks(std::string_view body);
domain::Result<CollectionSearch<domain::Playlist>> parse_playlist_search(std::string_view body);
domain::Result<CollectionSearch<domain::Album>> parse_album_search(std::string_view body);
domain::Result<std::vector<domain::Category>> parse_categories(std::string_view body);
domain::Result<std::vector<domain::Chart>> parse_charts(std::string_view body);
struct DiscoveryPlaylists {
    std::vector<domain::Playlist> items;
    std::optional<std::uint64_t> total;
    bool more{};
};
domain::Result<DiscoveryPlaylists> parse_discovery_playlists(std::string_view body, bool recommendations);
struct Link { std::string kind; std::string id; };
domain::Result<Link> parse_link(std::string_view link);

// What player/url/v1 says about the audio of the requested id.
struct AudioEvidence {
    int code{0};                 // per-song code: 200 playable, 404 no copyright or no song, -110 needs a paying account
    std::optional<std::int64_t> fee; // Preserve upstream entitlement category without inferring account access.
    std::string url;             // https on a *.music.126.net host when code is 200
    std::string container;       // "type", e.g. "mp3"
    std::string level;           // e.g. "standard"
    std::optional<std::int64_t> bitrate_bps;
    std::optional<std::int64_t> size_bytes;
    std::optional<std::int64_t> duration_ms;
    std::optional<std::int64_t> expires_in_seconds; // "expi"
    std::string md5;             // the CDN's strong ETag for the file
    // freeTrialInfo is set: the address is a trial clip, not the song.
    bool trial{false};
};

domain::Result<AudioEvidence> parse_audio(std::string_view body, std::string_view id);

// song/lyric. pureMusic, nolyric or uncollected mean the track has no lyrics; the
// placeholder text NetEase sends for pure music is not returned as lyrics.
domain::Result<domain::TrackLyrics> parse_lyrics(std::string_view body);

struct Account {
    bool signed_in{false};
    std::optional<std::string> nickname;
    std::optional<std::int64_t> vip_type; // 0 none; other values are the platform's membership kinds
};

// w/nuser/account/get. account null is a cookie that belongs to nobody.
domain::Result<Account> parse_account(std::string_view body);

// The song id of a NetEase song link: music.163.com/song?id=N, music.163.com/#/song?id=N,
// music.163.com/song/N, music.163.com/m/song?id=N and y.music.163.com/m/song?id=N,
// http or https. Short links (163cn.tv) would need the network and are refused.
domain::Result<std::string> parse_track_link(std::string_view link);

} // namespace music_service::providers::netease::parsing
