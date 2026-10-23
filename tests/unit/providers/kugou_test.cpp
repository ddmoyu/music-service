// 合成协议响应；无账号、访问密钥或在线请求。
#include "../domain/test_support.hpp"
#include "application/transport/http_rules.hpp"
#include "providers/kugou/artists.hpp"
#include "providers/kugou/bundle.hpp"
#include "providers/kugou/parsing.hpp"
#include "providers/registry/provider_registry.hpp"
#include <catch2/catch_test_macros.hpp>
#include <deque>
using namespace music_service;
using namespace music_service::test;
namespace ports = application::ports;
namespace kg = providers::kugou;
using Json = nlohmann::json;
using domain::ErrorCode;
namespace {
constexpr auto song = "11111111111111111111111111111111";
constexpr auto high = "22222222222222222222222222222222";
constexpr auto lossless = "33333333333333333333333333333333";
class FakeUpstream final : public ports::UpstreamHttp {
  public:
    std::deque<ports::HttpResponse> answers;
    std::vector<ports::HttpRequest> calls;
    bool deferred{false};
    void raw(Json j, int status = 200) { answers.push_back({{status, {}, {}}, j.dump()}); }
    void api(Json data) { raw({{"status", 1}, {"errcode", 0}, {"data", std::move(data)}}); }
    domain::Result<ports::HopResult<ports::HttpResponse>>
    fetch(ports::HttpRequest request, const ports::AdmissionRequest& base,
          const ports::OperationContext&) override {
        REQUIRE(application::transport::check_request(request));
        CHECK(base.source.value() == "kugou");
        CHECK(base.operation == request.operation);
        CHECK(request.secret_headers.empty());
        calls.push_back(request);
        if (deferred)
            return ports::HopResult<ports::HttpResponse>{ports::PendingHop{
                std::move(request), {"source", std::chrono::steady_clock::now()}}};
        REQUIRE_FALSE(answers.empty());
        auto result = std::move(answers.front());
        answers.pop_front();
        return ports::HopResult<ports::HttpResponse>{std::move(result)};
    }
    domain::Result<ports::HopResult<ports::HttpResponseHead>>
    stream(ports::HttpRequest, const ports::AdmissionRequest&, const ports::OperationContext&,
           const ports::HeadConsumer&, const ports::ChunkConsumer&) override {
        return domain::Error{ErrorCode::internal, "unexpected stream", false, {}};
    }
};
ports::ProviderContext context() {
    return {{std::chrono::steady_clock::now() + std::chrono::seconds(10), {}},
            unwrap(domain::AuthIdentityKey::make(anonymous("prn_a", "kugou"), {})),
            {},
            {}};
}
struct Fixture {
    std::shared_ptr<FakeUpstream> upstream = std::make_shared<FakeUpstream>();
    std::shared_ptr<const providers::ProviderRegistry> registry =
        unwrap(providers::ProviderRegistry::build({kg::make_bundle()}, {}, {upstream}));
    template <class Port> std::shared_ptr<Port> port() {
        return unwrap(registry->require<Port>("kugou"));
    }
};
Json track_json(const char* id = song) {
    return {{"hash", id},
            {"filename", "歌手 - 中文歌曲"},
            {"duration", 180},
            {"album_id", "100"},
            {"trans_param",
             {{"union_cover", "http://imge.kugou.com/stdmusic/{size}/cover.jpg"}, {"sort", 2}}}};
}
Json playlist_json(int id = 100) {
    return {{"specialid", id},
            {"specialname", "歌单"},
            {"songcount", 3},
            {"publishtime", "2026-10-23"}};
}
Json album_json() { return {{"albumid", 100}, {"albumname", "专辑"}, {"songcount", 3}}; }
Json info(const char* id = song, int bitrate = 128, std::string ext = "mp3") {
    return {{"hash", id},
            {"req_hash", id},
            {"status", 1},
            {"errcode", 0},
            {"songName", "中文歌曲"},
            {"author_name", "歌手"},
            {"albumid", 0},
            {"timeLength", 180},
            {"fileSize", 2880000},
            {"bitRate", bitrate},
            {"extName", ext},
            {"fileHead", 0},
            {"pay_type", 0},
            {"url", "https://sharefs.kugou.com/audio." + ext},
            {"extra",
             {{"128hash", song},
              {"320hash", high},
              {"sqhash", lossless},
              {"128timelength", 180000},
              {"320timelength", 180000},
              {"sqtimelength", 180000}}}};
}
Json search_data(Json list, int page = 1, int limit = 1, int total = 3) {
    return {{"lists", list}, {"page", page}, {"pagesize", limit}, {"total", total}};
}
domain::Result<ports::ResolvedAudio>
resolve(Fixture& f, domain::Quality quality = domain::Quality::standard,
        domain::QualityPolicy policy = domain::QualityPolicy::strict, bool preview = false) {
    return f.port<ports::IAssetResolver>()->resolve_audio(context(), kg::parsing::ref(song, true),
                                                          {quality, policy, preview});
}
} // namespace
TEST_CASE("Kugou registers only implemented anonymous native capabilities", "[providers][kugou]") {
    Fixture f;
    auto view = unwrap(f.registry->describe("kugou"));
    CHECK(view.capabilities.size() == 21);
    CHECK(view.auth_methods.empty());
    CHECK(f.registry->instantiated() == 0);
    for (const auto& v : view.capabilities)
        CHECK(v.spec.credentials == ports::CredentialRequirement::none);
    CHECK(f.registry->require<ports::ArtistSearch>("kugou"));
    CHECK(f.registry->require<ports::ArtistDetail>("kugou"));
    CHECK(f.registry->require<ports::ArtistTracks>("kugou"));
    CHECK(f.registry->require<ports::ArtistAlbums>("kugou"));
    CHECK(f.upstream->calls.empty());
}
TEST_CASE("Kugou search preserves identity and advances the upstream window",
          "[providers][kugou]") {
    Fixture f;
    SECTION("tracks") {
        f.upstream->api(search_data(Json::array({track_json()}), 2));
        auto out =
            unwrap(f.port<ports::ITrackSearch>()->search_tracks(context(), {"中文", 1, "1"}));
        CHECK(out.next_cursor == "2");
        CHECK(out.page.items.front().ref.id.value() == song);
        CHECK(out.page.items.front().title == "中文歌曲");
        CHECK(f.upstream->calls.back().url.find("page=2&pagesize=1") != std::string::npos);
    }
    SECTION("playlists") {
        f.upstream->api({{"info", Json::array({playlist_json()})}, {"total", 3}});
        CHECK(unwrap(f.port<ports::PlaylistSearch>()->search(context(), {"中文", 1, {}}))
                  .page.items.front()
                  .name == "歌单");
    }
    SECTION("albums") {
        f.upstream->api({{"info", Json::array({album_json()})}, {"total", 3}});
        CHECK(unwrap(f.port<ports::AlbumSearch>()->search(context(), {"中文", 1, {}}))
                  .page.items.front()
                  .title == "专辑");
    }
}
TEST_CASE("Kugou empty search and filtered short pages retain their semantics",
          "[providers][kugou]") {
    Fixture f;
    SECTION("empty") {
        f.upstream->api(search_data(Json::array(), 1, 1, 0));
        auto out =
            unwrap(f.port<ports::ITrackSearch>()->search_tracks(context(), {"unmatched", 1, {}}));
        CHECK(out.page.items.empty());
        CHECK_FALSE(out.next_cursor);
        CHECK(out.page.page.total == 0);
    }
    SECTION("filtered") {
        f.upstream->api(search_data(Json::array({track_json()}), 1, 2, 10));
        auto out = unwrap(f.port<ports::ITrackSearch>()->search_tracks(context(), {"中文", 2, {}}));
        CHECK(out.next_cursor == "2");
        CHECK_FALSE(out.page.page.unavailable_count);
    }
}
TEST_CASE("Kugou validates paging and query before network access", "[providers][kugou]") {
    Fixture f;
    for (const ports::SearchQuery q : {ports::SearchQuery{"", 1, {}},
                                       {"x", 51, {}},
                                       {"x", 2, "1"},
                                       {"x", 1, "10001"},
                                       {"x", 1, "0:bad"}})
        CHECK_FALSE(f.port<ports::ITrackSearch>()->search_tracks(context(), q));
    CHECK(f.upstream->calls.empty());
}
TEST_CASE("Kugou rejects malformed success and upstream page identity", "[providers][kugou]") {
    Fixture f;
    SECTION("wrong page") { f.upstream->api(search_data(Json::array({track_json()}), 2)); }
    SECTION("missing status") {
        f.upstream->raw({{"data", search_data(Json::array({track_json()}))}});
    }
    SECTION("bad hash") {
        auto j = track_json();
        j["hash"] = "bad";
        f.upstream->api(search_data(Json::array({j})));
    }
    auto result = f.port<ports::ITrackSearch>()->search_tracks(context(), {"x", 1, {}});
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::upstream_failed);
}
TEST_CASE("Kugou keeps admission and throttling errors explicit", "[providers][kugou]") {
    Fixture f;
    auto expected = ErrorCode::upstream_rate_limited;
    SECTION("HTTP throttle") { f.upstream->raw(Json::object(), 429); }
    SECTION("API throttle") { f.upstream->raw({{"status", 0}, {"errcode", 1002}}); }
    SECTION("admission") {
        f.upstream->deferred = true;
        expected = ErrorCode::service_busy;
    }
    auto result = f.port<ports::ITrackSearch>()->search_tracks(context(), {"x", 1, {}});
    REQUIRE_FALSE(result);
    CHECK(result.error().code == expected);
    CHECK(result.error().retryable);
}
TEST_CASE("Kugou playlist pages preserve duplicates and missing hash positions",
          "[providers][kugou]") {
    Fixture f;
    f.upstream->api(playlist_json());
    auto missing = track_json();
    missing["hash"] = "";
    f.upstream->api({{"info", Json::array({track_json(), track_json(), missing})}, {"total", 3}});
    auto out =
        unwrap(f.port<ports::PlaylistTracks>()->list(context(), kg::parsing::ref("100"), {3, {}}));
    REQUIRE(out.page.items.size() == 2);
    CHECK(out.page.items[0].ref == out.page.items[1].ref);
    CHECK(out.page.page.unavailable_count == 1);
    CHECK(out.page.page.total == 3);
    CHECK_FALSE(out.next_cursor);
}
TEST_CASE("Kugou album track number comes from album fields", "[providers][kugou]") {
    Fixture f;
    f.upstream->api(album_json());
    f.upstream->api({{"info", Json::array({track_json()})}, {"total", 3}});
    auto out =
        unwrap(f.port<ports::AlbumTracks>()->list(context(), kg::parsing::ref("100"), {1, {}}));
    CHECK(out.page.items.front().track_number == 2);
    CHECK(out.page.items.front().album->title == "专辑");
    CHECK_FALSE(kg::parsing::track(track_json()).track_number);
}
TEST_CASE("Kugou collection changes expire cursors", "[providers][kugou]") {
    Fixture f;
    f.upstream->api(playlist_json());
    f.upstream->api({{"info", Json::array({track_json()})}, {"total", 3}});
    auto first =
        unwrap(f.port<ports::PlaylistTracks>()->list(context(), kg::parsing::ref("100"), {1, {}}));
    f.upstream->api(playlist_json());
    f.upstream->api({{"info", Json::array({track_json()})}, {"total", 4}});
    auto next = f.port<ports::PlaylistTracks>()->list(context(), kg::parsing::ref("100"),
                                                      {1, first.next_cursor});
    REQUIRE_FALSE(next);
    CHECK(next.error().code == ErrorCode::cursor_expired);
}
TEST_CASE("Kugou entity detail checks identity and missing objects", "[providers][kugou]") {
    Fixture f;
    auto expected = ErrorCode::upstream_failed;
    SECTION("wrong identity") { f.upstream->api(playlist_json(101)); }
    SECTION("not found") {
        f.upstream->api(nullptr);
        expected = ErrorCode::not_found;
    }
    auto result = f.port<ports::PlaylistDetail>()->detail(context(), kg::parsing::ref("100"));
    REQUIRE_FALSE(result);
    CHECK(result.error().code == expected);
}
TEST_CASE("Kugou recommendation pagination crosses fixed thirty item windows",
          "[providers][kugou]") {
    Fixture f;
    Json a = Json::array(), b = Json::array();
    for (int i = 0; i < 30; ++i)
        a.push_back(playlist_json(100 + i));
    for (int i = 0; i < 10; ++i)
        b.push_back(playlist_json(130 + i));
    auto doc = [](Json list) {
        return Json{{"plist", {{"pagesize", 30}, {"list", {{"total", 40}, {"info", list}}}}}};
    };
    f.upstream->raw(doc(a));
    auto first =
        unwrap(f.port<ports::IRecommendedPlaylists>()->recommend_playlists(context(), {20, {}}));
    f.upstream->raw(doc(a));
    f.upstream->raw(doc(b));
    auto second = unwrap(f.port<ports::IRecommendedPlaylists>()->recommend_playlists(
        context(), {20, first.next_cursor}));
    REQUIRE(second.page.items.size() == 20);
    CHECK(second.page.items.front().ref.id.value() == "120");
    CHECK(second.page.items.back().ref.id.value() == "139");
    CHECK_FALSE(second.next_cursor);
    CHECK(f.upstream->calls.back().url.ends_with("page=2"));
}
TEST_CASE("Kugou category tags round trip into category playlist requests", "[providers][kugou]") {
    Fixture f;
    f.upstream->api({{"info", Json::array({{{"id", 11},
                                            {"name", "风格"},
                                            {"children", Json::array({{{"id", 12},
                                                                       {"special_tag_id", 99},
                                                                       {"name", "民谣"}}})}}})}});
    auto categories = unwrap(f.port<ports::ICategories>()->categories(context()));
    REQUIRE(categories.size() == 2);
    CHECK(categories[1].id == "12:99");
    f.upstream->api({{"info", Json::array({playlist_json()})}, {"total", 1}});
    auto ref = domain::PlatformRef{unwrap(domain::SourceId::parse("kugou")),
                                   unwrap(domain::PlatformId::parse(categories[1].id))};
    CHECK(f.port<ports::CategoryPlaylists>()->list(context(), ref, {1, {}}));
    CHECK(f.upstream->calls.back().url.find("id=12&tagid=99") != std::string::npos);
}
TEST_CASE("Kugou chart directory and unknown chart remain distinguishable", "[providers][kugou]") {
    Fixture f;
    auto charts = Json{{"total", 2},
                       {"info", Json::array({{{"rankid", 1}, {"rankname", "榜一"}},
                                             {{"rankid", 2}, {"rankname", "榜二"}}})}};
    f.upstream->api(charts);
    CHECK(unwrap(f.port<ports::ICharts>()->charts(context())).size() == 2);
    f.upstream->api(charts);
    auto missing = f.port<ports::ChartDetail>()->detail(context(), kg::parsing::ref("3"));
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == ErrorCode::not_found);
}
TEST_CASE("Kugou track details preserve unknown album and reject singer artwork",
          "[providers][kugou]") {
    Fixture f;
    auto j = info();
    j["imgUrl"] = "https://imge.kugou.com/singer/avatar.jpg";
    f.upstream->raw(j);
    auto result = unwrap(
        f.port<ports::ITrackDetail>()->track_detail(context(), kg::parsing::ref(song, true)));
    CHECK_FALSE(result.artwork);
    CHECK_FALSE(result.album);
    CHECK(result.duration_ms == 180000);
}
TEST_CASE("Kugou audio selects the official hash for each requested quality",
          "[providers][kugou]") {
    Fixture f;
    f.upstream->raw(info());
    auto quality = domain::Quality::standard;
    SECTION("standard") {}
    SECTION("high") {
        quality = domain::Quality::high;
        f.upstream->raw(info(high, 320));
    }
    SECTION("lossless") {
        quality = domain::Quality::lossless;
        f.upstream->raw(info(lossless, 900, "flac"));
    }
    auto out = unwrap(resolve(f, quality));
    CHECK(out.actual_quality == quality);
    CHECK(out.actual_track.id.value() == song);
    CHECK(out.completeness == domain::Completeness::full);
    CHECK_FALSE(out.shareable);
}
TEST_CASE("Kugou permission failure is not silently lowered under strict quality",
          "[providers][kugou]") {
    Fixture f;
    f.upstream->raw(info());
    auto denied = info(lossless, 900, "flac");
    denied["status"] = 0;
    denied["pay_type"] = 3;
    denied["url"] = "";
    f.upstream->raw(denied);
    SECTION("strict") {
        auto result = resolve(f, domain::Quality::lossless);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == ErrorCode::credentials_required);
    }
    SECTION("explicit fallback") {
        f.upstream->raw(info(high, 320));
        CHECK(unwrap(resolve(f, domain::Quality::lossless, domain::QualityPolicy::allow_lower))
                  .actual_quality == domain::Quality::high);
    }
}
TEST_CASE("Kugou audio rejects hash format origin and encryption mismatches",
          "[providers][kugou]") {
    Fixture f;
    auto j = info();
    SECTION("hash") { j["hash"] = high; }
    SECTION("format") { j["extName"] = "flac"; }
    SECTION("bitrate") { j["bitRate"] = 64000; }
    SECTION("origin") { j["url"] = "https://sharefs.kugou.com.evil.invalid/a.mp3"; }
    SECTION("HTTP") { j["url"] = "http://sharefs.kugou.com/a.mp3"; }
    SECTION("encryption") { j["fileHead"] = 1024; }
    f.upstream->raw(j);
    CHECK_FALSE(resolve(f));
}
TEST_CASE("Kugou previews require permission and remain previews", "[providers][kugou]") {
    Fixture f;
    auto j = info();
    j["timeLength"] = 60;
    f.upstream->raw(j);
    SECTION("default") { CHECK_FALSE(resolve(f)); }
    SECTION("allowed") {
        CHECK(unwrap(resolve(f, domain::Quality::standard, domain::QualityPolicy::strict, true))
                  .completeness == domain::Completeness::preview);
    }
}
TEST_CASE("Kugou unsupported Hi-Res does not request a network resource", "[providers][kugou]") {
    Fixture f;
    auto out = resolve(f, domain::Quality::hires);
    REQUIRE_FALSE(out);
    CHECK(out.error().code == ErrorCode::quality_unavailable);
    CHECK(f.upstream->calls.empty());
}
TEST_CASE("Kugou lyrics are hash bound and validate encoded response identity",
          "[providers][kugou]") {
    Fixture f;
    f.upstream->raw(
        {{"status", 200},
         {"errcode", 200},
         {"candidates", Json::array({{{"id", "123"}, {"accesskey", "synthetic-key"}}})}});
    auto response = Json{{"status", 200},
                         {"error_code", 0},
                         {"id", "123"},
                         {"fmt", "lrc"},
                         {"content", "WzAwOjAwLjAwXWhlbGxv"}};
    bool valid = true;
    SECTION("valid") {}
    SECTION("wrong lyric") {
        response["id"] = "124";
        valid = false;
    }
    SECTION("invalid encoding") {
        response["content"] = "!!!";
        valid = false;
    }
    f.upstream->raw(response);
    auto result = f.port<ports::ILyrics>()->lyrics(context(), kg::parsing::ref(song, true));
    CHECK(static_cast<bool>(result) == valid);
    if (result)
        CHECK(result->lrc == "[00:00.00]hello");
    CHECK(f.upstream->calls.front().url.ends_with(song));
}
TEST_CASE("Kugou empty lyric candidates report unavailable", "[providers][kugou]") {
    Fixture f;
    f.upstream->raw({{"status", 200}, {"errcode", 200}, {"candidates", Json::array()}});
    CHECK(unwrap(f.port<ports::ILyrics>()->lyrics(context(), kg::parsing::ref(song, true)))
              .unavailable);
}
TEST_CASE("Kugou link parser rejects misleading hosts and ambiguous identities",
          "[providers][kugou]") {
    Fixture f;
    auto port = f.port<ports::ILinkParser>();
    CHECK(unwrap(port->parse_link("https://www.kugou.com/song/#hash=" + std::string{song}))
              .ref.id.value() == song);
    CHECK(unwrap(port->parse_link("https://www.kugou.com/yy/special/single/123.html")).kind ==
          "playlist");
    CHECK(unwrap(port->parse_link("https://www.kugou.com/album/123.html")).kind == "album");
    for (const std::string link :
         {"https://www.kugou.com.evil.invalid/song/#hash=" + std::string{song},
          "https://www.kugou.com/song/?hash=" + std::string{song} + "#hash=" + high,
          std::string{"https://www.kugou.com/album/123.html?albumid=456"},
          std::string{"https://www.kugou.com/song/#hash=bad"}})
        CHECK_FALSE(port->parse_link(link));
    CHECK(f.upstream->calls.empty());
}
TEST_CASE("Kugou parser bounds body size and nesting depth", "[providers][kugou]") {
    CHECK_THROWS_AS(kg::parse_response(std::string(4 * 1024 * 1024 + 1, 'x')),
                    std::invalid_argument);
    std::string deep;
    for (int i = 0; i < 40; ++i)
        deep += "{\"x\":";
    deep += "0";
    deep += std::string(40, '}');
    CHECK_THROWS_AS(kg::parse_response(deep), std::invalid_argument);
    CHECK_THROWS_AS(kg::parse_response("<html>blocked</html>"), std::invalid_argument);
}

namespace {
Json artist_json(int id = 3060) {
    return {{"singerid", id},
            {"singername", "歌手"},
            {"profile", "完整简介"},
            {"intro", "简短介绍"},
            {"alias", "Alias"},
            {"songcount", 5},
            {"albumcount", 3},
            {"imgurl", "http://singerimg.kugou.com/uploadpic/softhead/{size}/head.jpg"}};
}
Json artist_hit(int id = 3060) {
    return {{"AuthorId", id},
            {"AuthorName", "歌手"},
            {"AudioCount", 5},
            {"AlbumCount", 3},
            {"Avatar", "http://singerimg.kugou.com/uploadpic/softhead/240/head.jpg"}};
}
Json artist_search_data(Json values, int page_number = 1, int limit = 2, int total = 3) {
    auto data = search_data(std::move(values), page_number, limit, total);
    data["from"] = (page_number - 1) * limit;
    return data;
}
} // namespace
TEST_CASE("Kugou artist search matches the public web signing vector", "[providers][kugou]") {
    const auto url = kg::artist_search_url("周杰伦", 1, 2, 1700000000000ULL);
    CHECK(url == "https://complexsearch.kugou.com/v1/search/"
                 "author?appid=1014&clienttime=1700000000000&clientver=20000&dfid=-&keyword=%E5%91%"
                 "A8%E6%9D%B0%E4%BC%A6&mid=1700000000000&page=1&pagesize=2&signature="
                 "AC53CB84CDB522AF354667C6C5BEF3EF&srcappid=2919&uuid=1700000000000");
    CHECK(kg::artist_search_url("a&b=c", 1, 2, 1700000000000ULL).find("keyword=a%26b%3Dc&") !=
          std::string::npos);
}
TEST_CASE("Kugou artist search uses genuine pages and maps artist identities",
          "[providers][kugou]") {
    Fixture f;
    f.upstream->raw({{"status", 1},
                     {"error_code", 0},
                     {"data", artist_search_data(Json::array({artist_hit()}), 2, 2, 7)}});
    auto out = unwrap(f.port<ports::ArtistSearch>()->search(context(), {"歌手", 2, "2"}));
    REQUIRE(out.page.items.size() == 1);
    CHECK(out.page.items.front().ref.id.value() == "3060");
    CHECK(out.page.items.front().name == "歌手");
    CHECK(out.page.items.front().track_count == 5);
    CHECK(out.page.items.front().artwork->starts_with("https://singerimg.kugou.com/"));
    CHECK(out.page.page.total == 7);
    CHECK(out.next_cursor == "4");
    CHECK_FALSE(out.page.page.unavailable_count);
    CHECK(f.upstream->calls.front().url.find("page=2&pagesize=2&signature=") != std::string::npos);
}
TEST_CASE("Kugou artist search rejects incorrect page identities and malformed entries",
          "[providers][kugou]") {
    Fixture f;
    auto data = artist_search_data(Json::array({artist_hit()}));
    SECTION("wrong page") { data["page"] = 2; }
    SECTION("wrong offset") { data["from"] = 1; }
    SECTION("wrong window") { data["pagesize"] = 50; }
    SECTION("duplicate artist") { data["lists"].push_back(artist_hit()); }
    SECTION("invalid count") { data["lists"][0]["AudioCount"] = -1; }
    SECTION("empty identity") { data["lists"][0]["AuthorId"] = 0; }
    f.upstream->api(data);
    auto result = f.port<ports::ArtistSearch>()->search(context(), {"歌手", 2, {}});
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::upstream_failed);
}
TEST_CASE("Kugou artist search has explicit empty and rate limited results", "[providers][kugou]") {
    Fixture f;
    SECTION("empty") {
        f.upstream->api(artist_search_data(Json::array(), 1, 2, 0));
        auto out = unwrap(f.port<ports::ArtistSearch>()->search(context(), {"unmatched", 2, {}}));
        CHECK(out.page.items.empty());
        CHECK(out.page.page.total == 0);
        CHECK_FALSE(out.next_cursor);
    }
    SECTION("rate limit") {
        f.upstream->raw({}, 429);
        auto out = f.port<ports::ArtistSearch>()->search(context(), {"歌手", 2, {}});
        REQUIRE_FALSE(out);
        CHECK(out.error().code == ErrorCode::upstream_rate_limited);
        CHECK(out.error().retryable);
    }
}
TEST_CASE("Kugou artist inputs are validated before network access", "[providers][kugou]") {
    Fixture f;
    for (const ports::SearchQuery query : {ports::SearchQuery{"", 2, {}},
                                           {"x", 0, {}},
                                           {"x", 51, {}},
                                           {"x", 2, "1"},
                                           {std::string(1, static_cast<char>(0xff)), 2, {}}})
        CHECK_FALSE(f.port<ports::ArtistSearch>()->search(context(), query));
    auto ref = kg::parsing::ref("3060");
    CHECK_FALSE(f.port<ports::ArtistTracks>()->list(context(), ref, {51, {}}));
    ref.source = unwrap(domain::SourceId::parse("kuwo"));
    CHECK_FALSE(f.port<ports::ArtistDetail>()->detail(context(), ref));
    CHECK_FALSE(f.port<ports::ArtistAlbums>()->list(context(), ref, {2, {}}));
    CHECK(f.upstream->calls.empty());
}
TEST_CASE("Kugou artist detail keeps biography and avatar separate from track artwork",
          "[providers][kugou]") {
    Fixture f;
    f.upstream->api(artist_json());
    auto artist =
        unwrap(f.port<ports::ArtistDetail>()->detail(context(), kg::parsing::ref("3060")));
    CHECK(artist.description == "完整简介");
    CHECK(artist.alternate_name == "Alias");
    CHECK(artist.track_count == 5);
    CHECK(artist.album_count == 3);
    CHECK_FALSE(artist.birth_date);
    CHECK_FALSE(artist.country);
    CHECK_FALSE(artist.mv_count);
    CHECK(artist.artwork == "https://singerimg.kugou.com/uploadpic/softhead/400/head.jpg");
    CHECK_FALSE(kg::parsing::artwork(*artist.artwork));
    auto sparse = Json{{"singerid", 1}, {"singername", "无资料"}};
    auto unknown = kg::parsing::artist(sparse);
    CHECK_FALSE(unknown.artwork);
    CHECK_FALSE(unknown.description);
    CHECK_FALSE(unknown.track_count);
    sparse["imgurl"] = "https://singerimg.kugou.com.evil.invalid/cover.jpg";
    CHECK_FALSE(kg::parsing::artist(sparse).artwork);
}
TEST_CASE("Kugou nonexistent artist is distinct from a mismatched response", "[providers][kugou]") {
    Fixture f;
    auto expected = ErrorCode::not_found;
    SECTION("missing") { f.upstream->raw({{"status", 0}, {"errcode", 0}}); }
    SECTION("mismatch") {
        f.upstream->api(artist_json(3061));
        expected = ErrorCode::upstream_failed;
    }
    auto result = f.port<ports::ArtistDetail>()->detail(context(), kg::parsing::ref("3060"));
    REQUIRE_FALSE(result);
    CHECK(result.error().code == expected);
}
TEST_CASE("Kugou artist songs preserve duplicate positions missing hashes and canonical ids",
          "[providers][kugou]") {
    Fixture f;
    f.upstream->api(artist_json());
    auto missing = track_json();
    missing["hash"] = "";
    f.upstream->api({{"total", 5},
                     {"timestamp", 123},
                     {"info", Json::array({track_json(), track_json(), missing})}});
    auto port = f.port<ports::ArtistTracks>();
    auto out = unwrap(port->list(context(), kg::parsing::ref("3060"), {3, {}}));
    REQUIRE(out.page.items.size() == 2);
    CHECK(out.page.items[0].ref == out.page.items[1].ref);
    CHECK(out.page.items[0].artists.front().ref == kg::parsing::ref("3060"));
    CHECK_FALSE(out.page.items[0].track_number);
    CHECK(out.page.page.total == 5);
    CHECK(out.page.page.unavailable_count == 1);
    REQUIRE(out.next_cursor);
    f.upstream->api(artist_json());
    f.upstream->api(
        {{"total", 5}, {"timestamp", 456}, {"info", Json::array({track_json(), track_json()})}});
    auto last = unwrap(port->list(context(), kg::parsing::ref("3060"), {3, out.next_cursor}));
    CHECK(last.page.items.size() == 2);
    CHECK_FALSE(last.next_cursor);
    CHECK(last.page.page.has_more == false);
    CHECK(f.upstream->calls.back().url.ends_with("sorttype=2&page=2&pagesize=3"));
}
TEST_CASE("Kugou artist album pages retain album metadata and unknown unavailable counts",
          "[providers][kugou]") {
    Fixture f;
    f.upstream->api(artist_json());
    auto album = album_json();
    album["singername"] = "合作歌手";
    album["publishtime"] = "2026-10-23 00:00:00";
    f.upstream->api({{"total", 3}, {"info", Json::array({album})}});
    auto out =
        unwrap(f.port<ports::ArtistAlbums>()->list(context(), kg::parsing::ref("3060"), {2, {}}));
    REQUIRE(out.page.items.size() == 1);
    CHECK(out.page.items[0].release_date == "2026-10-23");
    CHECK_FALSE(out.page.items[0].artists.front().ref);
    CHECK(out.page.items[0].track_count == 3);
    CHECK_FALSE(out.page.page.unavailable_count);
    CHECK(out.next_cursor->starts_with("2:"));
}
TEST_CASE("Kugou artist continuation detects changed totals and cross-directory reuse",
          "[providers][kugou]") {
    Fixture f;
    f.upstream->api(artist_json());
    f.upstream->api({{"total", 3}, {"info", Json::array({track_json()})}});
    auto cursor =
        unwrap(f.port<ports::ArtistTracks>()->list(context(), kg::parsing::ref("3060"), {1, {}}))
            .next_cursor;
    domain::Result<ports::CatalogPage<domain::Track>> changed =
        domain::Error{ErrorCode::internal, "unset", false, {}};
    SECTION("total") {
        f.upstream->api(artist_json());
        f.upstream->api({{"total", 4}, {"info", Json::array({track_json()})}});
        changed =
            f.port<ports::ArtistTracks>()->list(context(), kg::parsing::ref("3060"), {1, cursor});
    }
    SECTION("artist") {
        f.upstream->api(artist_json(3061));
        f.upstream->api({{"total", 3}, {"info", Json::array({track_json()})}});
        changed =
            f.port<ports::ArtistTracks>()->list(context(), kg::parsing::ref("3061"), {1, cursor});
    }
    REQUIRE_FALSE(changed);
    CHECK(changed.error().code == ErrorCode::cursor_expired);
}
TEST_CASE("Kugou artist links reject ambiguity and misleading hosts", "[providers][kugou]") {
    Fixture f;
    auto parser = f.port<ports::ILinkParser>();
    for (const auto* url : {"https://m.kugou.com/singer/info/3060",
                            "https://www.kugou.com/yy/singer/home/3060.html"}) {
        auto result = unwrap(parser->parse_link(url));
        CHECK(result.kind == "artist");
        CHECK(result.ref.id.value() == "3060");
    }
    for (const auto* url :
         {"https://m.kugou.com.evil.invalid/singer/info/3060",
          "https://m.kugou.com/singer/info/3060?singerid=1", "https://m.kugou.com/singer/info/0"})
        CHECK_FALSE(parser->parse_link(url));
}
TEST_CASE("Kugou song remarks never become invented album names", "[providers][kugou]") {
    auto raw = track_json();
    raw["remark"] = "电视剧插曲";
    CHECK_FALSE(kg::parsing::track(raw).album);
    raw["album_name"] = "真实专辑";
    CHECK(kg::parsing::track(raw).album->title == "真实专辑");
}
TEST_CASE("Kugou pagination bound cannot masquerade as collection exhaustion",
          "[providers][kugou]") {
    Fixture f;
    f.upstream->api(search_data(Json::array({track_json()}), 10001, 1, 10002));
    auto result = f.port<ports::ITrackSearch>()->search_tracks(context(), {"中文", 1, "10000"});
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::upstream_failed);
    CHECK(result.error().details.front().field == "pagination");
}
TEST_CASE("Kugou allowed downgrade skips encrypted candidates without hiding network errors",
          "[providers][kugou]") {
    Fixture f;
    f.upstream->raw(info());
    auto encrypted = info(lossless, 900, "flac");
    encrypted["fileHead"] = 1024;
    f.upstream->raw(encrypted);
    SECTION("lower clear audio") {
        f.upstream->raw(info(high, 320));
        CHECK(unwrap(resolve(f, domain::Quality::lossless, domain::QualityPolicy::allow_lower))
                  .actual_quality == domain::Quality::high);
    }
    SECTION("network error stays visible") {
        f.upstream->raw({}, 503);
        auto result = resolve(f, domain::Quality::lossless, domain::QualityPolicy::allow_lower);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == ErrorCode::upstream_failed);
        CHECK(result.error().retryable);
    }
    CHECK(f.upstream->calls.size() == 3);
}
