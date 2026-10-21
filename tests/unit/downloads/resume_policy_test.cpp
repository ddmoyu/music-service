// Resume decisions for partial files and the publication helpers: candidate names and
// the evidence record recovery relies on.

#include "downloads/finalizer.hpp"
#include "downloads/resume_policy.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace music_service::downloads;

namespace {

ResourceIdentity resource() {
    return ResourceIdentity{"kuwo:1", "standard", 1000, std::string{"\"abc\""}, std::string{"Wed, 21 Oct 2026 07:28:00 GMT"},
                            std::nullopt};
}

PartialFile partial(std::uint64_t file_bytes, std::uint64_t durable_bytes) {
    return PartialFile{file_bytes, durable_bytes, resource()};
}

} // namespace

TEST_CASE("a proven partial file resumes from its durable checkpoint", "[downloads][resume]") {
    const auto decision = decide_resume(partial(640, 600), resource());
    CHECK(decision.resume);
    CHECK(decision.offset == 600); // the unflushed tail beyond the checkpoint is cut
    CHECK(decision.if_range == "\"abc\"");

    const auto whole = decide_resume(partial(1000, 1000), resource());
    CHECK(whole.resume);
    CHECK(whole.reason == "complete");
}

TEST_CASE("without proof the download starts over instead of appending", "[downloads][resume]") {
    CHECK(decide_resume(std::nullopt, resource()).reason == "no_partial");
    CHECK(decide_resume(partial(0, 0), resource()).reason == "no_partial");
    CHECK(decide_resume(partial(500, 600), resource()).reason == "shorter_than_checkpoint");

    auto other_quality = resource();
    other_quality.quality = "high";
    CHECK(decide_resume(partial(600, 600), other_quality).reason == "resource_changed");
    auto other_track = resource();
    other_track.track = "kuwo:2";
    CHECK(decide_resume(partial(600, 600), other_track).reason == "resource_changed");

    auto unknown_size = resource();
    unknown_size.total_bytes.reset();
    CHECK(decide_resume(partial(600, 600), unknown_size).reason == "size_unknown");
    auto other_size = resource();
    other_size.total_bytes = 1200;
    CHECK(decide_resume(partial(600, 600), other_size).reason == "size_changed");

    auto new_tag = resource();
    new_tag.etag = "\"def\"";
    CHECK(decide_resume(partial(600, 600), new_tag).reason == "etag_changed");

    // A weak tag proves nothing; neither do a missing date and hash.
    auto weak = resource();
    weak.etag = "W/\"abc\"";
    weak.last_modified.reset();
    PartialFile weak_partial{600, 600, weak};
    CHECK(decide_resume(weak_partial, weak).reason == "no_validator");
    for (const auto& decision : {decide_resume(partial(600, 600), other_quality), decide_resume(weak_partial, weak)}) {
        CHECK_FALSE(decision.resume);
        CHECK(decision.offset == 0);
    }
}

TEST_CASE("other validators resume when no strong tag is available", "[downloads][resume]") {
    auto dated = resource();
    dated.etag.reset();
    const auto by_date = decide_resume(PartialFile{600, 600, dated}, dated);
    CHECK(by_date.resume);
    CHECK(by_date.if_range == dated.last_modified);

    auto hashed = dated;
    hashed.last_modified.reset();
    hashed.content_hash = "md5:0123";
    const auto by_hash = decide_resume(PartialFile{600, 600, hashed}, hashed);
    CHECK(by_hash.resume);
    CHECK_FALSE(by_hash.if_range);

    // A known hash that changed is other bytes: an equal date or tag does not resume them.
    auto rehashed = resource();
    rehashed.etag.reset();
    rehashed.content_hash = "md5:0123";
    auto replaced = rehashed;
    replaced.content_hash = "md5:4567";
    const auto by_date_only = decide_resume(PartialFile{600, 600, rehashed}, replaced);
    CHECK_FALSE(by_date_only.resume);
    CHECK(by_date_only.reason == "content_hash_changed");
    auto tagged = resource();
    tagged.content_hash = "md5:0123";
    auto tagged_replaced = tagged;
    tagged_replaced.content_hash = "md5:4567";
    CHECK(decide_resume(PartialFile{600, 600, tagged}, tagged_replaced).reason == "content_hash_changed");
}

TEST_CASE("candidate names are deterministic and keep the extension", "[downloads][publication]") {
    CHECK(candidate_path("歌手/Song.mp3", 1) == "歌手/Song.mp3");
    CHECK(candidate_path("歌手/Song.mp3", 2) == "歌手/Song (2).mp3");
    CHECK(candidate_path("a.b/Track", 3) == "a.b/Track (3)");
    CHECK(candidate_path(".hidden", 2) == ".hidden (2)");
    CHECK(candidate_path("Album/Song.Live.flac", 10) == "Album/Song.Live (10).flac");
}

TEST_CASE("publication evidence round-trips and rejects other records", "[downloads][publication]") {
    const PublicationEvidence evidence{{42, "ab12"}, "歌手/Song.mp3", "mp3", R"({"quality":"standard"})",
                                       R"({"metadata_status":"skipped"})"};
    const auto decoded = decode_evidence(encode_evidence(evidence));
    REQUIRE(decoded);
    CHECK(decoded->file == evidence.file);
    CHECK(decoded->wanted_path == evidence.wanted_path);
    CHECK(decoded->container == "mp3");
    CHECK(decoded->properties_json == evidence.properties_json);
    CHECK(decoded->result_json == evidence.result_json);
    for (const auto* bad : {"", "[]", R"({"size":3})", R"({"size":-1,"sha256":"x","wanted_path":"a","container":"",)"
                                                       R"("properties":{},"result":{}})"}) {
        CHECK_FALSE(decode_evidence(bad));
    }
    CHECK(file_etag({1, "0123456789abcdef0123456789abcdef0123"}) == "\"sha256-0123456789abcdef0123456789abcdef\"");
}
