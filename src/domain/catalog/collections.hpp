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
};

struct Album {
    CollectionRef ref;
    std::string title{};
    std::vector<ArtistSummary> artists{};
    std::optional<std::string> release_date{}; // YYYY, YYYY-MM or YYYY-MM-DD
    std::optional<std::uint64_t> track_count{};
    Artwork artwork{};
};

// An artist as a search result: the name and an id that its track and album lists take.
struct Artist {
    ArtistRef ref;
    std::string name{};
    Artwork artwork{};
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
};

} // namespace music_service::domain
