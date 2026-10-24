// 按已探测的官方响应结构构造合成样本，不包含 Cookie、签名地址或真实歌词。
#include "../domain/test_support.hpp"
#include "application/transport/http_rules.hpp"
#include "providers/migu/bundle.hpp"
#include "providers/migu/parsing.hpp"
#include "providers/registry/provider_registry.hpp"
#include <catch2/catch_test_macros.hpp>
#include <deque>

using namespace music_service;
using namespace music_service::test;
namespace ports = application::ports;
namespace mg = providers::migu;
using Json = nlohmann::json;
using domain::ErrorCode;
namespace {
constexpr auto content = "600900000000000001";
constexpr auto copyright = "600000A0001";
Json song(std::string id = content) {
    return {
        {"resourceType", "2"},      {"contentId", id},
        {"copyrightId", copyright}, {"songId", "123"},
        {"songName", "合成歌曲"},   {"singerList", Json::array({{{"id", "45"}, {"name", "歌手"}}})},
        {"albumId", "67"},          {"album", "测试专辑"},
        {"length", "00:03:20"},     {"trackNumber", "2"},
        {"needEncrypt", "0"},       {"auditionsType", "00"},
        {"chargeAuditions", "0"}};
}
Json songs(int n, int start = 1) {
    Json out = Json::array();
    for (int i = 0; i < n; ++i)
        out.push_back(song(std::to_string(600900000000000000ULL + start + i)));
    return out;
}
Json playlist(std::string id = "111") { return {{"id", id}, {"name", "歌单"}, {"musicNum", "60"}}; }
Json album(bool digital = false) {
    return {{"resourceType", digital ? "5" : "2003"},
            {digital ? "contentId" : "albumId", digital ? "600900000000000111" : "67"},
            {"title", "专辑"},
            {"materialId", "67"},
            {"totalCount", "3"}};
}
Json audio(std::string format = "PQ") {
    return {{"url", "https://freetyst.nf.migu.cn/test.mp3?Key=synthetic"},
            {"formatType", format},
            {"freeListenType", "0"},
            {"songItem", song()}};
}
class Upstream final : public ports::UpstreamHttp {
  public:
    std::deque<ports::HttpResponse> answers;
    std::vector<ports::HttpRequest> calls;
    bool deferred{false}, timeout{false};
    void raw(Json j, int status = 200) { answers.push_back({{status, {}, {}}, j.dump()}); }
    void data(Json j) { raw({{"code", "000000"}, {"data", std::move(j)}}); }
    void resource(Json j) { raw({{"code", "000000"}, {"resource", Json::array({std::move(j)})}}); }
    domain::Result<ports::HopResult<ports::HttpResponse>>
    fetch(ports::HttpRequest req, const ports::AdmissionRequest& a,
          const ports::OperationContext&) override {
        REQUIRE(application::transport::check_request(req));
        CHECK(a.source.value() == "migu");
        CHECK(a.operation == req.operation);
        calls.push_back(req);
        if (timeout)
            return domain::Error{ErrorCode::upstream_timeout, "timed out", true, {}};
        if (deferred)
            return ports::HopResult<ports::HttpResponse>{
                ports::PendingHop{std::move(req), {"source", std::chrono::steady_clock::now()}}};
        REQUIRE_FALSE(answers.empty());
        auto out = std::move(answers.front());
        answers.pop_front();
        return ports::HopResult<ports::HttpResponse>{std::move(out)};
    }
    domain::Result<ports::HopResult<ports::HttpResponseHead>>
    stream(ports::HttpRequest, const ports::AdmissionRequest&, const ports::OperationContext&,
           const ports::HeadConsumer&, const ports::ChunkConsumer&) override {
        return domain::Error{ErrorCode::internal, "unexpected stream", false, {}};
    }
};
ports::ProviderContext context(bool auth = false) {
    auto identity =
        auth
            ? unwrap(domain::AuthIdentityKey::make(user("prn_a", "migu", "cre_1"), {1, {}, {}, {}}))
            : unwrap(domain::AuthIdentityKey::make(anonymous("prn_a", "migu"), {}));
    return {{std::chrono::steady_clock::now() + std::chrono::seconds(10), {}},
            identity,
            {},
            auth ? std::make_shared<ports::SecretBytes>("session=synthetic-secret") : nullptr};
}
struct Fixture {
    std::shared_ptr<Upstream> upstream = std::make_shared<Upstream>();
    std::shared_ptr<const providers::ProviderRegistry> registry =
        unwrap(providers::ProviderRegistry::build({mg::make_bundle()}, {}, {upstream}));
    template <class Port> std::shared_ptr<Port> port() {
        return unwrap(registry->require<Port>("migu"));
    }
    void search(Json list, int total, const char* field = "songResultData") {
        upstream->raw(
            {{"code", "000000"},
             {field, {{"totalCount", std::to_string(total)}, {"result", std::move(list)}}}});
    }
    void resolve(Json data) {
        upstream->resource(song());
        upstream->data(std::move(data));
    }
};
} // namespace

TEST_CASE("Migu registers only implemented native capabilities without network",
          "[providers][migu]") {
    Fixture f;
    CHECK(f.registry->instantiated() == 0);
    CHECK(unwrap(f.registry->describe("migu")).capabilities.size() == 18);
    CHECK(unwrap(f.registry->describe_capability("migu", "recommend_playlists")).catalog_kind ==
          ports::CatalogKind::live);
    CHECK(unwrap(f.registry->describe_capability("migu", "categories")).catalog_kind ==
          ports::CatalogKind::live);
    CHECK_FALSE(f.registry->require<ports::ArtistTracks>("migu"));
    CHECK(f.upstream->calls.empty());
}

TEST_CASE("Migu payment metadata only records explicit paid audition evidence", "[providers][migu][pay]") {
    auto doc = song(); doc["chargeAuditions"] = "1"; doc["vipType"] = "";
    auto value = mg::parsing::track(doc);
    CHECK(value.pay.pay_play == 1); CHECK_FALSE(value.pay.pay_down); CHECK_FALSE(value.pay.pay_month);
    for (const auto& flag : {Json("0"), Json("2"), Json(nullptr)}) {
        doc["chargeAuditions"] = flag;
        value = mg::parsing::track(doc);
        CHECK_FALSE(value.pay.pay_play); CHECK_FALSE(value.pay.pay_down); CHECK_FALSE(value.pay.pay_month);
    }
}

TEST_CASE("Migu fixed search windows preserve every item across public limits",
          "[providers][migu]") {
    Fixture f;
    auto search = f.port<ports::ITrackSearch>();
    std::optional<std::string> cursor;
    std::vector<std::string> ids;
    for (int i = 0; i < 4; ++i) {
        f.search(i < 3 ? songs(20) : songs(3, 21), 23);
        auto page = unwrap(search->search_tracks(context(), {"中文", 7, cursor}));
        for (const auto& t : page.page.items)
            ids.push_back(t.ref.id.value());
        cursor = page.next_cursor;
        CHECK(page.page.page.total == 23);
    }
    REQUIRE(ids.size() == 23);
    for (int i = 0; i < 23; ++i)
        CHECK(ids[static_cast<std::size_t>(i)] == std::to_string(600900000000000001ULL + i));
    CHECK_FALSE(cursor);
    CHECK(f.upstream->calls[2].url.find("pageNo=1") != std::string::npos);
    CHECK(f.upstream->calls[3].url.find("pageNo=2") != std::string::npos);
}

TEST_CASE("Migu empty search and incomplete response are different outcomes", "[providers][migu]") {
    Fixture f;
    SECTION("empty") {
        f.search(Json::array(), 0);
        CHECK(unwrap(f.port<ports::ITrackSearch>()->search_tracks(context(), {"none", 3, {}}))
                  .page.items.empty());
    }
    SECTION("missing result") {
        f.upstream->raw({{"code", "000000"}, {"songResultData", {{"totalCount", "3"}}}});
        CHECK_FALSE(f.port<ports::ITrackSearch>()->search_tracks(context(), {"x", 3, {}}));
    }
    SECTION("empty but positive count") {
        f.search(Json::array(), 30);
        CHECK_FALSE(f.port<ports::ITrackSearch>()->search_tracks(context(), {"x", 3, {}}));
    }
    SECTION("negative count") {
        f.upstream->raw({{"code", "000000"},
                         {"songResultData", {{"totalCount", -1}, {"result", Json::array()}}}});
        CHECK_FALSE(f.port<ports::ITrackSearch>()->search_tracks(context(), {"x", 3, {}}));
    }
}

TEST_CASE("Migu rejects bad input and cursor before HTTP", "[providers][migu]") {
    Fixture f;
    for (const ports::SearchQuery q : {ports::SearchQuery{"", 1, {}},
                                       {"x", 0, {}},
                                       {"x", 51, {}},
                                       {"x", 2, "-1"},
                                       {"x", 2, "10001"},
                                       {"x", 2, "2:bad"},
                                       {"x", 2, "0"}})
        CHECK_FALSE(f.port<ports::ITrackSearch>()->search_tracks(context(), q));
    CHECK_FALSE(f.port<ports::ITrackDetail>()->track_detail(
        context(),
        {unwrap(domain::SourceId::parse("qq")), unwrap(domain::PlatformId::parse(content))}));
    CHECK(f.upstream->calls.empty());
}

TEST_CASE("Migu inconsistent short windows never skip missing positions", "[providers][migu]") {
    Fixture f;
    SECTION("search window before another upstream page") {
        f.search(songs(19), 40);
        auto out = f.port<ports::ITrackSearch>()->search_tracks(context(), {"x", 20, {}});
        REQUIRE_FALSE(out);
        CHECK(out.error().code == ErrorCode::upstream_failed);
    }
    SECTION("collection claims more entries than its final window contains") {
        f.upstream->data({{"songList", songs(5)}, {"totalCount", 10}});
        auto out =
            f.port<ports::PlaylistTracks>()->list(context(), mg::parsing::ref("111"), {50, {}});
        REQUIRE_FALSE(out);
        CHECK(out.error().code == ErrorCode::upstream_failed);
    }
}

TEST_CASE("Migu search distinguishes playlists artists and digital albums", "[providers][migu]") {
    Fixture f;
    SECTION("playlist") {
        f.search(Json::array({playlist()}), 1, "songListResultData");
        CHECK(unwrap(f.port<ports::PlaylistSearch>()->search(context(), {"x", 2, {}}))
                  .page.items.front()
                  .track_count == 60);
    }
    SECTION("artist") {
        f.search(Json::array({{{"id", "45"}, {"name", "歌手"}}}), 1, "singerResultData");
        CHECK(unwrap(f.port<ports::ArtistSearch>()->search(context(), {"x", 2, {}}))
                  .page.items.front()
                  .ref.id.value() == "45");
    }
    SECTION("digital album") {
        auto data = album(true);
        data["albumId"] = "67"; // 不能覆盖数字专辑的内容身份。
        f.search(Json::array({data}), 1, "albumResultData");
        CHECK(unwrap(f.port<ports::AlbumSearch>()->search(context(), {"x", 2, {}}))
                  .page.items.front()
                  .ref.id.value() == "d:600900000000000111");
    }
    SECTION("regular album") {
        f.search(Json::array({album()}), 1, "albumResultData");
        CHECK(unwrap(f.port<ports::AlbumSearch>()->search(context(), {"x", 2, {}}))
                  .page.items.front()
                  .ref.id.value() == "67");
    }
}

TEST_CASE("Migu resource identity is checked and duration is never estimated from size",
          "[providers][migu]") {
    Fixture f;
    SECTION("metadata") {
        auto j = song();
        j.erase("length");
        j["rateFormats"] = Json::array({{{"size", "999999"}, {"formatType", "PQ"}}});
        f.upstream->resource(j);
        auto out = unwrap(
            f.port<ports::ITrackDetail>()->track_detail(context(), mg::parsing::ref(content)));
        CHECK_FALSE(out.duration_ms);
        CHECK(out.track_number == 2);
        CHECK(out.artists.front().name == "歌手");
    }
    SECTION("wrong song") {
        f.upstream->resource(song("600900000000000002"));
        CHECK_FALSE(
            f.port<ports::ITrackDetail>()->track_detail(context(), mg::parsing::ref(content)));
    }
    SECTION("not found") {
        f.upstream->raw({{"code", "000000"}, {"resource", Json::array()}});
        auto out =
            f.port<ports::ITrackDetail>()->track_detail(context(), mg::parsing::ref(content));
        REQUIRE_FALSE(out);
        CHECK(out.error().code == ErrorCode::not_found);
    }
}

TEST_CASE("Migu digital album tracks use verified material id and preserve duplicate positions",
          "[providers][migu]") {
    Fixture f;
    auto port = f.port<ports::AlbumTracks>();
    auto ref = mg::parsing::ref("d:600900000000000111", true);
    Json list = Json::array({song(), song(), song("600900000000000002")});
    f.upstream->resource(album(true));
    f.upstream->data({{"songList", list}, {"totalCount", 3}});
    auto first = unwrap(port->list(context(), ref, {2, {}}));
    REQUIRE(first.page.items.size() == 2);
    CHECK(first.page.items[0].ref == first.page.items[1].ref);
    CHECK(f.upstream->calls[0].url.find("resourceType=5") != std::string::npos);
    CHECK(f.upstream->calls[1].url.find("albumId=67") != std::string::npos);
    f.upstream->resource(album(true));
    f.upstream->data({{"songList", list}, {"totalCount", 3}});
    auto last = unwrap(port->list(context(), ref, {2, first.next_cursor}));
    CHECK(last.page.items.size() == 1);
    CHECK_FALSE(last.next_cursor);
}

TEST_CASE("Migu collection revisions reject stale cursors", "[providers][migu]") {
    Fixture f;
    auto port = f.port<ports::PlaylistTracks>();
    f.upstream->data({{"songList", songs(3)}, {"totalCount", 3}, {"publishTime", "a"}});
    auto first = unwrap(port->list(context(), mg::parsing::ref("111"), {1, {}}));
    f.upstream->data({{"songList", songs(3)}, {"totalCount", 3}, {"publishTime", "b"}});
    auto next = port->list(context(), mg::parsing::ref("111"), {1, first.next_cursor});
    REQUIRE_FALSE(next);
    CHECK(next.error().code == ErrorCode::cursor_expired);
}

TEST_CASE("Migu charts retain ordering and missing song slots", "[providers][migu]") {
    Fixture f;
    auto port = f.port<ports::ChartTracks>();
    Json list = Json::array({{{"resId", content}, {"songData", song().dump()}},
                             {{"resId", "600900000000000002"}, {"songData", ""}},
                             {{"resId", content}, {"songData", song().dump()}}});
    f.upstream->data({{"columnId", "100"},
                      {"title", "榜单"},
                      {"updateTime", "2026-10-23"},
                      {"totalCount", 3},
                      {"hasNextPage", false},
                      {"contents", list}});
    auto result = unwrap(port->list(context(), mg::parsing::ref("100"), {3, {}}));
    CHECK(result.page.items.size() == 2);
    CHECK(result.page.page.unavailable_count == 1);
    CHECK(result.page.page.total == 3);
}

TEST_CASE("Migu charts deduplicate directory groups but do not invent empty success",
          "[providers][migu]") {
    Fixture f;
    SECTION("two charts") {
        Json list = Json::array(
            {{{"rankId", "100"}, {"rankName", "榜一"}}, {{"rankId", "101"}, {"rankName", "榜二"}}});
        f.upstream->data({{"contents", Json::array({{{"contents", list}}, {{"contents", list}}})}});
        CHECK(unwrap(f.port<ports::ICharts>()->charts(context())).size() == 2);
    }
    SECTION("empty directory") {
        f.upstream->data({{"contents", Json::array()}});
        CHECK_FALSE(f.port<ports::ICharts>()->charts(context()));
    }
    SECTION("wrong chart") {
        f.upstream->data({{"columnId", "102"}});
        CHECK_FALSE(f.port<ports::ChartDetail>()->detail(context(), mg::parsing::ref("100")));
    }
}

TEST_CASE("Migu chart snapshot can exceed fifty entries without skipping or refetching page two",
          "[providers][migu]") {
    Fixture f;
    auto port = f.port<ports::ChartTracks>();
    Json rows = Json::array();
    for (const auto& track : songs(100))
        rows.push_back({{"resId", track["contentId"]}, {"songData", track.dump()}});
    auto data =
        Json{{"columnId", "100"}, {"totalCount", 100}, {"hasNextPage", false}, {"contents", rows}};
    f.upstream->data(data);
    auto first = unwrap(port->list(context(), mg::parsing::ref("100"), {50, {}}));
    REQUIRE(first.page.items.size() == 50);
    REQUIRE(first.next_cursor);
    f.upstream->data(data);
    auto last = unwrap(port->list(context(), mg::parsing::ref("100"), {50, first.next_cursor}));
    REQUIRE(last.page.items.size() == 50);
    CHECK(last.page.items.front().ref.id.value() == "600900000000000051");
    CHECK_FALSE(last.next_cursor);
    CHECK(f.upstream->calls.back().url.ends_with("pageNo=1"));
    std::swap(data["contents"][0], data["contents"][1]);
    f.upstream->data(data);
    auto changed = port->list(context(), mg::parsing::ref("100"), {50, first.next_cursor});
    REQUIRE_FALSE(changed);
    CHECK(changed.error().code == ErrorCode::cursor_expired);
}

TEST_CASE("Migu recommendations come from playlist plaza with stable ordered snapshot",
          "[providers][migu]") {
    Fixture f;
    auto port = f.port<ports::IRecommendedPlaylists>();
    Json entries = Json::array(
        {{{"view", "ZJ-SongList-Item"}, {"resType", "2021"}, {"resId", "111"}, {"txt", "推荐一"}},
         {{"view", "ZJ-SongList-Item"}, {"resType", "2021"}, {"resId", "112"}, {"txt", "推荐二"}}});
    f.upstream->data({{"contents", Json::array({{{"contents", entries}}})}});
    auto first = unwrap(port->recommend_playlists(context(), {1, {}}));
    CHECK(first.page.items.front().name == "推荐一");
    CHECK(f.upstream->calls.front().url.find("playlist-square-recommend") != std::string::npos);
    // 随机场景模块不参与推荐目录；其变化不应让编辑列表无法翻页。
    f.upstream->data(
        {{"contents", Json::array({{{"view", "playlist_square_scene_zone"},
                                    {"contents", Json::array({{{"view", "ZJ-SongList-Item"},
                                                               {"resType", "2021"},
                                                               {"resId", "999"},
                                                               {"txt", "随机场景"}}})}},
                                   {{"contents", entries}}})}});
    auto second = unwrap(port->recommend_playlists(context(), {1, first.next_cursor}));
    CHECK(second.page.items.front().ref.id.value() == "112");
    CHECK(second.page.page.total == 2);
    entries[0]["resId"] = "113";
    f.upstream->data({{"contents", Json::array({{{"contents", entries}}})}});
    auto next = port->recommend_playlists(context(), {1, first.next_cursor});
    REQUIRE_FALSE(next);
    CHECK(next.error().code == ErrorCode::cursor_expired);
}

TEST_CASE("Migu live category groups and category page slicing", "[providers][migu]") {
    Fixture f;
    SECTION("directory") {
        f.upstream->data(Json::array(
            {{{"header", {{"title", "风格"}}},
              {"content", Json::array({{{"texts", Json::array({"流行", "123", "encoded"})}}})}}}));
        auto out = unwrap(f.port<ports::ICategories>()->categories(context()));
        REQUIRE(out.size() == 1);
        CHECK(out[0].id == "123");
        CHECK(out[0].group == "风格");
    }
    SECTION("category") {
        Json list = Json::array();
        for (int i = 0; i < 30; ++i)
            list.push_back({{"title", "推荐"},
                            {"barList", Json::array()},
                            {"logEvent", {{"contentId", std::to_string(100 + i)}}}});
        auto data = Json{{"nextPageUrl",
                          "http://app.c.nf.migu.cn/MIGUM3.0/v1.0/template/"
                          "musiclistplaza-listbytag?templateVersion=2&tagId=123&pageNumber=2"},
                         {"contentItemList", Json::array({{{"itemList", list}}})}};
        f.upstream->data(data);
        auto first = unwrap(
            f.port<ports::CategoryPlaylists>()->list(context(), mg::parsing::ref("123"), {7, {}}));
        f.upstream->data(data);
        auto next = unwrap(f.port<ports::CategoryPlaylists>()->list(
            context(), mg::parsing::ref("123"), {7, first.next_cursor}));
        CHECK(next.page.items.front().ref.id.value() == "107");
        CHECK_FALSE(next.page.page.total);
        CHECK(f.upstream->calls.back().url.starts_with("https://"));
    }
}

TEST_CASE("Migu audio keeps permission preview and encryption evidence", "[providers][migu]") {
    Fixture f;
    auto port = f.port<ports::IAssetResolver>();
    auto data = audio();
    SECTION("full") {
        f.resolve(data);
        auto out = unwrap(port->resolve_audio(context(true), mg::parsing::ref(content), {}));
        CHECK(out.completeness == domain::Completeness::full);
        CHECK(out.actual_quality == domain::Quality::standard);
        CHECK_FALSE(out.shareable);
        CHECK(f.upstream->calls[0].secret_headers.empty());
        REQUIRE(f.upstream->calls[1].secret_headers.size() == 1);
        CHECK(f.upstream->calls[1].secret_headers[0].value == "session=synthetic-secret");
        for (const auto& h : out.request_headers)
            CHECK(h.second.find("synthetic-secret") == std::string::npos);
    }
    SECTION("preview even with normal CDN file") {
        data["auditionsLength"] = 60;
        data["songItem"]["auditionsType"] = "04";
        f.resolve(data);
        auto out = unwrap(port->resolve_audio(context(), mg::parsing::ref(content), {}));
        CHECK(out.completeness == domain::Completeness::preview);
        CHECK_FALSE(domain::evaluate_resource({}, {out.actual_quality, out.completeness}));
    }
    SECTION("encrypted") {
        data["songItem"]["needEncrypt"] = "1";
        f.resolve(data);
        CHECK_FALSE(port->resolve_audio(context(), mg::parsing::ref(content), {}));
    }
    SECTION("wrong content") {
        data["songItem"]["contentId"] = "600900000000000002";
        f.resolve(data);
        CHECK_FALSE(port->resolve_audio(context(), mg::parsing::ref(content), {}));
    }
    SECTION("external host") {
        data["url"] = "https://migu.cn.example/test.mp3";
        f.resolve(data);
        CHECK_FALSE(port->resolve_audio(context(), mg::parsing::ref(content), {}));
    }
    SECTION("plaintext") {
        data["url"] = "http://freetyst.nf.migu.cn/test.mp3";
        f.resolve(data);
        CHECK_FALSE(port->resolve_audio(context(), mg::parsing::ref(content), {}));
    }
    SECTION("strict downgrade") {
        f.resolve(data);
        auto out =
            port->resolve_audio(context(), mg::parsing::ref(content), {domain::Quality::high});
        REQUIRE_FALSE(out);
        CHECK(out.error().code == ErrorCode::quality_unavailable);
    }
    SECTION("strict permission") {
        f.resolve({{"cannotCode", "440003"}});
        auto out =
            port->resolve_audio(context(), mg::parsing::ref(content), {domain::Quality::lossless});
        REQUIRE_FALSE(out);
        CHECK(out.error().code == ErrorCode::credentials_required);
    }
    SECTION("explicit lower fallback") {
        f.resolve({{"cannotCode", "440003"}});
        f.upstream->data({{"cannotCode", "440003"}});
        f.upstream->data(audio());
        auto out = unwrap(
            port->resolve_audio(context(), mg::parsing::ref(content),
                                {domain::Quality::lossless, domain::QualityPolicy::allow_lower}));
        CHECK(out.actual_quality == domain::Quality::standard);
        CHECK(f.upstream->calls.size() == 4);
    }
    SECTION("hires") {
        CHECK_FALSE(
            port->resolve_audio(context(), mg::parsing::ref(content), {domain::Quality::hires}));
        CHECK(f.upstream->calls.empty());
    }
}

TEST_CASE("Migu verified format and unknown completeness are not guessed", "[providers][migu]") {
    Fixture f;
    SECTION("high") {
        f.resolve(audio("HQ"));
        auto out = unwrap(f.port<ports::IAssetResolver>()->resolve_audio(
            context(), mg::parsing::ref(content), {domain::Quality::high}));
        CHECK(out.actual_quality == domain::Quality::high);
        CHECK(out.container == "mp3");
        CHECK(f.upstream->calls.back().url.ends_with("toneFlag=HQ"));
    }
    SECTION("lossless") {
        auto data = audio("SQ");
        data["url"] = "https://freetyst.nf.migu.cn/official.flac?Key=synthetic";
        f.resolve(data);
        auto out = unwrap(f.port<ports::IAssetResolver>()->resolve_audio(
            context(), mg::parsing::ref(content), {domain::Quality::lossless}));
        CHECK(out.actual_quality == domain::Quality::lossless);
        CHECK(out.container == "flac");
        CHECK(out.url == data["url"].get<std::string>());
        CHECK(f.upstream->calls.back().url.ends_with("toneFlag=SQ"));
    }
    SECTION("unknown") {
        auto data = audio();
        data["songItem"].erase("auditionsType");
        data["songItem"].erase("chargeAuditions");
        f.resolve(data);
        auto out = unwrap(f.port<ports::IAssetResolver>()->resolve_audio(
            context(), mg::parsing::ref(content), {}));
        CHECK(out.completeness == domain::Completeness::unknown);
    }
}

TEST_CASE("Migu Z3D attaches a transient Android key only to explicitly returned Z3D", "[providers][migu][z3d]") {
    Fixture f;
    auto data = audio("Z3D");
    data["songItem"]["needEncrypt"] = "1";
    data["songItem"]["z3dCode"] = {{"formatType", "Z3D"}, {"androidFileType", "wav"},
        {"androidFileKey", "9A14CB2B6AA5DA30CE036BBD6664373E"}};
    SECTION("supported Android Z3D") {
        data["auditionsLength"] = 60;
        f.resolve(data);
        auto out = unwrap(f.port<ports::IAssetResolver>()->resolve_audio(context(), mg::parsing::ref(content), {}));
        CHECK(out.processing == ports::ProcessingPlan{"audio.migu-z3d", 1, "migu.z3d"});
        REQUIRE(out.decryption_key);
        CHECK(out.decryption_key->reveal() == "9A14CB2B6AA5DA30CE036BBD6664373E");
        CHECK(out.container == "wav");
        CHECK_FALSE(out.actual_quality);
        CHECK_FALSE(out.bitrate_bps);
        CHECK_FALSE(out.shareable);
        CHECK(out.completeness == domain::Completeness::preview);
        CHECK(out.duration_ms == 60000);
        for (const auto& h : out.request_headers) CHECK(h.second.find("9A14CB") == std::string::npos);
    }
    SECTION("side metadata never switches an ordinary stream to 3D") {
        data["formatType"] = "PQ";
        data["songItem"]["needEncrypt"] = "0";
        f.resolve(data);
        auto out = unwrap(f.port<ports::IAssetResolver>()->resolve_audio(context(), mg::parsing::ref(content), {}));
        CHECK_FALSE(out.decryption_key);
        CHECK_FALSE(out.processing.decryption);
        CHECK(out.container == "mp3");
    }
    SECTION("permission denied even when a key is present") {
        data["cannotCode"] = "440003";
        f.resolve(data);
        auto out = f.port<ports::IAssetResolver>()->resolve_audio(context(), mg::parsing::ref(content), {});
        REQUIRE_FALSE(out);
        CHECK(out.error().code == ErrorCode::credentials_required);
    }
    SECTION("missing key") {
        data["songItem"]["z3dCode"].erase("androidFileKey");
        f.resolve(data);
        CHECK_FALSE(f.port<ports::IAssetResolver>()->resolve_audio(context(), mg::parsing::ref(content), {}));
    }
    SECTION("iOS format cannot use Android decoding") {
        data["songItem"]["z3dCode"]["androidFileType"] = "m4a";
        f.resolve(data);
        CHECK_FALSE(f.port<ports::IAssetResolver>()->resolve_audio(context(), mg::parsing::ref(content), {}));
    }
}

TEST_CASE("Migu preview duration uses positive clip evidence instead of the full song",
          "[providers][migu]") {
    Fixture f;
    auto data = audio();
    data["songItem"]["auditionsType"] = "04";
    std::optional<std::int64_t> expected;
    bool invalid = false;
    SECTION("response clip duration") {
        data["auditionsLength"] = 60;
        expected = 60000;
    }
    SECTION("song item clip duration") {
        data["songItem"]["auditionsLength"] = 30;
        expected = 30000;
    }
    SECTION("zero response length must not hide positive item length") {
        data["auditionsLength"] = 0;
        data["songItem"]["auditionsLength"] = 30;
        expected = 30000;
    }
    SECTION("no clip duration stays unknown") {}
    SECTION("invalid response clip duration") {
        data["auditionsLength"] = 86401;
        invalid = true;
    }
    SECTION("invalid item clip duration") {
        data["songItem"]["auditionsLength"] = 86401;
        invalid = true;
    }
    f.resolve(data);
    auto out = f.port<ports::IAssetResolver>()->resolve_audio(
        context(), mg::parsing::ref(content),
        {domain::Quality::standard, domain::QualityPolicy::strict, true});
    if (invalid) {
        REQUIRE_FALSE(out);
        CHECK(out.error().code == ErrorCode::upstream_failed);
    } else {
        REQUIRE(out);
        CHECK(out->completeness == domain::Completeness::preview);
        CHECK(out->duration_ms == expected);
    }
}

TEST_CASE("Migu explicit lower fallback can skip an encrypted quality", "[providers][migu]") {
    Fixture f;
    auto encrypted = audio("SQ");
    encrypted["songItem"]["needEncrypt"] = "1";
    f.resolve(encrypted);
    SECTION("strict returns the encryption failure without downgrade") {
        auto out = f.port<ports::IAssetResolver>()->resolve_audio(
            context(), mg::parsing::ref(content), {domain::Quality::lossless});
        REQUIRE_FALSE(out);
        CHECK(out.error().code == ErrorCode::quality_unavailable);
        CHECK(f.upstream->calls.size() == 2);
    }
    SECTION("allow lower continues through the official permission endpoint") {
        f.upstream->data({{"cannotCode", "440003"}});
        f.upstream->data(audio());
        auto out = f.port<ports::IAssetResolver>()->resolve_audio(
            context(), mg::parsing::ref(content),
            {domain::Quality::lossless, domain::QualityPolicy::allow_lower});
        REQUIRE(out);
        CHECK(out->actual_quality == domain::Quality::standard);
        CHECK(out->completeness == domain::Completeness::full);
        CHECK_FALSE(out->processing.decryption);
        REQUIRE(f.upstream->calls.size() == 4);
        CHECK(f.upstream->calls[2].url.ends_with("toneFlag=HQ"));
        CHECK(f.upstream->calls[3].url.ends_with("toneFlag=PQ"));
    }
}

TEST_CASE("Migu lyrics report absence and reject error documents without leaking credentials",
          "[providers][migu]") {
    Fixture f;
    auto j = song();
    auto port = f.port<ports::ILyrics>();
    SECTION("absent") {
        f.upstream->resource(j);
        CHECK(unwrap(port->lyrics(context(), mg::parsing::ref(content))).unavailable);
    }
    SECTION("present") {
        j["lrcUrl"] = "https://d.musicapp.migu.cn/test.lrc";
        f.upstream->resource(j);
        f.upstream->answers.push_back({{200, {}, {}}, "[00:00.00]合成测试"});
        auto out = unwrap(port->lyrics(context(true), mg::parsing::ref(content)));
        CHECK(out.lrc == "[00:00.00]合成测试");
        CHECK(f.upstream->calls.back().secret_headers.empty());
    }
    SECTION("error body") {
        j["lrcUrl"] = "https://d.musicapp.migu.cn/test.lrc";
        f.upstream->resource(j);
        f.upstream->answers.push_back({{200, {}, {}}, "<Error>missing</Error>"});
        CHECK_FALSE(port->lyrics(context(), mg::parsing::ref(content)));
    }
}

TEST_CASE("Migu transport preserves bounded error categories", "[providers][migu]") {
    Fixture f;
    SECTION("rate limit") {
        f.upstream->raw({}, 429);
        auto out = f.port<ports::ITrackSearch>()->search_tracks(context(), {"x", 1, {}});
        REQUIRE_FALSE(out);
        CHECK(out.error().code == ErrorCode::upstream_rate_limited);
    }
    SECTION("timeout") {
        f.upstream->timeout = true;
        auto out = f.port<ports::ITrackSearch>()->search_tracks(context(), {"x", 1, {}});
        REQUIRE_FALSE(out);
        CHECK(out.error().code == ErrorCode::upstream_timeout);
    }
    SECTION("admission") {
        f.upstream->deferred = true;
        auto out = f.port<ports::ITrackSearch>()->search_tracks(context(), {"x", 1, {}});
        REQUIRE_FALSE(out);
        CHECK(out.error().code == ErrorCode::service_busy);
    }
    SECTION("unknown upstream code") {
        f.upstream->raw({{"code", "299999"}, {"info", "synthetic-secret"}});
        auto out = f.port<ports::ITrackSearch>()->search_tracks(context(), {"x", 1, {}});
        REQUIRE_FALSE(out);
        CHECK(out.error().message.find("synthetic-secret") == std::string::npos);
    }
    SECTION("too deep") {
        std::string body(35, '[');
        body += "0";
        body += std::string(35, ']');
        CHECK_THROWS(mg::parse_json(body));
    }
}

TEST_CASE("Migu links reject copyright confusion and ambiguous identity", "[providers][migu]") {
    Fixture f;
    auto port = f.port<ports::ILinkParser>();
    CHECK(unwrap(port->parse_link("https://music.migu.cn/v3/music/song/600900000000000001"))
              .ref.id.value() == content);
    CHECK(unwrap(
              port->parse_link(
                  "https://music.migu.cn/v5/albumDetail?albumId=600900000000000111&playlistType=5"))
              .ref.id.value() == "d:600900000000000111");
    CHECK(unwrap(port->parse_link(
                     "https://music.migu.cn/v5/playlist?playlistId=111&playlistType=ordinary"))
              .kind == "playlist");
    for (auto link : {"https://music.migu.cn.evil/v3/music/song/600900000000000001",
                      "https://music.migu.cn/v3/music/song/60000000001",
                      "https://music.migu.cn/v3/music/album/600900000000000111",
                      "https://music.migu.cn/v5/playlist?playlistId=111&playlistId=222",
                      "https://music.migu.cn/v5/playlist?playlistId=111&songId=222",
                      "https://music.migu.cn/v3/music/playlist/111#x"})
        CHECK_FALSE(port->parse_link(link));
    CHECK(f.upstream->calls.empty());
}
