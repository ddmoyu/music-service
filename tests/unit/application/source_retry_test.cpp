#include "../domain/test_support.hpp"
#include "application/catalog/source_retry.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>

using namespace music_service;
using namespace music_service::test;
using namespace music_service::application::catalog;
namespace ports = music_service::application::ports;

TEST_CASE("strict source matching preserves complete identities and recording versions", "[source-retry]") {
    domain::Track a{track("qq", "1"), "稻香", {{"周杰伦", {}}, {"合唱者", {}}}, {}, 200000, {}};
    auto b = a;
    b.ref = track("kuwo", "2");
    SECTION("only whitespace and artist ordering are normalized") {
        b.title = "  稻香\t";
        std::reverse(b.artists.begin(), b.artists.end());
        b.duration_ms = 202000;
        CHECK(strict_track_match(a, b));
    }
    SECTION("different title") { b.title = "稻香 (Live)"; CHECK_FALSE(strict_track_match(a, b)); }
    SECTION("a subset of artists is insufficient") { b.artists.pop_back(); CHECK_FALSE(strict_track_match(a, b)); }
    SECTION("aliases are not guessed") { b.artists[0].name = "Jay Chou"; CHECK_FALSE(strict_track_match(a, b)); }
    SECTION("empty artists are rejected") { a.artists.clear(); b.artists.clear(); CHECK_FALSE(strict_track_match(a, b)); }
    SECTION("duplicate artists are ambiguous") { a.artists.push_back(a.artists.front()); CHECK_FALSE(strict_track_match(a, a)); }
    SECTION("version metadata is checked even with equal titles") { b.version.kind = domain::VersionKind::live; CHECK_FALSE(strict_track_match(a, b)); }
    SECTION("version labels are not stripped") { b.version.label = "伴奏"; CHECK_FALSE(strict_track_match(a, b)); }
    SECTION("different lengths are rejected") { b.duration_ms = 202001; CHECK_FALSE(strict_track_match(a, b)); }
    SECTION("unknown length does not invent a value") { b.duration_ms.reset(); CHECK(strict_track_match(a, b)); }
    SECTION("empty title cannot match") { a.title = " "; b.title = ""; CHECK_FALSE(strict_track_match(a, b)); }
}

TEST_CASE("source retry snapshots round trip and reject corrupt states", "[source-retry]") {
    SourceRetryState state{domain::Track{track("qq", "1"), "歌", {{"歌手", {}}}, {}, 200000, {}}, {"qq", "kuwo"}, true};
    auto doc = nlohmann::json{{"source_retry", nlohmann::json::parse(encode_source_retry(state))}};
    auto restored = unwrap(decode_source_retry(doc.dump(), track("qq", "1")));
    REQUIRE(restored.original);
    CHECK(strict_track_match(*state.original, *restored.original));
    CHECK(restored.attempted_sources == state.attempted_sources);
    CHECK(restored.pending);
    SECTION("unknown fields") { doc["source_retry"]["typo"] = 1; CHECK_FALSE(decode_source_retry(doc.dump(), track("qq", "1"))); }
    SECTION("duplicate sources") { doc["source_retry"]["attempted_sources"] = {"qq", "qq"}; CHECK_FALSE(decode_source_retry(doc.dump(), track("qq", "1"))); }
    SECTION("unsupported schema") { doc["source_retry"]["schema"] = 0; CHECK_FALSE(decode_source_retry(doc.dump(), track("qq", "1"))); }
}

TEST_CASE("source ranking uses evidence without counting local failures", "[source-retry]") {
    ports::SourceDownloadStatistics unknown{"new"}, one{"one", 1, 0}, good{"good", 90, 10};
    CHECK(unknown.score() == 0.5);
    CHECK(good.score() > one.score());
    for (const auto code : {domain::ErrorCode::canceled, domain::ErrorCode::service_busy,
        domain::ErrorCode::file_system, domain::ErrorCode::destination_unavailable, domain::ErrorCode::storage_unavailable})
        CHECK_FALSE(source_failure(code));
    CHECK(source_failure(domain::ErrorCode::audio_invalid));
    CHECK(source_failure(domain::ErrorCode::credentials_required));
}
