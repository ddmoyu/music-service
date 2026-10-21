#include "domain/assets/container.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace music_service::domain;

namespace {

ContentSniff sniff(std::string bytes) { return sniff_content(bytes); }

} // namespace

TEST_CASE("audio containers are recognized from their leading bytes", "[domain][container]") {
    CHECK(sniff("ID3\x04\x00").container == Container::mp3);
    CHECK(sniff(std::string{"\xFF\xFB\x90\x00", 4}).container == Container::mp3);  // MPEG-1 layer III
    CHECK(sniff(std::string{"\xFF\xF3\x80\x00", 4}).container == Container::mp3);  // MPEG-2 layer III
    CHECK(sniff("fLaC\x00\x00\x00\x22").container == Container::flac);
    CHECK(sniff("OggS\x00\x02").container == Container::ogg);
    CHECK(sniff(std::string{"\x00\x00\x00\x20" "ftypM4A ", 12}).container == Container::mp4);
    CHECK(sniff(std::string{"RIFF\x24\x00\x00\x00WAVE", 12}).container == Container::wav);
    CHECK(to_string(Container::mp4) == "m4a");
}

TEST_CASE("ADTS and invalid MPEG headers are not taken for MP3", "[domain][container]") {
    // The ADTS sync has zero layer bits, which MPEG audio reserves.
    const auto adts = sniff(std::string{"\xFF\xF1\x50\x80", 4});
    CHECK(adts.container == Container::aac_adts);
    // A reserved sample rate (index 3) or bitrate 15 is not a frame.
    CHECK(sniff(std::string{"\xFF\xFB\x9C\x00", 4}).kind == ContentKind::unknown);
    CHECK(sniff(std::string{"\xFF\xFB\xF0\x00", 4}).kind == ContentKind::unknown);
}

TEST_CASE("upstream error bodies are recognized as text, not audio", "[domain][container]") {
    CHECK(sniff("<!DOCTYPE html><html>").kind == ContentKind::markup);
    CHECK(sniff("\xEF\xBB\xBF  \r\n<?xml version=\"1.0\"?>").kind == ContentKind::markup);
    CHECK(sniff("{\"code\":403,\"msg\":\"forbidden\"}").kind == ContentKind::json);
    CHECK(sniff("  [1,2]").kind == ContentKind::json);
    CHECK(sniff("Forbidden\r\n").kind == ContentKind::text);
    CHECK(sniff("").kind == ContentKind::empty);
    CHECK(sniff(std::string{"\x00\x01\x02\x03\x04", 5}).kind == ContentKind::unknown);
    CHECK_FALSE(sniff("<html>").container);
}
