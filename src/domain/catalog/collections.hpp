#pragma once

#include "domain/catalog/refs.hpp"
#include "domain/catalog/track.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace music_service::domain {

// A thumbnail anyone may fetch; covers that need secret headers stay empty
// (data-models.md, 歌曲与集合).
using Artwork = std::optional<std::string>;

// Platform collections and catalog entries. Unknown values stay empty; a collection's
// track_count is the platform's claim, never the size of one fetched page.
struct Playlist {
    CollectionRef ref;
    std::string name{};
    std::optional<std::string> creator{};
    std::optional<std::string> description{};
    Artwork artwork{};
    std::optional<std::uint64_t> track_count{};
    std::optional<std::uint64_t> play_count{};
    std::optional<std::string> updated_at{}; // as the platform reports it (ISO date or date-time)
    std::optional<std::string> relationship{}; // 个人目录中的 created / subscribed；公共目录未知
};

struct Album {
    CollectionRef ref;
    std::string title{};
    std::vector<ArtistSummary> artists{};
    std::optional<std::string> release_date{}; // YYYY, YYYY-MM or YYYY-MM-DD
    std::optional<std::uint64_t> track_count{};
    Artwork artwork{};
    std::optional<std::string> description{};
};

// An artist as a search result: the name and an id that its track and album lists take.
struct Artist {
    ArtistRef ref;
    std::string name{};
    Artwork artwork{};
    std::optional<std::string> description{};
    std::optional<std::string> alternate_name{};
    std::optional<std::string> country{};
    std::optional<std::string> birth_date{};
    std::optional<std::string> birthplace{};
    std::optional<std::string> gender{};
    std::optional<std::string> language{};
    std::optional<std::string> height{};
    std::optional<std::string> weight{};
    std::optional<std::string> constellation{};
    std::optional<std::uint64_t> track_count{};
    std::optional<std::uint64_t> album_count{};
    std::optional<std::uint64_t> mv_count{};
};

struct Podcast {
    CollectionRef ref;
    std::string title;
    std::optional<std::string> presenter;
    std::optional<std::string> description;
    Artwork artwork;
    std::optional<std::uint64_t> episode_count;
};

struct Episode {
    CollectionRef ref; // 节目 ID，不是音频 ID
    CollectionRef podcast;
    std::string title;
    std::string podcast_title;
    std::optional<std::string> presenter; // 主播独立展示，不映射成歌曲演唱者
    std::optional<std::string> description;
    Artwork artwork;
    std::optional<std::int64_t> duration_ms;
    std::optional<std::string> published_at;
    std::optional<TrackRef> audio_ref; // 服务端验证的稳定节目音频引用，可提交现有下载任务
    bool requires_purchase{false}; // 上游明确未购买；false 不保证可播放
};

// What a category or chart lists. Where the entry comes from (live, built-in or derived
// from search) is the capability's catalog_kind, not part of the entry.
enum class ListedKind { playlist, track, album, artist };

struct Category {
    std::string id{}; // platform category id, only meaningful with its source
    std::string name{};
    std::optional<std::string> group{};
    ListedKind lists{ListedKind::playlist};
};

struct Chart {
    CollectionRef ref;
    std::string name{};
    std::optional<std::string> description{};
    std::optional<std::string> update_hint{}; // e.g. "daily", as the platform labels it
    std::optional<std::string> updated_at{};
    std::optional<std::uint64_t> track_count{};
    ListedKind lists{ListedKind::track};
};

// Lyrics of one track. A known empty answer is a value with nothing set and unavailable
// true; a platform without lyrics is CAPABILITY_UNSUPPORTED instead.
struct KaraokeWord {
    std::int64_t start_ms{0};
    std::int64_t duration_ms{0};
    std::string text{};
};

struct KaraokeLine {
    std::int64_t start_ms{0};
    std::int64_t duration_ms{0};
    std::vector<KaraokeWord> words{};
};

struct TrackLyrics {
    std::optional<std::string> lrc{};            // LRC text, or plain text without times
    std::optional<std::string> translated_lrc{}; // a translation in LRC
    std::vector<KaraokeLine> karaoke{};          // word timing, milliseconds
    bool unavailable{false};                   // the platform has no lyrics for this track
    std::vector<std::string> warnings{};
    std::optional<std::string> romanized_lrc{};
};

} // namespace music_service::domain
