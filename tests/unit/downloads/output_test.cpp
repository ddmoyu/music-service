#include "../domain/test_support.hpp"
#include "downloads/metadata_support.hpp"
#include "infrastructure/filesystem/paths.hpp"

#include <filesystem>
#include <fstream>

using namespace music_service;
using namespace music_service::test;

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

TEST_CASE("translated lyrics preserve both time lines and absent lyrics stay empty", "[output]") {
    domain::TrackLyrics l;
    l.lrc = "[00:01.00]original";
    l.translated_lrc = "[00:01.25]译文";
    CHECK(downloads::embedded_lyrics(l) == "[00:01.00]original\n\n[translation]\n[00:01.25]译文");
    l.unavailable = true;
    CHECK(downloads::embedded_lyrics(l).empty());
}
