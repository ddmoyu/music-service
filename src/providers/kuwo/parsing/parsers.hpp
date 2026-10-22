#pragma once

#include "domain/catalog/track.hpp"
#include "domain/result.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Kuwo response parsing. Pure functions over response bodies; a changed or broken
// body is UPSTREAM_FAILED with details {"protocol", reason}, never an empty result.
namespace music_service::providers::kuwo::parsing {

// Kuwo song ids ("rid") are 1..18 decimal digits without a leading zero.
[[nodiscard]] bool is_valid_rid(std::string_view rid) noexcept;

struct SearchPage {
    std::vector<domain::Track> tracks;
    std::uint64_t total{0};
    std::uint32_t page{0};
};

// searchMusicBykeyWord JSON.
domain::Result<SearchPage> parse_search(std::string_view body);

// The play_detail page of rid. A page without song information is NOT_FOUND; a page
// about another song is a protocol error. Unknown fields stay empty.
domain::Result<domain::Track> parse_track_page(std::string_view html, std::string_view rid);

// What mobi.s says about the audio for the requested rid.
struct AudioEvidence {
    std::string url;      // always https on a kuwo.cn host
    std::string format;   // e.g. "mp3"
    std::optional<std::int64_t> bitrate_kbps;
    std::optional<std::int64_t> duration_seconds;
    // The address belongs to another rid or is marked as a substitute (type 1): Kuwo
    // hands out a short clip instead of the song.
    bool substitute{false};
    // Same rid and type 0: the upstream marks the address as the song itself. A
    // missing or unreadable type leaves this false without making it a substitute.
    bool regular{false};
};

domain::Result<AudioEvidence> parse_audio(std::string_view body, std::string_view requested_rid);

// The rid of a Kuwo song link: www.kuwo.cn/play_detail/{rid} or
// m.kuwo.cn/newh5app/play_detail/{rid}, http or https. Anything else is
// VALIDATION_FAILED. No network.
domain::Result<std::string> parse_track_link(std::string_view link);
struct Link { std::string kind; std::string id; };
domain::Result<Link> parse_link(std::string_view link);

// Artists from Kuwo's '&'-joined names and ids; ids line up only when the counts match.
[[nodiscard]] std::vector<domain::ArtistSummary> parse_artists(std::string_view names, std::string_view ids);

// Decodes the HTML character references Kuwo uses (&amp; &lt; &gt; &quot; &#39;
// &apos; &nbsp; and numeric forms) and trims surrounding whitespace.
[[nodiscard]] std::string decode_text(std::string_view text);

} // namespace music_service::providers::kuwo::parsing
