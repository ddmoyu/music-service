#include "../domain/test_support.hpp"
#include "downloads/metadata_support.hpp"
#include "infrastructure/filesystem/paths.hpp"

#include <filesystem>
#include <fstream>

using namespace music_service;
using namespace music_service::test;

TEST_CASE("published quality cannot exceed platform or byte evidence", "[output][stage-e]") {
    application::ports::AudioFacts facts;
    facts.container = "flac";
    facts.bits_per_sample = 24;
    facts.sample_rate_hz = 44100;
    CHECK(unwrap(downloads::verified_quality(domain::Quality::hires, facts)) == domain::Quality::lossless);
    facts.sample_rate_hz = 96000;
    CHECK(unwrap(downloads::verified_quality(domain::Quality::hires, facts)) == domain::Quality::hires);
    CHECK(unwrap(downloads::verified_quality(domain::Quality::lossless, facts)) == domain::Quality::lossless);
    facts.bits_per_sample = 16;
    CHECK(unwrap(downloads::verified_quality(domain::Quality::hires, facts)) == domain::Quality::lossless);
    facts.container = "mp3"; facts.bitrate_bps = 128000;
    CHECK(unwrap(downloads::verified_quality(domain::Quality::high, facts)) == domain::Quality::standard);
    facts.bitrate_bps = 320000;
    CHECK(unwrap(downloads::verified_quality(domain::Quality::high, facts)) == domain::Quality::high);
    CHECK_FALSE(unwrap(downloads::verified_quality(std::nullopt, facts)));
    facts.container = "unknown";
    CHECK_FALSE(downloads::verified_quality(domain::Quality::hires, facts));
}

TEST_CASE("output names sanitize every component and bound UTF-8 paths", "[output]") {
    const auto sanitize = [](std::string_view s) { return infrastructure::filesystem::sanitize_file_name(s); };
    domain::Track item{track("src", "1"), "晴天", {{"歌手甲", {}}, {"歌手乙", {}}}, domain::AlbumSummary{"专辑", {}}, {}, {}};
    domain::OutputOptions o;
    CHECK(downloads::output_name(item, o, sanitize) == "歌手甲, 歌手乙 - 晴天");
    o.name_format = "title";
    o.folder_layout = "artist_album";
    CHECK(downloads::output_name(item, o, sanitize) == "歌手甲, 歌手乙/专辑/晴天");
    o.name_format = "title_artist";
    o.folder_layout = "artist";
    CHECK(downloads::output_name(item, o, sanitize) == "歌手甲, 歌手乙/晴天 - 歌手甲, 歌手乙");
    o.folder_layout = "artist_album";
    item.title = "../CON";
    item.artists = {{"../../NUL", {}}};
    item.album = domain::AlbumSummary{"..", {}};
    const auto safe = downloads::output_name(item, o, sanitize);
    const auto path = infrastructure::filesystem::path_from_utf8(safe);
    CHECK_FALSE(path.is_absolute());
    for (const auto& part : path) {
        CHECK(part != "..");
        CHECK(part != ".");
    }
    item.title.clear();
    item.artists.clear();
    item.album.reset();
    CHECK(downloads::output_name(item, o, sanitize) == "未知歌手/未知专辑/src-1");
    item.title = "CON";
    o.name_format = "title";
    CHECK(downloads::output_name(item, o, sanitize) != "未知歌手/未知专辑/CON");
    for (int n = 0; n < 80; ++n) item.title += "中文";
    item.artists = {{item.title, {}}};
    item.album = domain::AlbumSummary{item.title, {}};
    const auto long_name = downloads::output_name(item, o, sanitize);
    CHECK(long_name.size() <= 185);
    CHECK(infrastructure::filesystem::path_to_utf8(infrastructure::filesystem::path_from_utf8(long_name)) == long_name);
}

TEST_CASE("covers reject error bodies and truncated structures", "[output]") {
    std::ifstream file(std::filesystem::path{MUSIC_SERVICE_TEST_FIXTURES} / "audio/cover.png", std::ios::binary);
    const std::string png{std::istreambuf_iterator<char>(file), {}};
    REQUIRE(downloads::picture_mime(png) == "image/png");
    CHECK_FALSE(downloads::picture_mime(png.substr(0, png.size() - 1)));
    CHECK_FALSE(downloads::picture_mime(png.substr(0, 8)));
    CHECK_FALSE(downloads::picture_mime(png + "extra"));
    CHECK_FALSE(downloads::picture_mime("<html>error</html>"));
    CHECK_FALSE(downloads::picture_mime("{\"error\":404}"));
    CHECK_FALSE(downloads::picture_mime(std::string{"\xff\xd8\xff\xd9", 4}));
}

TEST_CASE("static WebP cover structures reject truncated animated and oversized chunks", "[output][migu]") {
    const auto le32 = [](std::uint32_t value) {
        std::string out;
        for (int i = 0; i < 4; ++i) out += static_cast<char>((value >> (i*8)) & 255U);
        return out;
    };
    const auto wrap = [&](const std::string& chunks) { return "RIFF" + le32(static_cast<std::uint32_t>(chunks.size()+4)) + "WEBP" + chunks; };
    const auto vp8 = std::string{"\x10\0\0\x9d\x01\x2a\x01\0\x01\0\x01\0",12};
    const auto chunk = "VP8 " + le32(12) + vp8;
    const auto picture = wrap(chunk);
    REQUIRE(downloads::picture_mime(picture) == "image/webp");
    CHECK_FALSE(downloads::picture_mime(picture.substr(0,picture.size()-1)));
    CHECK_FALSE(downloads::picture_mime(picture + "extra"));
    CHECK_FALSE(downloads::picture_mime(wrap("VP8 " + le32(9999) + vp8)));
    CHECK_FALSE(downloads::picture_mime(wrap(chunk + chunk)));
    CHECK_FALSE(downloads::picture_mime(wrap("ANIM" + le32(0) + chunk)));
    auto zero_width = picture; zero_width[26] = 0; CHECK_FALSE(downloads::picture_mime(zero_width));
    auto animated = wrap("VP8X" + le32(10) + std::string(10,'\0') + chunk);
    REQUIRE(downloads::picture_mime(animated) == "image/webp");
    animated[20] = 2; CHECK_FALSE(downloads::picture_mime(animated));
    const auto vp8l = std::string{"\x2f\0\0\0\0\x01",6};
    CHECK(downloads::picture_mime(wrap("VP8L" + le32(6) + vp8l)) == "image/webp");
}

TEST_CASE("translated lyrics preserve both time lines and absent lyrics stay empty", "[output]") {
    domain::TrackLyrics l;
    l.lrc = "[00:01.00]original";
    l.translated_lrc = "[00:01.25]译文";
    CHECK(downloads::embedded_lyrics(l) == "[00:01.00]original\n\n[translation]\n[00:01.25]译文");
    l.unavailable = true;
    CHECK(downloads::embedded_lyrics(l).empty());
}

TEST_CASE("lyric variants preserve timestamps and fall back without inventing word timing", "[output][stage-d]") {
    domain::TrackLyrics l;
    l.lrc = "[00:01.00]原文";
    l.translated_lrc = "[00:01.000]translation\n[00:09.50]unmatched";
    l.romanized_lrc = "[00:01.20]romaji";
    l.karaoke = {{1000, 1000, {{1000, 400, "原"}, {1400, 600, "文"}}}};
    CHECK(downloads::embedded_lyrics(l, "verbatim") ==
        "[00:01.000]<00:01.000>原<00:01.400>文<00:02.000>\n[00:01.000]translation\n\n\n[translation]\n[00:09.50]unmatched\n");
    CHECK(downloads::embedded_lyrics(l, "romanized").starts_with("[00:01.20]romaji"));
    l.karaoke.clear();
    l.romanized_lrc.reset();
    CHECK(downloads::embedded_lyrics(l, "verbatim") == downloads::embedded_lyrics(l));
    CHECK(downloads::embedded_lyrics(l, "romanized") == downloads::embedded_lyrics(l));
}
