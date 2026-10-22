#pragma once

#include "domain/catalog/collections.hpp"
#include "domain/catalog/track.hpp"
#include "domain/result.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// 5sing response parsing. Pure functions over response bodies; a changed or broken body
// is UPSTREAM_FAILED with details {"protocol", reason}, never an empty result.
namespace music_service::providers::fivesing::parsing {

// A 5sing work is identified by its type and number together: the same number names
// different works under yc (original), fc (cover) and bz (accompaniment). The platform
// id is "<type>-<number>", e.g. "fc-16394286".
struct WorkKey {
    std::string type; // yc, fc or bz
    std::string id;   // 1..12 decimal digits without a leading zero

    [[nodiscard]] std::string platform_id() const { return type + "-" + id; }
    friend bool operator==(const WorkKey&, const WorkKey&) = default;
};

// The key of a platform id; anything else is VALIDATION_FAILED.
domain::Result<WorkKey> parse_work_key(std::string_view platform_id);

struct SearchPage {
    std::vector<domain::Track> tracks;
    std::uint64_t total{0};
    std::uint32_t page{0}; // 1-based, as the upstream reports it
    std::uint32_t total_pages{0};
};

// search.5sing.kugou.com/home/json. Entries of an unknown work type are left out.
domain::Result<SearchPage> parse_search(std::string_view body);

// What song/newget says about one work. A missing work is NOT_FOUND; an answer about
// another work is a protocol error. Unknown fields stay empty; no duration is given.
struct SongInfo {
    domain::Track track;
    domain::TrackLyrics lyrics; // unavailable when the work has no timed lyrics
};

domain::Result<SongInfo> parse_song_info(std::string_view body, const WorkKey& key);

// One tier of song/getSongUrl: sq (320 kbit/s), hq (192 kbit/s) or lq (128 kbit/s),
// as the MP3 frame headers of the recorded samples show.
struct AudioTier {
    std::string name;
    std::string primary;
    std::string backup;
    std::string md5;
    std::optional<std::uint64_t> size;
    std::string ext;
};

// The tiers that have an address, highest first. A missing work is NOT_FOUND.
domain::Result<std::vector<AudioTier>> parse_song_urls(std::string_view body, const WorkKey& key);

// The key of a 5sing song link: 5sing.kugou.com/{yc|fc|bz}/{number}.html, http or https.
// Anything else is VALIDATION_FAILED. No network.
domain::Result<WorkKey> parse_track_link(std::string_view link);

// Decodes HTML character references, removes markup such as the search highlight
// <em class="keyword"> and trims surrounding whitespace.
[[nodiscard]] std::string clean_text(std::string_view text);

} // namespace music_service::providers::fivesing::parsing
