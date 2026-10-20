#include "test_support.hpp"

#include "domain/catalog/refs.hpp"

#include <string>

using namespace music_dl::domain;

TEST_CASE("source ids follow the open lowercase format", "[domain][refs]") {
    CHECK(SourceId::parse("kuwo"));
    CHECK(SourceId::parse("a"));
    CHECK(SourceId::parse("yt_music2"));
    CHECK(SourceId::parse(std::string(64, 'a')));

    CHECK_FALSE(SourceId::parse(""));
    CHECK_FALSE(SourceId::parse("Kuwo"));
    CHECK_FALSE(SourceId::parse("1kuwo"));
    CHECK_FALSE(SourceId::parse("ku-wo"));
    CHECK_FALSE(SourceId::parse(std::string(65, 'a')));

    const auto error = SourceId::parse("Bad").error();
    CHECK(error.code == ErrorCode::validation_failed);
    REQUIRE(error.details.size() == 1);
    CHECK(error.details[0].field == "source");
}

TEST_CASE("platform ids are opaque UTF-8 strings", "[domain][refs]") {
    CHECK(PlatformId::parse("MUSIC_123"));
    CHECK(PlatformId::parse("00123")); // leading zeros survive; never parsed as a number
    CHECK(PlatformId::parse("歌曲/id?x=1"));
    CHECK(PlatformId::parse(std::string(PlatformId::max_bytes, 'x')));

    CHECK_FALSE(PlatformId::parse(""));
    CHECK_FALSE(PlatformId::parse(std::string(PlatformId::max_bytes + 1, 'x')));
    CHECK_FALSE(PlatformId::parse("\xC3\x28"));     // broken sequence
    CHECK_FALSE(PlatformId::parse("\xC0\xAF"));     // overlong '/'
    CHECK_FALSE(PlatformId::parse("\xED\xA0\x80")); // UTF-16 surrogate

    CHECK(music_dl::test::unwrap(PlatformId::parse("00123")).value() == "00123");
}

TEST_CASE("track refs compare by source and id", "[domain][refs]") {
    using music_dl::test::track;
    CHECK(track("kuwo", "1") == track("kuwo", "1"));
    CHECK(track("kuwo", "1") != track("kuwo", "01"));
    CHECK(track("kuwo", "1") != track("qq", "1"));
}

TEST_CASE("service ids carry their type prefix", "[domain][refs]") {
    CHECK(is_valid_service_id("tsk_", "tsk_a1B2"));
    CHECK_FALSE(is_valid_service_id("tsk_", "tsk_"));
    CHECK_FALSE(is_valid_service_id("tsk_", "fil_a1"));
    CHECK_FALSE(is_valid_service_id("tsk_", "tsk_a-1"));
    CHECK_FALSE(is_valid_service_id("tsk_", "tsk_" + std::string(61, 'a')));
}
