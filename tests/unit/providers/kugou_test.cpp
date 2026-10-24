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
    std::vector<domain::AuthIdentityKey> identities;
    std::optional<domain::Error> failure;
    bool deferred{false};
    void raw(Json j, int status = 200) { answers.push_back({{status, {}, {}}, j.dump()}); }
    void api(Json data) { raw({{"status", 1}, {"errcode", 0}, {"data", std::move(data)}}); }
    domain::Result<ports::HopResult<ports::HttpResponse>>
    fetch(ports::HttpRequest request, const ports::AdmissionRequest& base,
          const ports::OperationContext&) override {
        REQUIRE(application::transport::check_request(request));
        CHECK(base.source.value() == "kugou");
        CHECK(base.operation == request.operation);
        if (!request.secret_headers.empty()) {
            CHECK(request.url.starts_with("https://wwwapi.kugou.com/play/songinfo?"));
            CHECK(request.operation == "resolve_audio");
            CHECK_FALSE(request.allow_redirects);
        }
        calls.push_back(request);
        REQUIRE(base.identity);
        identities.push_back(*base.identity);
        if (failure) return *failure;
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
TEST_CASE("Kugou registers optional Cookie audio while keeping its catalogue anonymous", "[providers][kugou]") {
    Fixture f;
    auto view = unwrap(f.registry->describe("kugou"));
    CHECK(view.capabilities.size() == 21);
    REQUIRE(view.auth_methods.size() == 1);
    CHECK(view.auth_methods[0].auth_profile == "web_cookie");
    CHECK_FALSE(view.auth_methods[0].supports_verify);
    CHECK(f.registry->instantiated() == 0);
    for (const auto& v : view.capabilities) {
        CHECK(v.spec.credentials == (v.spec.key == "resolve_audio"
            ? ports::CredentialRequirement::optional : ports::CredentialRequirement::none));
        if (v.spec.key == "resolve_audio") CHECK(v.spec.auth_profiles == std::vector<std::string>{"web_cookie"});
    }
    CHECK(f.registry->require<ports::ArtistSearch>("kugou"));
    CHECK(f.registry->require<ports::ArtistDetail>("kugou"));
    CHECK(f.registry->require<ports::ArtistTracks>("kugou"));
    CHECK(f.registry->require<ports::ArtistAlbums>("kugou"));
    CHECK(f.upstream->calls.empty());
}

namespace {
ports::ProviderContext logged_in(std::string cookie = "KugooID=12345; t=synthetic-first; kg_mid=mid-a; kg_dfid=dfid-a",
                                 std::string id = "cre_first") {
    auto c = context();
    c.identity = unwrap(domain::AuthIdentityKey::make(user("prn_a", "kugou", id), {1, {}, {}, {}}));
    c.secret = std::make_shared<const ports::SecretBytes>(cookie);
    return c;
}
Json web_info() {
    return {{"hash", song}, {"encode_album_audio_id", "encodedSong"}, {"is_free_part", 0},
            {"play_url", "https://webfs.tx.kugou.com/audio.mp3"}, {"bitrate", 128},
            {"timelength", 180000}, {"filesize", 2880000}};
}
void web_answers(Fixture& f, Json result = web_info()) {
    auto first = web_info();
    first["play_url"] = "";
    f.upstream->raw({{"status", 1}, {"err_code", 0}, {"data", first}});
    f.upstream->raw({{"status", 1}, {"err_code", 0}, {"data", std::move(result)}});
}
domain::Result<ports::ResolvedAudio> web_resolve(Fixture& f, const ports::ProviderContext& c,
                                                domain::ResourceRequirement requirement = {}) {
    return f.port<ports::IAssetResolver>()->resolve_audio(c, kg::parsing::ref(song, true), requirement);
}
}

TEST_CASE("Kugou Cookie audio signs both requests with the selected account and excludes CDN secrets", "[providers][kugou][cookie-audio]") {
    Fixture f;
    const auto first = logged_in();
    const auto second = logged_in("KuGoo=KugooID=23456&t=synthetic-second; mid=mid-b; dfid=dfid-b", "cre_second");
    for (const auto& c : {first, second, first}) {
        web_answers(f);
        const auto audio = unwrap(web_resolve(f, c));
        CHECK(audio.actual_quality == domain::Quality::standard);
        CHECK(audio.completeness == domain::Completeness::full);
        CHECK(audio.bitrate_bps == 128000);
        CHECK_FALSE(audio.shareable);
        const auto count = f.upstream->calls.size();
        for (std::size_t i = count - 2; i < count; ++i) {
            const auto& call = f.upstream->calls[i];
            CHECK(f.upstream->identities[i] == c.identity);
            REQUIRE(call.secret_headers.size() == 1);
            CHECK(call.secret_headers[0].name == "Cookie");
            CHECK(call.secret_headers[0].value == c.secret->reveal());
            CHECK(call.url.find(c.identity == first.identity ? "userid=12345" : "userid=23456") != std::string::npos);
            CHECK(call.url.find(c.identity == first.identity ? "token=synthetic-first" : "token=synthetic-second") != std::string::npos);
            CHECK(call.url.find("signature=") != std::string::npos);
            CHECK_FALSE(call.allow_redirects);
            CHECK(call.body.empty());
            for (const auto& header : call.headers) CHECK(header.name != "Cookie");
        }
        CHECK(f.upstream->calls[count - 2].url.find("hash=" + std::string{song}) != std::string::npos);
        CHECK(f.upstream->calls[count - 1].url.find("encode_album_audio_id=encodedSong") != std::string::npos);
        CHECK(audio.url.find("synthetic-") == std::string::npos);
        for (const auto& [name, value] : audio.request_headers) {
            CHECK(name != "Cookie");
            CHECK(value.find("synthetic-") == std::string::npos);
        }
    }
    f.upstream->raw(info());
    REQUIRE(resolve(f));
    CHECK(f.upstream->calls.back().url.starts_with("https://m.kugou.com/app/i/getSongInfo.php?"));
    CHECK(f.upstream->calls.back().secret_headers.empty());
    f.upstream->raw(info());
    REQUIRE(f.port<ports::ITrackDetail>()->track_detail(first, kg::parsing::ref(song, true)));
    CHECK(f.upstream->calls.back().secret_headers.empty());
}

TEST_CASE("Kugou Cookie preview stays explicit and never becomes full audio", "[providers][kugou][cookie-audio]") {
    Fixture f;
    auto response = web_info();
    response["is_free_part"] = 1;
    web_answers(f, response);
    const auto denied = web_resolve(f, logged_in());
    REQUIRE_FALSE(denied);
    CHECK(denied.error().code == ErrorCode::preview_rejected);
    web_answers(f, response);
    const auto preview = unwrap(web_resolve(f, logged_in(), {domain::Quality::standard, domain::QualityPolicy::strict, true}));
    CHECK(preview.completeness == domain::Completeness::preview);
    CHECK_FALSE(preview.bitrate_bps); // 网页的 bitrate 是整曲元数据，不是试听文件的实测码率。
    CHECK(preview.duration_ms == 180000);
    REQUIRE(f.upstream->calls.size() == 4);
}

TEST_CASE("Kugou Cookie quality policy never falls back to an anonymous account", "[providers][kugou][cookie-audio]") {
    Fixture f;
    for (const auto quality : {domain::Quality::high, domain::Quality::lossless, domain::Quality::hires}) {
        const auto result = web_resolve(f, logged_in(), {quality});
        REQUIRE_FALSE(result);
        CHECK(result.error().code == ErrorCode::quality_unavailable);
        CHECK(f.upstream->calls.empty());
    }
    web_answers(f);
    const auto audio = unwrap(web_resolve(f, logged_in(), {domain::Quality::lossless, domain::QualityPolicy::allow_lower}));
    CHECK(audio.actual_quality == domain::Quality::standard);
    REQUIRE(f.upstream->calls.size() == 2);
    for (const auto& call : f.upstream->calls) CHECK_FALSE(call.secret_headers.empty());
}

TEST_CASE("Kugou bound credentials must contain unambiguous web login fields", "[providers][kugou][cookie-audio]") {
    Fixture f;
    auto c = logged_in();
    c.secret.reset();
    const auto missing = web_resolve(f, c);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == ErrorCode::credentials_required);
    for (const auto& cookie : {"", "kg_mid=only-device", "KugooID=0; t=token; kg_mid=mid; kg_dfid=dfid",
            "KugooID=123; KuGoo=KugooID=456&t=token; kg_mid=mid; kg_dfid=dfid",
            "KugooID=123; t=first; t=second; kg_mid=mid; kg_dfid=dfid",
            "KugooID=123; t=token\r\nInjected:value; kg_mid=mid; kg_dfid=dfid"}) {
        const auto result = web_resolve(f, logged_in(cookie));
        REQUIRE_FALSE(result);
        CHECK(result.error().code == ErrorCode::credentials_required);
    }
    CHECK(f.upstream->calls.empty());
}

TEST_CASE("Kugou Cookie rejects wrong identities malformed responses and untrusted resources", "[providers][kugou][cookie-audio]") {
    for (const auto key : {"hash", "encode_album_audio_id", "is_free_part", "timelength", "filesize", "bitrate"}) {
        Fixture f;
        auto response = web_info();
        response.erase(key);
        web_answers(f, response);
        const auto result = web_resolve(f, logged_in());
        REQUIRE_FALSE(result);
        CHECK(result.error().code == ErrorCode::upstream_failed);
    }
    for (const auto& patch : std::vector<Json>{{{"hash", high}}, {{"encode_album_audio_id", "anotherSong"}},
            {{"is_free_part", 2}}, {{"play_url", "http://webfs.tx.kugou.com/audio.mp3"}},
            {{"play_url", "https://kugou.com.evil.test/audio.mp3"}},
            {{"play_url", "https://webfs.tx.kugou.com/audio.kgm"}}}) {
        Fixture f;
        auto response = web_info();
        response.update(patch);
        web_answers(f, response);
        const auto result = web_resolve(f, logged_in());
        REQUIRE_FALSE(result);
        CHECK(result.error().code == ErrorCode::upstream_failed);
        CHECK(result.error().message.find("synthetic-") == std::string::npos);
    }
}

TEST_CASE("Kugou Cookie permission upstream and cancellation failures do not retry anonymously", "[providers][kugou][cookie-audio]") {
    Fixture f;
    SECTION("no granted URL") {
        auto denied = web_info();
        denied["play_url"] = "";
        web_answers(f, denied);
        const auto result = web_resolve(f, logged_in());
        REQUIRE_FALSE(result);
        CHECK(result.error().code == ErrorCode::credentials_required);
        CHECK(f.upstream->calls.size() == 2);
    }
    SECTION("HTTP redirect") {
        f.upstream->raw({}, 302);
        const auto result = web_resolve(f, logged_in());
        REQUIRE_FALSE(result);
        CHECK(result.error().code == ErrorCode::upstream_failed);
        CHECK(f.upstream->calls.size() == 1);
    }
    SECTION("upstream rejection") {
        f.upstream->raw({{"status", 0}, {"error_code", 20010}});
        const auto result = web_resolve(f, logged_in());
        REQUIRE_FALSE(result);
        CHECK(result.error().code == ErrorCode::upstream_failed);
        CHECK(f.upstream->calls.size() == 1);
    }
    SECTION("transport failures") {
        for (const auto error : {ErrorCode::canceled, ErrorCode::upstream_timeout, ErrorCode::upstream_failed}) {
            f.upstream->failure = domain::Error{error, "synthetic transport failure", false, {}};
            const auto result = web_resolve(f, logged_in());
            REQUIRE_FALSE(result);
            CHECK(result.error().code == error);
        }
        CHECK(f.upstream->calls.size() == 3);
    }
    SECTION("admission") {
        f.upstream->deferred = true;
        const auto result = web_resolve(f, logged_in());
        REQUIRE_FALSE(result);
        CHECK(result.error().code == ErrorCode::service_busy);
        CHECK(f.upstream->calls.size() == 1);
    }
    for (const auto& call : f.upstream->calls) CHECK_FALSE(call.secret_headers.empty());
}

TEST_CASE("Kugou pay metadata uses standard tariff without guessing unknown privileges", "[providers][kugou][pay]") {
    for (const bool detail : {false, true}) {
        auto doc = detail ? info() : track_json();
        const auto type = detail ? "pay_type" : "PayType";
        const auto privilege = detail ? "128privilege" : "Privilege";
        doc[type] = 3; doc[privilege] = 10;
        auto value = kg::parsing::track(doc, detail);
        CHECK(value.pay.pay_play == 1); CHECK(value.pay.pay_down == 1); CHECK(value.pay.pay_month == 1);
        doc[type] = 2;
        value = kg::parsing::track(doc, detail);
        CHECK(value.pay.pay_play == 1); CHECK(value.pay.pay_down == 1); CHECK(value.pay.pay_month == 0);
        doc[type] = 3; doc[privilege] = 8;
        value = kg::parsing::track(doc, detail);
        CHECK_FALSE(value.pay.pay_play); CHECK(value.pay.pay_down == 1); CHECK(value.pay.pay_month == 1);
        doc[type] = 0; doc["HQPrivilege"] = 10;
        value = kg::parsing::track(doc, detail);
        CHECK(value.pay.pay_play == 0); CHECK(value.pay.pay_down == 0); CHECK(value.pay.pay_month == 0);
        doc[type] = 99;
        value = kg::parsing::track(doc, detail);
        CHECK_FALSE(value.pay.pay_play); CHECK_FALSE(value.pay.pay_down); CHECK_FALSE(value.pay.pay_month);
    }
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
