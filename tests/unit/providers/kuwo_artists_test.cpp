#include "../domain/test_support.hpp"
#include "providers/kuwo/bundle.hpp"
#include "providers/kuwo/parsing/artists.hpp"
#include "providers/kuwo/parsing/parsers.hpp"
#include "providers/registry/provider_registry.hpp"
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <deque>

using namespace music_service;
using namespace music_service::test;
namespace ports = application::ports;
namespace parsing = providers::kuwo::parsing;
using domain::ErrorCode;
using Json = nlohmann::json;

namespace {
Json artist(std::uint64_t id = 336) {
    return {{"id", id}, {"name", "测试歌手"}, {"pic300", "https://star.kuwo.cn/star/starheads/300/test.jpg"},
        {"info", "简介&amp;资料"}, {"aartist", "Test&nbsp;Artist"}, {"country", "地区"}, {"birthday", "1990-01-01"},
        {"birthplace", "出生地"}, {"gener", "男"}, {"language", "国语;"}, {"tall", "170cm"}, {"weight", "60kg"},
        {"constellation", "星座"}, {"musicNum", 3}, {"albumNum", "2"}, {"mvNum", 1}};
}
Json track(std::uint64_t id = 228908) {
    return {{"rid", id}, {"name", "测试歌曲"}, {"artist", "测试歌手"}, {"artistid", 336},
        {"album", "测试专辑"}, {"albumid", 1293}, {"duration", 269}, {"track", 3},
        {"albumpic", "http://img2.kuwo.cn/star/albumcover/500/test.jpg"}};
}
Json album(std::uint64_t id = 1293) {
    return {{"albumid", id}, {"album", "测试专辑"}, {"artist", "测试歌手"}, {"artistid", "336"},
        {"releaseDate", "2003-07-31"}, {"pic", "https://img2.kuwo.cn/star/albumcover/300/test.jpg"}};
}
std::string body(Json data) { return Json{{"code", 200}, {"data", std::move(data)}}.dump(); }
Json search() { return {{"pn", 0}, {"rn", 2}, {"total", "3"}, {"artistList", {artist(), artist(1062)}}}; }

class Upstream final : public ports::UpstreamHttp {
public:
    std::deque<ports::HttpResponse> replies;
    std::vector<ports::HttpRequest> requests;
    void respond(std::string value, int status = 200) { replies.push_back({{status, {}, {}}, std::move(value)}); }
    domain::Result<ports::HopResult<ports::HttpResponse>> fetch(ports::HttpRequest request,
        const ports::AdmissionRequest&, const ports::OperationContext&) override {
        requests.push_back(std::move(request));
        REQUIRE_FALSE(replies.empty());
        auto result = std::move(replies.front()); replies.pop_front();
        return ports::HopResult<ports::HttpResponse>{std::move(result)};
    }
    domain::Result<ports::HopResult<ports::HttpResponseHead>> stream(ports::HttpRequest,
        const ports::AdmissionRequest&, const ports::OperationContext&, const ports::HeadConsumer&,
        const ports::ChunkConsumer&) override { return domain::Error{ErrorCode::internal, "unused", false, {}}; }
};
ports::ProviderContext context() {
    return {{std::chrono::steady_clock::now() + std::chrono::seconds{10}, {}},
        unwrap(domain::AuthIdentityKey::make(anonymous("prn_a", "kuwo"), {})), {}};
}
struct Kuwo {
    std::shared_ptr<Upstream> upstream = std::make_shared<Upstream>();
    std::shared_ptr<const providers::ProviderRegistry> registry;
    Kuwo() {
        providers::ProviderServices services; services.upstream = upstream;
        registry = unwrap(providers::ProviderRegistry::build({providers::kuwo::make_bundle()}, {}, services));
    }
    template<class Port> auto get() { return unwrap(registry->require<Port>("kuwo")); }
};
}

TEST_CASE("kuwo artist search checks page identity and termination", "[kuwo][artists]") {
    auto doc = search();
    const auto first = unwrap(parsing::parse_artist_search(body(doc), 0, 2));
    REQUIRE(first.page.items.size() == 2);
    CHECK(first.page.items[0].ref.id.value() == "336");
    CHECK(first.page.page.total == 3);
    CHECK(first.next_cursor == "1");
    doc["pn"] = 1; doc["artistList"] = Json::array({artist(7)});
    INFO(doc.dump());
    const auto last = unwrap(parsing::parse_artist_search(body(doc), 1, 2));
    CHECK(last.page.page.returned == 1);
    CHECK(last.page.page.has_more == false);
    CHECK_FALSE(last.next_cursor);
    doc["pn"] = 0; doc["total"] = 0; doc["artistList"] = Json::array();
    CHECK(unwrap(parsing::parse_artist_search(body(doc), 0, 2)).page.items.empty());
    for (const auto* field : {"pn", "rn", "total", "artistList"}) {
        auto broken = search(); broken.erase(field);
        CHECK(parsing::parse_artist_search(body(broken), 0, 2).error().code == ErrorCode::upstream_failed);
    }
    doc = search(); doc["pn"] = 1;
    CHECK_FALSE(parsing::parse_artist_search(body(doc), 0, 2));
    doc = search(); doc["rn"] = 3;
    CHECK_FALSE(parsing::parse_artist_search(body(doc), 0, 2));
    doc = search(); doc["artistList"] = {artist(), artist()};
    CHECK_FALSE(parsing::parse_artist_search(body(doc), 0, 2));
    doc = search(); doc["artistList"] = Json::array();
    CHECK_FALSE(parsing::parse_artist_search(body(doc), 0, 2));
}

TEST_CASE("kuwo artist details preserve profile and reject wrong identity", "[kuwo][artists]") {
    const auto value = unwrap(parsing::parse_artist_detail(body(artist()), "336"));
    CHECK(value.name == "测试歌手");
    CHECK(value.description == "简介&资料");
    CHECK(value.alternate_name == "Test Artist");
    CHECK(value.birth_date == "1990-01-01");
    CHECK(value.birthplace == "出生地");
    CHECK(value.country == "地区");
    CHECK(value.gender == "男");
    CHECK(value.language == "国语;");
    CHECK(value.height == "170cm");
    CHECK(value.weight == "60kg");
    CHECK(value.constellation == "星座");
    CHECK(value.track_count == 3);
    CHECK(value.album_count == 2);
    CHECK(value.mv_count == 1);
    CHECK(value.artwork);
    CHECK_FALSE(parsing::parse_artist_detail(body(artist()), "1062"));
    CHECK(parsing::parse_artist_detail(R"({"code":-1,"data":null})", "336").error().code == ErrorCode::not_found);
    const auto minimal = unwrap(parsing::parse_artist_detail(body({{"id", 336}, {"name", "歌手"}}), "336"));
    CHECK_FALSE(minimal.description);
    CHECK_FALSE(minimal.track_count);
    CHECK_FALSE(minimal.artwork);
    for (const auto* malformed : {"<html>artist</html>", "[]", "{}", R"({"code":200})", R"({"code":200,"data":[]})"})
        CHECK(parsing::parse_artist_detail(malformed, "336").error().code == ErrorCode::upstream_failed);
}

TEST_CASE("kuwo tracks use matching album covers and bounded duration", "[kuwo][artists][cover]") {
    const auto value = unwrap(parsing::parse_track_info(body(track()), "228908"));
    REQUIRE(value.album);
    CHECK(value.album->ref->id.value() == "1293");
    CHECK(value.duration_ms == 269000);
    CHECK(value.track_number == 3);
    CHECK(value.artwork == "https://img2.kuwo.cn/star/albumcover/500/test.jpg");
    CHECK_FALSE(parsing::parse_track_info(body(track(7)), "228908"));
    for (const auto* invalid : {"https://evil.example/star/albumcover/x.jpg", "https://kuwo.cn.evil/star/albumcover/x.jpg",
        "https://star.kuwo.cn/star/starheads/x.jpg", "https://user@img2.kuwo.cn/star/albumcover/x.jpg", "file:///cover.jpg",
        "https://img2.kuwo.cn:444/star/albumcover/x.jpg"}) {
        auto doc = track(); doc["albumpic"] = invalid;
        CHECK_FALSE(unwrap(parsing::parse_track_info(body(doc), "228908")).artwork);
    }
    auto doc = track(); doc.erase("albumpic"); doc["pic"] = "https://img2.kuwo.cn/star/albumcover/500/other.jpg";
    CHECK_FALSE(unwrap(parsing::parse_track_info(body(doc), "228908")).artwork);
    doc = track(); doc["albumid"] = 0;
    CHECK_FALSE(unwrap(parsing::parse_track_info(body(doc), "228908")).artwork);
    doc = track(); doc["duration"] = UINT64_MAX;
    CHECK_FALSE(parsing::parse_track_info(body(doc), "228908"));
}

TEST_CASE("kuwo artist lists advance by upstream window and reject false empty pages", "[kuwo][artists]") {
    Json doc{{"total", 5}, {"list", {track()}}};
    const auto short_page = unwrap(parsing::parse_artist_tracks(body(doc), 0, 2));
    CHECK(short_page.page.page.returned == 1);
    CHECK(short_page.next_cursor == "1");
    CHECK(short_page.page.page.has_more == true);
    doc["list"] = Json::array();
    CHECK_FALSE(parsing::parse_artist_tracks(body(doc), 0, 2));
    CHECK_FALSE(unwrap(parsing::parse_artist_tracks(body(doc), 3, 2)).next_cursor);
    doc = {{"total", 3}, {"albumList", {album(), album(8)}}};
    const auto albums = unwrap(parsing::parse_artist_albums(body(doc), 0, 2));
    CHECK(albums.page.items[0].release_date == "2003-07-31");
    CHECK(albums.page.items[0].artists[0].ref->id.value() == "336");
    CHECK_FALSE(albums.page.items[0].track_count);
    CHECK(albums.next_cursor == "1");
    doc["albumList"] = Json::array({album(9)});
    INFO(doc.dump());
    CHECK_FALSE(unwrap(parsing::parse_artist_albums(body(doc), 1, 2)).next_cursor);
    doc["total"] = 0;
    CHECK_FALSE(parsing::parse_artist_albums(body(doc), 0, 2));
    doc["total"] = "-1";
    CHECK_FALSE(parsing::parse_artist_albums(body(doc), 0, 2));
}

TEST_CASE("kuwo artist links accept only known tabs on official hosts", "[kuwo][artists]") {
    for (const auto* suffix : {"", "/", "/info", "/album", "/mv?from=share"}) {
        const auto link = unwrap(parsing::parse_link("https://www.kuwo.cn/singer_detail/336" + std::string{suffix}));
        CHECK(link.kind == "artist"); CHECK(link.id == "336");
    }
    for (const auto* link : {"https://kuwo.cn.evil/singer_detail/336/info", "https://www.kuwo.cn/singer_detail/336/unknown",
        "https://www.kuwo.cn/singer_detail/0/info", "https://www.kuwo.cn/singer_detail/336/info/extra",
        "https://www.kuwo.cn/singer_detail/../info"}) CHECK_FALSE(parsing::parse_link(link));
}

TEST_CASE("kuwo artist capabilities use anonymous direct JSON and validate before I O", "[kuwo][artists]") {
    Kuwo kuwo;
    const auto c = context(); const auto ref = test::track("kuwo", "336");
    auto search_port = kuwo.get<ports::ArtistSearch>();
    kuwo.upstream->respond(body(search()));
    CHECK(unwrap(search_port->search(c, {"周杰伦", 2, {}})).next_cursor == "1");
    CHECK(kuwo.upstream->requests.back().url.find("searchArtistBykeyWord?key=%E5%91%A8%E6%9D%B0%E4%BC%A6&pn=1&rn=2") != std::string::npos);
    kuwo.upstream->respond(body(artist()));
    CHECK(unwrap(kuwo.get<ports::ArtistDetail>()->detail(c, ref)).ref == ref);
    kuwo.upstream->respond(body({{"total", 3}, {"list", {track()}}}));
    CHECK_FALSE(unwrap(kuwo.get<ports::ArtistTracks>()->list(c, ref, {2, "1"})).next_cursor);
    CHECK(kuwo.upstream->requests.back().url.find("artistMusic?artistid=336&pn=2&rn=2") != std::string::npos);
    kuwo.upstream->respond(body({{"total", 1}, {"albumList", {album()}}}));
    CHECK(unwrap(kuwo.get<ports::ArtistAlbums>()->list(c, ref, {2, {}})).page.items.size() == 1);
    const auto before = kuwo.upstream->requests.size();
    CHECK(search_port->search(c, {" ", 2, {}}).error().code == ErrorCode::validation_failed);
    CHECK(search_port->search(c, {"x", 51, {}}).error().code == ErrorCode::validation_failed);
    CHECK(search_port->search(c, {"x", 2, "bad"}).error().code == ErrorCode::cursor_mismatch);
    CHECK(kuwo.get<ports::ArtistDetail>()->detail(c, test::track("qq", "336")).error().code == ErrorCode::validation_failed);
    CHECK(kuwo.get<ports::ArtistTracks>()->list(c, ref, {101, {}}).error().code == ErrorCode::validation_failed);
    CHECK(kuwo.get<ports::ArtistAlbums>()->list(c, ref, {51, {}}).error().code == ErrorCode::validation_failed);
    CHECK(kuwo.upstream->requests.size() == before);
    for (const auto& request : kuwo.upstream->requests) {
        CHECK(request.url.starts_with("https://wapi.kuwo.cn/api/www/"));
        CHECK(request.secret_headers.empty());
        for (const auto& header : request.headers) CHECK(header.name != "Cookie");
    }
    kuwo.upstream->respond("busy", 429);
    CHECK(search_port->search(c, {"x", 2, {}}).error().code == ErrorCode::upstream_rate_limited);
    kuwo.upstream->respond("busy", 503);
    CHECK(kuwo.get<ports::ArtistDetail>()->detail(c, ref).error().retryable);
}

TEST_CASE("kuwo direct track detail carries cover without a webpage request", "[kuwo][artists][cover]") {
    Kuwo kuwo;
    kuwo.upstream->respond(body(track()));
    const auto value = unwrap(kuwo.get<ports::ITrackDetail>()->track_detail(context(), test::track("kuwo", "228908")));
    CHECK(value.artwork);
    REQUIRE(kuwo.upstream->requests.size() == 1);
    CHECK(kuwo.upstream->requests[0].url.starts_with("https://wapi.kuwo.cn/api/www/music/musicInfo?mid=228908&"));
    kuwo.upstream->respond(body(track(9)));
    CHECK(kuwo.get<ports::ITrackDetail>()->track_detail(context(), test::track("kuwo", "228908")).error().code == ErrorCode::upstream_failed);
}
