#pragma once

#include "domain/catalog/refs.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace music_service::domain {

struct ArtistSummary {
    std::string name;
    std::optional<ArtistRef> ref;
};

struct AlbumSummary {
    std::string title;
    std::optional<CollectionRef> ref;
};

enum class VersionKind { studio, live, remix, cover, instrumental, unknown };

struct VersionInfo {
    VersionKind kind{VersionKind::unknown};
    std::optional<std::string> label;
    std::vector<std::string> evidence;
};

// Platform track metadata. Unknown values stay empty; they are never guessed.
struct Track {
    TrackRef ref;
    std::string title;
    std::vector<ArtistSummary> artists;
    std::optional<AlbumSummary> album;
    std::optional<std::int64_t> duration_ms;
    VersionInfo version;
};

// One source's page. total and has_more may be unknown.
struct PageInfo {
    std::uint32_t limit{0};
    std::uint32_t returned{0};
    std::optional<std::uint64_t> total;
    std::optional<bool> has_more;
};

template <class Item> struct SourcePage {
    std::vector<Item> items;
    PageInfo page;
};

} // namespace music_service::domain
