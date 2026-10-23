// QQ 协议的合成响应回归；不访问网络，不含真实账号、Cookie 或 vkey。
#include "../domain/test_support.hpp"
#include "application/transport/http_rules.hpp"
#include "providers/qq/bundle.hpp"
#include "providers/qq/parsing.hpp"
#include "providers/registry/provider_registry.hpp"
#include <catch2/catch_test_macros.hpp>
#include <deque>

using namespace music_service;
using namespace music_service::test;
namespace ports = application::ports;
namespace qq = providers::qq;
using Json = nlohmann::json;
using domain::ErrorCode;
namespace {
constexpr auto song = "00000000000001";
constexpr auto song2 = "00000000000002";
constexpr auto media = "00000000000009";
constexpr auto album_id = "00000000000011";
constexpr auto artist_id = "00000000000021";
constexpr auto fake_cookie = "uin=12345; qqmusic_key=synthetic-secret";

class FakeUpstream final : public ports::UpstreamHttp {
public:
    std::deque<ports::HttpResponse> answers;
    std::vector<ports::HttpRequest> calls;
    bool deferred{false};
    void raw(Json j, int status = 200) { answers.push_back({{status, {}, {}}, j.dump()}); }
    void rpc(Json j) { raw({{"code", 0}, {"req_0", {{"code", 0}, {"data", std::move(j)}}}}); }
    void login() { rpc({{"info", {{"retCode", 0}, {"nick", "测试用户"}}}}); }
    domain::Result<ports::HopResult<ports::HttpResponse>> fetch(ports::HttpRequest request,
        const ports::AdmissionRequest& base, const ports::OperationContext&) override {
        REQUIRE(application::transport::check_request(request));
        CHECK(base.source.value() == "qq");
        CHECK(base.operation == request.operation);
        calls.push_back(request);
        if (deferred) return ports::HopResult<ports::HttpResponse>{ports::PendingHop{std::move(request), {"source", std::chrono::steady_clock::now()}}};
        REQUIRE_FALSE(answers.empty());
        auto result = std::move(answers.front()); answers.pop_front();
        return ports::HopResult<ports::HttpResponse>{std::move(result)};
    }
    domain::Result<ports::HopResult<ports::HttpResponseHead>> stream(ports::HttpRequest,
        const ports::AdmissionRequest&, const ports::OperationContext&, const ports::HeadConsumer&, const ports::ChunkConsumer&) override {
        return domain::Error{ErrorCode::internal, "unexpected stream", false, {}};
    }
};
ports::ProviderContext context(bool auth = true) {
    auto identity = auth ? unwrap(domain::AuthIdentityKey::make(user("prn_a", "qq", "cre_1"), {1, {}, {}, {}})) :
                           unwrap(domain::AuthIdentityKey::make(anonymous("prn_a", "qq"), {}));
    return {{std::chrono::steady_clock::now() + std::chrono::seconds(10), {}}, identity, {},
            auth ? std::make_shared<ports::SecretBytes>(fake_cookie) : nullptr};
}
struct Fixture {
    std::shared_ptr<FakeUpstream> upstream = std::make_shared<FakeUpstream>();
    std::shared_ptr<const providers::ProviderRegistry> registry = unwrap(providers::ProviderRegistry::build({qq::make_bundle()}, {}, {upstream}));
    template<class Port> std::shared_ptr<Port> port() { return unwrap(registry->require<Port>("qq")); }
};
Json track_json(const char* mid = song, int id = 1) {
    return {{"id", id}, {"mid", mid}, {"name", "中文歌名"}, {"singer", Json::array({{{"mid", artist_id}, {"name", "歌手"}}})},
            {"album", {{"mid", album_id}, {"name", "专辑"}}}, {"interval", 180}, {"index_album", 1},
            {"file", {{"media_mid", media}, {"size_128mp3", 10000}, {"size_320mp3", 20000}, {"size_flac", 30000}}}};
}
Json playlist_json(int id = 100) { return {{"dissid", id}, {"dissname", "歌单"}, {"song_count", 3}}; }
Json album_json() { return {{"albumMID", album_id}, {"albumName", "专辑"}, {"song_count", 3}}; }
Json search_json(std::string field, Json value, int page = 1) {
    return {{"meta", {{"curpage", page}, {"perpage", 1}, {"sum", 3}}}, {"body", {{field, {{"list", Json::array({value})}}}}}};
}
Json playlist_data(int revision = 1) {
    return {{"dirinfo", {{"id", 100}, {"title", "歌单"}, {"songnum", 4}, {"mtime", revision}}},
            {"songlist", Json::array({track_json(), track_json()})}};
}
Json vkey_item(std::string prefix = "M500", int result = 0) {
    auto filename = prefix + media + (prefix == "F000" ? ".flac" : ".mp3");
    return {{"filename", filename}, {"songmid", song}, {"result", result}, {"ekey", ""},
            {"purl", result ? "" : filename + "?vkey=synthetic-vkey"}};
}
void resolve_answers(Fixture& f, Json candidates) {
    f.upstream->rpc({{"track_info", track_json()}});
    f.upstream->rpc({{"retcode", 0}, {"expiration", 3600}, {"midurlinfo", std::move(candidates)}});
}
}

TEST_CASE("QQ registry advertises native capabilities without opening connections", "[providers][qq]") {
    Fixture f;
    CHECK(f.registry->instantiated() == 0);
    CHECK(unwrap(f.registry->describe("qq")).capabilities.size() == 21);
    CHECK(unwrap(f.registry->describe_capability("qq", "search_tracks")).credentials == ports::CredentialRequirement::required);
    CHECK(unwrap(f.registry->describe_capability("qq", "recommend_playlists")).credentials == ports::CredentialRequirement::none);
    CHECK(f.upstream->calls.empty());
}

TEST_CASE("QQ search preserves Chinese data and selects the matching search type", "[providers][qq]") {
    Fixture f;
    f.upstream->login();
    SECTION("track second page") {
        f.upstream->rpc(search_json("song", track_json(), 2));
        auto out = unwrap(f.port<ports::ITrackSearch>()->search_tracks(context(), {"中文", 1, "1"}));
        CHECK(out.page.items.front().title == "中文歌名"); CHECK(out.next_cursor == "2");
        auto param = Json::parse(f.upstream->calls.back().body)["req_0"]["param"];
        CHECK(param["search_type"] == 0); CHECK(param["page_num"] == 2);
    }
    SECTION("playlist") {
        f.upstream->rpc(search_json("songlist", playlist_json()));
        CHECK(unwrap(f.port<ports::PlaylistSearch>()->search(context(), {"中文", 1, {}})).page.items.front().name == "歌单");
        CHECK(Json::parse(f.upstream->calls.back().body)["req_0"]["param"]["search_type"] == 3);
    }
    SECTION("album") {
        f.upstream->rpc(search_json("album", album_json()));
        CHECK(unwrap(f.port<ports::AlbumSearch>()->search(context(), {"中文", 1, {}})).page.items.front().title == "专辑");
        CHECK(Json::parse(f.upstream->calls.back().body)["req_0"]["param"]["search_type"] == 2);
    }
    SECTION("artist") {
        f.upstream->rpc(search_json("singer", {{"singerMID", artist_id}, {"singerName", "歌手"}}));
        CHECK(unwrap(f.port<ports::ArtistSearch>()->search(context(), {"中文", 1, {}})).page.items.front().name == "歌手");
        CHECK(Json::parse(f.upstream->calls.back().body)["req_0"]["param"]["search_type"] == 1);
    }
    REQUIRE(f.upstream->calls.size() == 2);
    for (const auto& request : f.upstream->calls) {
        CHECK(request.body.find("synthetic-secret") == std::string::npos);
        CHECK(request.url.find("synthetic-secret") == std::string::npos);
        REQUIRE(request.secret_headers.size() == 1);
        CHECK(request.secret_headers[0].name == "Cookie");
        CHECK(request.secret_headers[0].value == fake_cookie);
        for (const auto& h : request.headers) CHECK(h.value.find("synthetic-secret") == std::string::npos);
    }
}

TEST_CASE("QQ search distinguishes an expired session from an empty result", "[providers][qq]") {
    Fixture f;
    SECTION("missing secret") {
        auto result = f.port<ports::ITrackSearch>()->search_tracks(context(false), {"test", 1, {}});
        REQUIRE_FALSE(result); CHECK(result.error().code == ErrorCode::credentials_required); CHECK(f.upstream->calls.empty());
    }
    SECTION("expired session") {
        f.upstream->raw({{"code", 0}, {"req_0", {{"code", 1000}}}});
        auto result = f.port<ports::ITrackSearch>()->search_tracks(context(), {"test", 1, {}});
        REQUIRE_FALSE(result); CHECK(result.error().code == ErrorCode::credentials_required); CHECK(f.upstream->calls.size() == 1);
    }
    SECTION("valid empty search") {
        f.upstream->login(); f.upstream->rpc({{"meta", {{"curpage", 1}, {"perpage", 1}, {"sum", 0}}}, {"body", {{"song", {{"list", Json::array()}}}}}});
        auto result = unwrap(f.port<ports::ITrackSearch>()->search_tracks(context(), {"test", 1, {}}));
        CHECK(result.page.items.empty()); CHECK_FALSE(result.next_cursor);
    }
}

TEST_CASE("QQ rejects malformed search pages and bounds input before HTTP", "[providers][qq]") {
    Fixture f; auto port = f.port<ports::ITrackSearch>();
    for (const ports::SearchQuery q : {ports::SearchQuery{"x", 0, {}}, {"", 1, {}}, {"x", 51, {}},
                                      {"x", 2, "1"}, {"x", 1, "10001"}, {"x", 1, "1:bad"}}) {
        CHECK_FALSE(port->search_tracks(context(), q));
    }
    CHECK(f.upstream->calls.empty());
    f.upstream->login(); auto response = search_json("song", track_json());
    SECTION("wrong page") { response["meta"]["curpage"] = 2; }
    SECTION("too many results") { response["body"]["song"]["list"].push_back(track_json(song2, 2)); }
    SECTION("missing count") { response["meta"].erase("sum"); }
    SECTION("negative count") { response["meta"]["sum"] = -1; }
    f.upstream->rpc(response);
    auto result = port->search_tracks(context(), {"x", 1, {}});
    REQUIRE_FALSE(result); CHECK(result.error().code == ErrorCode::upstream_failed);
}

TEST_CASE("QQ playlist keeps duplicates and invalidates changed revisions", "[providers][qq]") {
    Fixture f; auto port = f.port<ports::PlaylistTracks>();
    f.upstream->rpc(playlist_data());
    auto first = unwrap(port->list(context(), track("qq", "100"), {2, {}}));
    REQUIRE(first.page.items.size() == 2); CHECK(first.page.items[0].ref == first.page.items[1].ref);
    REQUIRE(first.next_cursor);
    SECTION("unchanged") {
        f.upstream->rpc(playlist_data());
        auto second = unwrap(port->list(context(), track("qq", "100"), {2, first.next_cursor}));
        CHECK_FALSE(second.next_cursor); CHECK(second.page.page.total == 4);
        CHECK(Json::parse(f.upstream->calls.back().body)["req_0"]["param"]["song_begin"] == 2);
    }
    SECTION("changed") {
        f.upstream->rpc(playlist_data(2));
        auto result = port->list(context(), track("qq", "100"), {2, first.next_cursor});
        REQUIRE_FALSE(result); CHECK(result.error().code == ErrorCode::cursor_expired);
    }
}

TEST_CASE("QQ short pages advance the upstream window without invented unavailable counts", "[providers][qq]") {
    Fixture f;
    SECTION("search") {
        f.upstream->login(); auto doc = search_json("songlist", playlist_json(), 2);
        doc["meta"]["perpage"] = 2; doc["meta"]["sum"] = 20; f.upstream->rpc(doc);
        auto result = unwrap(f.port<ports::PlaylistSearch>()->search(context(), {"中文", 2, "2"}));
        CHECK(result.next_cursor == "4"); CHECK(result.page.items.size() == 1); CHECK_FALSE(result.page.page.unavailable_count);
    }
    SECTION("playlist") {
        auto doc = playlist_data(); doc["songlist"].erase(1); f.upstream->rpc(doc);
        auto result = unwrap(f.port<ports::PlaylistTracks>()->list(context(), track("qq", "100"), {2, {}}));
        REQUIRE(result.next_cursor); CHECK(result.next_cursor->starts_with("2:")); CHECK_FALSE(result.page.page.unavailable_count);
    }
}

TEST_CASE("QQ chart preserves rank ordering and counts unavailable songs", "[providers][qq]") {
    Fixture f;
    Json details = Json::array({track_json(song2, 2), track_json()});
    SECTION("complete") {}
    SECTION("one missing") { details.erase(1); }
    const auto available = details.size();
    f.upstream->rpc({{"data", {{"topId", 62}, {"totalNum", 3}, {"period", "2026-10-23"}, {"updateTime", "2026-10-23"},
        {"song", Json::array({{{"songId", 1}}, {{"songId", 2}}})}}}, {"songInfoList", details}});
    auto result = unwrap(f.port<ports::ChartTracks>()->list(context(), track("qq", "62"), {2, {}}));
    CHECK(result.page.items.size() == available);
    CHECK(result.page.page.unavailable_count == 2 - available);
    CHECK(result.page.items.back().ref.id.value() == song2);
    if (available == 2) CHECK(result.page.items.front().ref.id.value() == song);
    REQUIRE(result.next_cursor); CHECK(result.next_cursor->starts_with("2:"));
    CHECK(f.upstream->calls.back().secret_headers.empty());
}

TEST_CASE("QQ recommendations are public and reject a changed continuation", "[providers][qq]") {
    Fixture f; auto port = f.port<ports::IRecommendedPlaylists>();
    Json list = Json::array({playlist_json(100), playlist_json(101), playlist_json(102)});
    f.upstream->rpc({{"v_hot", list}});
    auto first = unwrap(port->recommend_playlists(context(), {2, {}}));
    CHECK(first.page.page.listed_total == 3); CHECK(f.upstream->calls.back().secret_headers.empty());
    SECTION("same set") {
        list[0]["listennum"] = 999;
        f.upstream->rpc({{"v_hot", list}});
        auto second = unwrap(port->recommend_playlists(context(), {2, first.next_cursor}));
        REQUIRE(second.page.items.size() == 1); CHECK(second.page.items[0].ref.id.value() == "102"); CHECK_FALSE(second.next_cursor);
    }
    SECTION("changed set") {
        list[1] = playlist_json(103); f.upstream->rpc({{"v_hot", list}});
        auto result = port->recommend_playlists(context(), {2, first.next_cursor});
        REQUIRE_FALSE(result); CHECK(result.error().code == ErrorCode::cursor_expired);
    }
}

TEST_CASE("QQ categories exclude disabled entries and reject duplicate identifiers", "[providers][qq]") {
    Fixture f;
    Json list = Json::array({{{"categoryId", 1}, {"categoryName", "中文"}, {"usable", 1}},
                            {{"categoryId", 0}, {"categoryName", "禁用"}, {"usable", 0}}});
    bool duplicate = false;
    SECTION("valid") {}
    SECTION("duplicate") { list.push_back(list[0]); duplicate = true; }
    f.upstream->raw({{"code", 0}, {"data", {{"categories", Json::array({{{"categoryGroupName", "语种"}, {"items", list}}})}}}});
    auto result = f.port<ports::ICategories>()->categories(context());
    if (duplicate) CHECK_FALSE(result);
    else { REQUIRE(result); REQUIRE(result->size() == 1); CHECK(result->front().name == "中文"); }
    CHECK(f.upstream->calls.back().secret_headers.empty());
    CHECK(f.upstream->calls.back().url.find("outCharset=utf-8") != std::string::npos);
}

TEST_CASE("QQ details cannot silently substitute another entity", "[providers][qq]") {
    Fixture f;
    SECTION("track") {
        f.upstream->rpc({{"track_info", track_json(song2, 2)}});
        auto result = f.port<ports::ITrackDetail>()->track_detail(context(), track("qq", song));
        REQUIRE_FALSE(result); CHECK(result.error().code == ErrorCode::not_found);
    }
    SECTION("playlist") {
        auto doc = playlist_data(); doc["dirinfo"]["id"] = 101; f.upstream->rpc(doc);
        auto result = f.port<ports::PlaylistDetail>()->detail(context(), track("qq", "100"));
        REQUIRE_FALSE(result); CHECK(result.error().code == ErrorCode::not_found);
    }
}

TEST_CASE("QQ album and artist lists use upstream offsets and matching identities", "[providers][qq]") {
    Fixture f;
    SECTION("album tracks") {
        f.upstream->rpc({{"albumMid", album_id}, {"totalNum", 2}, {"songList", Json::array({{{"songInfo", track_json()}}})}});
        CHECK(unwrap(f.port<ports::AlbumTracks>()->list(context(), track("qq", album_id), {1, {}})).next_cursor.has_value());
    }
    SECTION("artist tracks") {
        f.upstream->rpc({{"singerMid", artist_id}, {"totalNum", 2}, {"songList", Json::array({{{"songInfo", track_json()}}})}});
        CHECK(unwrap(f.port<ports::ArtistTracks>()->list(context(), track("qq", artist_id), {1, {}})).next_cursor.has_value());
    }
    SECTION("artist albums") {
        Json album{{"albumMid", album_id}, {"albumName", "专辑"}, {"totalNum", 0}};
        f.upstream->rpc({{"singerMid", artist_id}, {"total", 2}, {"albumList", Json::array({album})}});
        auto result = unwrap(f.port<ports::ArtistAlbums>()->list(context(), track("qq", artist_id), {1, {}}));
        CHECK_FALSE(result.page.items[0].track_count); CHECK(result.next_cursor.has_value());
    }
    auto param = Json::parse(f.upstream->calls.back().body)["req_0"]["param"];
    CHECK(param["begin"] == 0); CHECK(param["num"] == 1);
}

TEST_CASE("QQ lyrics distinguish unavailable lyrics and translated text", "[providers][qq]") {
    Fixture f; Json doc{{"code", 0}, {"lyric", ""}, {"trans", ""}};
    bool translated = false;
    SECTION("absent") {}
    SECTION("translated") { translated = true; doc["trans"] = "[00:01.00]翻译"; }
    f.upstream->raw(doc);
    auto out = unwrap(f.port<ports::ILyrics>()->lyrics(context(), track("qq", song)));
    CHECK(out.unavailable == !translated); CHECK(out.translated_lrc.has_value() == translated);
    CHECK(f.upstream->calls.back().secret_headers.empty());
}

TEST_CASE("QQ resolver uses media mid once and keeps Cookie out of CDN requests", "[providers][qq]") {
    Fixture f; resolve_answers(f, Json::array({vkey_item()}));
    auto out = unwrap(f.port<ports::IAssetResolver>()->resolve_audio(context(), track("qq", song), {}));
    CHECK(out.actual_quality == domain::Quality::standard); CHECK(out.bitrate_bps == 128000);
    CHECK(out.completeness == domain::Completeness::full); CHECK_FALSE(out.shareable); CHECK(out.expires_at.has_value());
    CHECK(out.url.starts_with(std::string{"https://ws.stream.qqmusic.qq.com/M500"} + media + ".mp3?"));
    auto params = Json::parse(f.upstream->calls.back().body)["req_0"]["param"];
    CHECK(params["filename"][0] == std::string{"M500"} + media + ".mp3");
    for (const auto& header : out.request_headers) { CHECK(header.first != "Cookie"); CHECK(header.second.find("synthetic-secret") == std::string::npos); }
}

TEST_CASE("QQ quality fallback is explicit and never substitutes a higher quality", "[providers][qq]") {
    Fixture f; auto port = f.port<ports::IAssetResolver>();
    SECTION("allowed lower") {
        resolve_answers(f, Json::array({vkey_item("F000", 104003), vkey_item("M800", 104003), vkey_item()}));
        auto out = unwrap(port->resolve_audio(context(), track("qq", song), {domain::Quality::lossless, domain::QualityPolicy::allow_lower}));
        CHECK(out.actual_quality == domain::Quality::standard);
    }
    SECTION("strict denied") {
        resolve_answers(f, Json::array({vkey_item("F000", 104003)}));
        auto result = port->resolve_audio(context(), track("qq", song), {domain::Quality::lossless});
        REQUIRE_FALSE(result); CHECK(result.error().code == ErrorCode::credentials_required);
    }
    SECTION("hires unsupported") {
        auto result = port->resolve_audio(context(), track("qq", song), {domain::Quality::hires});
        REQUIRE_FALSE(result); CHECK(result.error().code == ErrorCode::quality_unavailable); CHECK(f.upstream->calls.empty());
    }
}

TEST_CASE("QQ resolver rejects mismatched encrypted and malformed resources", "[providers][qq]") {
    Fixture f; auto candidate = vkey_item();
    SECTION("song mismatch") { candidate["songmid"] = song2; }
    SECTION("filename mismatch") { candidate["filename"] = "M500wrong.mp3"; }
    SECTION("foreign origin") { candidate["purl"] = "https://example.com/audio.mp3"; }
    SECTION("path escape") { candidate["purl"] = "../audio.mp3"; }
    SECTION("encrypted") { candidate["ekey"] = "synthetic-unsupported-encryption"; }
    SECTION("unexpected status") { candidate["result"] = 999; }
    resolve_answers(f, Json::array({candidate}));
    auto result = f.port<ports::IAssetResolver>()->resolve_audio(context(), track("qq", song), {});
    REQUIRE_FALSE(result); CHECK(result.error().code == ErrorCode::upstream_failed);
    CHECK(result.error().message.find("synthetic") == std::string::npos);
}

TEST_CASE("QQ link parsing is offline and rejects ambiguous origins and entities", "[providers][qq]") {
    Fixture f; auto parser = f.port<ports::ILinkParser>();
    CHECK(unwrap(parser->parse_link(std::string{"https://y.qq.com/n/ryqq/songDetail/"} + song)).ref.id.value() == song);
    CHECK(unwrap(parser->parse_link("https://y.qq.com/n/ryqq/playlist/100")).kind == "playlist");
    CHECK(unwrap(parser->parse_link(std::string{"https://y.qq.com/n/yqq/album/"} + album_id + ".html")).kind == "album");
    for (const auto& bad : {std::string{"https://y.qq.com.evil.com/n/ryqq/songDetail/"} + song,
         std::string{"https://y.qq.com:444/n/ryqq/songDetail/"} + song,
         std::string{"https://y.qq.com/n/ryqq/songDetail/"} + song + "?songmid=" + song2,
         std::string{"https://y.qq.com/n/ryqq/songDetail/"} + song + "/extra",
         std::string{"https://y.qq.com/n/ryqq/playlist/0"}}) CHECK_FALSE(parser->parse_link(bad));
    CHECK(f.upstream->calls.empty());
}

TEST_CASE("QQ verifier separates invalid credentials from upstream failure without account disclosure", "[providers][qq]") {
    Fixture f; auto verifier = unwrap(f.registry->require_verifier("qq", "web_cookie", ports::CredentialKind::cookie));
    auto c = context(); ports::SecretBytes secret{fake_cookie};
    SECTION("valid") {
        f.upstream->login(); auto result = unwrap(verifier->verify({c.identity, secret}, c.operation));
        CHECK(result.status == ports::VerificationStatus::valid); CHECK_FALSE(result.account_hint); CHECK(result.scopes.empty());
    }
    SECTION("missing key") {
        ports::SecretBytes bad{"uin=12345"};
        CHECK(unwrap(verifier->verify({c.identity, bad}, c.operation)).status == ports::VerificationStatus::invalid);
        CHECK(f.upstream->calls.empty());
    }
    SECTION("invalid") {
        f.upstream->raw({{"code", 0}, {"req_0", {{"code", 1000}}}});
        CHECK(unwrap(verifier->verify({c.identity, secret}, c.operation)).status == ports::VerificationStatus::invalid);
    }
    SECTION("unknown failure") {
        f.upstream->raw({{"code", 0}, {"req_0", {{"code", 9999}}}});
        auto result = verifier->verify({c.identity, secret}, c.operation);
        REQUIRE_FALSE(result); CHECK(result.error().code == ErrorCode::upstream_failed);
    }
}

TEST_CASE("QQ transport rejects oversized malformed and deep JSON and reports retryable failures", "[providers][qq]") {
    Fixture f; ErrorCode expected = ErrorCode::upstream_failed; bool retryable = false;
    SECTION("deep JSON") { f.upstream->answers.push_back({{200, {}, {}}, std::string(40, '[') + "0" + std::string(40, ']')}); }
    SECTION("oversized JSON") { f.upstream->answers.push_back({{200, {}, {}}, std::string(4 * 1024 * 1024 + 1, ' ')}); }
    SECTION("malformed JSON") { f.upstream->answers.push_back({{200, {}, {}}, "not json"}); }
    SECTION("rate limited") { f.upstream->raw({}, 429); expected = ErrorCode::upstream_rate_limited; retryable = true; }
    SECTION("server error") { f.upstream->raw({}, 503); retryable = true; }
    SECTION("admission full") { f.upstream->deferred = true; expected = ErrorCode::service_busy; retryable = true; }
    auto result = f.port<ports::ICharts>()->charts(context());
    REQUIRE_FALSE(result); CHECK(result.error().code == expected); CHECK(result.error().retryable == retryable);
}
