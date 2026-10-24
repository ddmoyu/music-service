// NetEase protocol package against desensitized responses recorded on 2026-10-22
// (tests/fixtures/providers/netease) and a fake upstream; no network. The opt-in
// [online-netease] case probes the live service.

#include "../domain/test_support.hpp"

#include "application/transport/http_rules.hpp"
#include "application/transport/upstream_client.hpp"
#include "infrastructure/clock.hpp"
#include "infrastructure/config/config_loader.hpp"
#include "infrastructure/transport/httplib_transport.hpp"
#include "providers/netease/bundle.hpp"
#include "providers/netease/capabilities.hpp"
#include "providers/netease/parsing/parsers.hpp"
#include "providers/netease/protocol/requests.hpp"
#include "providers/netease/protocol/weapi.hpp"
#include "providers/registry/provider_registry.hpp"
#include "runtime/admission/admission_ledger.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <map>
#include <deque>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace music_service;
using namespace music_service::test;
namespace ports = application::ports;
namespace netease = providers::netease;
namespace parsing = providers::netease::parsing;
namespace protocol = providers::netease::protocol;
using domain::ErrorCode;

TEST_CASE("netease song tariffs normalize independently of account privileges", "[netease][pay]") {
    using Json = nlohmann::json;
    for (const int fee : {0, 1, 4, 8, 99}) {
        CAPTURE(fee);
        Json song{{"id", 42}, {"name", "test"}, {"fee", fee},
                  {"privilege", {{"payed", 1}, {"pl", 0}, {"dl", 0}}}};
        const domain::TrackPay expected = fee == 0 ? domain::TrackPay{0, 0, 0} :
            fee == 1 ? domain::TrackPay{1, 1, 1} : fee == 4 ? domain::TrackPay{1, 1, 0} :
            fee == 8 ? domain::TrackPay{0, 1, 1} : domain::TrackPay{};
        auto detail = unwrap(parsing::parse_detail(Json{{"code", 200}, {"songs", Json::array({song})}}.dump(), "42"));
        auto list = unwrap(parsing::parse_tracks(Json{{"code", 200}, {"songs", Json::array({song})}}.dump()));
        auto search = unwrap(parsing::parse_search(Json{{"code", 200}, {"result", {{"songCount", 1}, {"songs", Json::array({song})}}}}.dump()));
        for (const auto& actual : {detail.pay, list.front().pay, search.tracks.front().pay}) {
            CHECK(actual.pay_play == expected.pay_play);
            CHECK(actual.pay_down == expected.pay_down);
            CHECK(actual.pay_month == expected.pay_month);
        }
    }
    for (const auto& fee : {Json(nullptr), Json("1"), Json(-1)}) {
        auto result = parsing::parse_detail(Json{{"code", 200}, {"songs", Json::array({{{"id", 42}, {"name", "test"}, {"fee", fee}}})}}.dump(), "42");
        if (fee.is_null()) { REQUIRE(result); CHECK_FALSE(result->pay.pay_play); }
        else { REQUIRE_FALSE(result); CHECK(result.error().code == ErrorCode::upstream_failed); }
    }
}

namespace {

constexpr std::string_view fixed_key = "0123456789abcdef";

std::string fixture(const char* name) {
    std::ifstream file(std::filesystem::path{MUSIC_SERVICE_TEST_FIXTURES} / "providers" / "netease" / name,
                       std::ios::binary);
    REQUIRE(file);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

std::string detail_reason(const domain::Error& error, std::string_view field) {
    for (const auto& detail : error.details) {
        if (detail.field == field) {
            return detail.reason;
        }
    }
    return {};
}

// The exact body a weapi call with this JSON text and the fixed key carries.
std::string weapi_body_of(std::string_view json_text) {
    return protocol::weapi_body(protocol::weapi_encrypt(json_text, fixed_key));
}

class FakeUpstream final : public ports::UpstreamHttp {
public:
    struct Call {
        ports::HttpRequest request;
        ports::AdmissionRequest base;
    };

    void respond(int status, std::string body) {
        answers_.push_back(ports::HttpResponse{{status, {}, {}}, std::move(body)});
    }
    void defer(std::string dimension) { deferred_ = std::move(dimension); }

    domain::Result<ports::HopResult<ports::HttpResponse>> fetch(ports::HttpRequest request,
                                                                const ports::AdmissionRequest& base,
                                                                const ports::OperationContext&) override {
        // Exactly what the real transport would accept.
        REQUIRE(application::transport::check_request(request).has_value());
        calls.push_back({request, base});
        if (deferred_) {
            return ports::HopResult<ports::HttpResponse>{
                ports::PendingHop{std::move(request), {*deferred_, std::chrono::steady_clock::now()}}};
        }
        REQUIRE_FALSE(answers_.empty());
        auto answer = std::move(answers_.front());
        answers_.pop_front();
        return ports::HopResult<ports::HttpResponse>{std::move(answer)};
    }
    domain::Result<ports::HopResult<ports::HttpResponseHead>> stream(ports::HttpRequest, const ports::AdmissionRequest&,
                                                                     const ports::OperationContext&,
                                                                     const ports::HeadConsumer&,
                                                                     const ports::ChunkConsumer&) override {
        return domain::Error{ErrorCode::internal, "not used", false, {}};
    }

    std::vector<Call> calls;

private:
    std::deque<ports::HttpResponse> answers_;
    std::optional<std::string> deferred_;
};

ports::ProviderContext anonymous_context() {
    return {{std::chrono::steady_clock::now() + std::chrono::seconds(10), {}},
            unwrap(domain::AuthIdentityKey::make(anonymous("prn_a", "netease"), {})),
            std::nullopt};
}

domain::AuthIdentityKey user_identity() {
    return unwrap(domain::AuthIdentityKey::make(user("prn_a", "netease", "cre_1"),
                                                {1, std::nullopt, std::nullopt, std::nullopt}));
}

ports::ProviderContext user_context(std::string cookie) {
    return {{std::chrono::steady_clock::now() + std::chrono::seconds(10), {}},
            user_identity(),
            std::nullopt,
            std::make_shared<const ports::SecretBytes>(std::move(cookie))};
}

ports::ProviderSetup setup(std::shared_ptr<ports::UpstreamHttp> upstream) {
    return {source("netease"), "{}", std::move(upstream), "netease_web"};
}

const std::string* header(const std::vector<ports::HttpHeader>& headers, std::string_view name) {
    for (const auto& h : headers) {
        if (h.name == name) {
            return &h.value;
        }
    }
    return nullptr;
}

// Pins the weapi key for every capability made in this test; restores randomness after.
struct FixedKey {
    FixedKey() {
        netease::set_secret_key_source_for_testing([] { return std::string{fixed_key}; });
    }
    ~FixedKey() { netease::set_secret_key_source_for_testing({}); }
};

} // namespace

TEST_CASE("weapi matches the web client's encryption", "[providers][netease]") {
    // Reference values computed independently (Python, cryptography 50) on 2026-10-22.
    const auto form = protocol::weapi_encrypt(
        R"({"ids":"[5257138]","level":"standard","encodeType":"flac","csrf_token":""})", fixed_key);
    CHECK(form.params == "k4ZDLhIDnMAMrSesw4JbrDqo++jM0jkZV5TSx0gLLGe+ixX+eWMQFe5KWDYxF3Q37iywwRWCJvV7nAvA3NVM04hW1E8SWqDll"
                         "FNplfbuE19QSoIgF/2UgaLvqhSm09dKG/hCQIVzz7tQgjXzQEcOyw==");
    CHECK(form.enc_sec_key ==
          "35701388baf89fed412e11269b9c76625d095ecaf17f03fa018abe19ea2d38b949debf242ee39a71ca1f6cda71b1b86a45aa909ee27f7"
          "e78e267d34e732f0de948206c3340a788d0003372183e2f753c1f78b66ac23d134ac1fc9b993156520ea826b8aa89a962d4491b4b8d7e"
          "08738e1da9b07aa39bf4a7ef0b1c210728cd52");
    // The form body percent-encodes base64's + / =.
    const auto body = protocol::weapi_body(form);
    CHECK(body.starts_with("params=k4ZDLhIDnMAMrSesw4JbrDqo%2B%2BjM0"));
    CHECK(body.find("&encSecKey=35701388") != std::string::npos);

    const auto key = protocol::random_secret_key();
    CHECK(key.size() == 16);
    CHECK(std::all_of(key.begin(), key.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }));
}

TEST_CASE("netease requests are weapi POSTs to music.163.com", "[providers][netease]") {
    const protocol::Call anonymous{std::string{fixed_key}, ""};
    const auto search = protocol::search_tracks("晴天", 30, 10, anonymous);
    CHECK(search.method == ports::HttpMethod::post);
    CHECK(search.url == "https://music.163.com/weapi/cloudsearch/pc?csrf_token=");
    CHECK(search.operation == "search_tracks");
    CHECK(search.body ==
          weapi_body_of(R"({"s":"晴天","type":1,"limit":10,"offset":30,"total":true,"csrf_token":""})"));
    REQUIRE(header(search.headers, "Referer"));
    CHECK(*header(search.headers, "Content-Type") == "application/x-www-form-urlencoded");
    CHECK(search.secret_headers.empty());

    CHECK(protocol::song_detail("5257138", anonymous).body ==
          weapi_body_of(R"({"c":"[{\"id\":5257138}]","csrf_token":""})"));
    CHECK(protocol::song_url("5257138", "standard", anonymous).body ==
          weapi_body_of(R"({"ids":"[5257138]","level":"standard","encodeType":"flac","csrf_token":""})"));
    CHECK(protocol::lyric("5257138", anonymous).body ==
          weapi_body_of(R"({"id":5257138,"lv":-1,"kv":-1,"tv":-1,"rv":-1,"yv":-1,"csrf_token":""})"));

    // A signed-in call carries the csrf token in the URL and the payload.
    const auto signed_in = protocol::account({std::string{fixed_key}, "abc123"});
    CHECK(signed_in.url == "https://music.163.com/weapi/w/nuser/account/get?csrf_token=abc123");
    CHECK(signed_in.body == weapi_body_of(R"({"csrf_token":"abc123"})"));
}

TEST_CASE("cookies are read and passed only as a secret header", "[providers][netease]") {
    const std::string cookie = " MUSIC_U=u1; __csrf=c2 ;NMTID=n3; junk ";
    CHECK(protocol::music_u_of(cookie) == "u1");
    CHECK(protocol::csrf_of(cookie) == "c2");
    CHECK_FALSE(protocol::music_u_of("__csrf=c2"));
    CHECK_FALSE(protocol::music_u_of("MUSIC_U="));

    auto request = protocol::account({std::string{fixed_key}, "c2"});
    protocol::with_cookie(request, cookie);
    REQUIRE(request.secret_headers.size() == 1);
    CHECK(request.secret_headers[0].name == "Cookie");
    CHECK(request.secret_headers[0].value == "MUSIC_U=u1; __csrf=c2; NMTID=n3");
    CHECK_FALSE(header(request.headers, "Cookie"));
    CHECK(application::transport::check_request(request));
}

TEST_CASE("netease search maps songs and their counts", "[providers][netease]") {
    const auto page = unwrap(parsing::parse_search(fixture("search-qingtian-o0-l3.json")));
    CHECK(page.total > 0);
    REQUIRE(page.tracks.size() == 3);
    for (const auto& track : page.tracks) {
        CHECK(track.ref.source.value() == "netease");
        CHECK(parsing::is_valid_song_id(track.ref.id.value()));
        CHECK_FALSE(track.title.empty());
        CHECK_FALSE(track.artists.empty());
        CHECK(track.duration_ms);
        REQUIRE(track.album);
        REQUIRE(track.album->ref);
        CHECK(track.album->ref->source.value() == "netease");
    }

    const auto empty = unwrap(parsing::parse_search(fixture("search-empty.json")));
    CHECK(empty.tracks.empty());
    CHECK(empty.total == 0);

    CHECK(parsing::parse_search("[]").error().code == ErrorCode::upstream_failed);
    CHECK(detail_reason(parsing::parse_search(R"({"code":200})").error(), "protocol") == "search has no result");
    CHECK(detail_reason(parsing::parse_search(R"({"code":200,"result":{"songCount":1,"songs":[{"name":"x"}]}})")
                            .error(),
                        "protocol") == "song has no id or name");
}

TEST_CASE("netease top-level codes become service errors", "[providers][netease]") {
    const auto login = parsing::parse_search(fixture("needs-login-synthetic.json"));
    CHECK(login.error().code == ErrorCode::credentials_required);
    const auto limited = parsing::parse_search(fixture("rate-limited-synthetic.json"));
    CHECK(limited.error().code == ErrorCode::upstream_rate_limited);
    CHECK(limited.error().retryable);
    const auto risk = parsing::parse_search(R"({"code":-460,"message":"Cheating"})");
    CHECK(risk.error().code == ErrorCode::upstream_failed);
    CHECK_FALSE(risk.error().retryable);
    CHECK(detail_reason(risk.error(), "netease_code") == "-460");
    // cloudsearch/get/web refuses weapi with this code; it is not a result.
    CHECK(detail_reason(parsing::parse_search(R"({"code":50000005})").error(), "netease_code") == "50000005");
}

TEST_CASE("netease details are exactly the requested song", "[providers][netease]") {
    const auto track = unwrap(parsing::parse_detail(fixture("detail-5257138.json"), "5257138"));
    CHECK(track.ref == test::track("netease", "5257138"));
    CHECK(track.title == "屋顶");
    REQUIRE(track.artists.size() == 3);
    CHECK(track.artists[0].name == "周杰伦");
    CHECK(track.artists[0].ref == domain::PlatformRef{source("netease"), unwrap(domain::PlatformId::parse("6452"))});
    REQUIRE(track.album);
    CHECK(track.album->ref->id.value() == "512175");
    CHECK(track.duration_ms == 319039);
    CHECK(track.artwork == "https://p1.music.126.net/81BsxxhomJ4aJZYvEbyPkw==/109951165671182684.jpg");
    CHECK(track.track_number == 1);
    CHECK_FALSE(track.track_total);
    CHECK(track.version.kind == domain::VersionKind::unknown);

    CHECK(parsing::parse_detail(fixture("detail-missing.json"), "99999999999").error().code == ErrorCode::not_found);
    CHECK(detail_reason(parsing::parse_detail(fixture("detail-5257138.json"), "186016").error(), "protocol") ==
          "detail is about another song");
}

TEST_CASE("netease optional metadata comes only from valid album and track fields", "[providers][netease][output]") {
    auto doc = nlohmann::json::parse(fixture("detail-5257138.json"));
    auto& song = doc["songs"][0];
    song["no"] = 3;
    song["al"]["size"] = 12;
    auto parsed = unwrap(parsing::parse_detail(doc.dump(), "5257138"));
    CHECK(parsed.track_number == 3);
    CHECK(parsed.track_total == 12);
    song["no"] = 0;
    song["al"]["size"] = -1;
    song["al"]["picUrl"] = "https://p1.music.126.net.attacker.invalid/a.jpg";
    parsed = unwrap(parsing::parse_detail(doc.dump(), "5257138"));
    CHECK_FALSE(parsed.track_number);
    CHECK_FALSE(parsed.track_total);
    CHECK_FALSE(parsed.artwork);
    song["no"] = 14;
    song["al"]["size"] = 12;
    parsed = unwrap(parsing::parse_detail(doc.dump(), "5257138"));
    CHECK(parsed.track_number == 14);
    CHECK_FALSE(parsed.track_total);
}

TEST_CASE("netease collection fixtures preserve declared metadata and ordered content", "[netease][collections-b]") {
    const auto p = unwrap(parsing::parse_playlist(fixture("collection-playlist.json"), "3778678"));
    CHECK(p.detail.name == "热歌榜");
    CHECK(p.detail.track_count == 200);
    CHECK(p.ids.size() == 200);
    CHECK(p.detail.creator);
    CHECK(p.detail.artwork);
    CHECK(p.detail.updated_at);
    const auto a = unwrap(parsing::parse_album(fixture("collection-album.json"), "512175"));
    CHECK(a.detail.track_count == 26);
    CHECK(a.tracks.size() == 26);
    CHECK(a.detail.release_date);
    CHECK(a.detail.description);
    CHECK(a.detail.artwork);
    CHECK_FALSE(a.detail.artists.empty());
    CHECK(unwrap(parsing::parse_playlist_search(fixture("collection-search-playlists.json"))).items.size() == 2);
    CHECK(unwrap(parsing::parse_album_search(fixture("collection-search-albums.json"))).items.size() == 2);
    CHECK_FALSE(parsing::parse_playlist(fixture("collection-playlist.json"), "1"));
    CHECK_FALSE(parsing::parse_album(fixture("collection-album.json"), "1"));
}

TEST_CASE("netease collections reject failures and malformed responses without inventing empty success", "[netease][collections-b]") {
    using nlohmann::json;
    for (const auto [code, expected] : {std::pair{301, ErrorCode::credentials_required}, {403, ErrorCode::credentials_required},
         {404, ErrorCode::not_found}, {405, ErrorCode::upstream_rate_limited}, {500, ErrorCode::upstream_failed}}) {
        const auto body = json{{"code", code}}.dump();
        CHECK(parsing::parse_playlist(body, "1").error().code == expected);
        CHECK(parsing::parse_album(body, "1").error().code == expected);
        CHECK(parsing::parse_playlist_search(body).error().code == expected);
        CHECK(parsing::parse_album_search(body).error().code == expected);
        CHECK(parsing::parse_tracks(body).error().code == expected);
    }
    CHECK_FALSE(parsing::parse_playlist(R"({"code":200,"playlist":{"id":1,"name":"x"}})", "1"));
    CHECK_FALSE(parsing::parse_album(R"({"code":200,"album":{"id":1,"name":"x"}})", "1"));
    CHECK_FALSE(parsing::parse_playlist_search(R"({"code":200,"result":{"playlists":[]}})"));
    CHECK_FALSE(parsing::parse_album_search(R"({"code":200,"result":{"albumCount":1}})"));
    CHECK(unwrap(parsing::parse_playlist_search(R"({"code":200,"result":{"playlistCount":0}})")).items.empty());
    CHECK(unwrap(parsing::parse_album_search(R"({"code":200,"result":{"albumCount":0,"albums":[]}})")).items.empty());
    CHECK(unwrap(parsing::parse_playlist(R"({"code":200,"playlist":{"id":1,"name":"x","trackCount":0,"trackIds":[]}})", "1")).ids.empty());
    CHECK(unwrap(parsing::parse_album(R"({"code":200,"album":{"id":1,"name":"x","size":0},"songs":[]})", "1")).tracks.empty());
    auto p = json::parse(fixture("collection-playlist.json"));
    p["playlist"]["trackIds"] = json::array();
    for (int i = 1; i <= 10001; ++i) p["playlist"]["trackIds"].push_back({{"id", i}});
    CHECK_FALSE(parsing::parse_playlist(p.dump(), "3778678"));
    auto a = json::parse(fixture("collection-album.json"));
    const auto song = a["songs"][0];
    a["songs"] = std::vector<json>(2001, song);
    CHECK_FALSE(parsing::parse_album(a.dump(), "512175"));
}

TEST_CASE("netease pages a 507-position playlist with bounded detail requests and honest missing counts", "[netease][collections-b]") {
    using nlohmann::json;
    FixedKey fixed;
    auto upstream = std::make_shared<FakeUpstream>();
    auto list = unwrap(netease::make_playlist_tracks(setup(upstream)));
    json ids = json::array();
    for (int i = 1; i <= 507; ++i) ids.push_back({{"id", i == 506 ? 1 : i}});
    const auto body = json{{"code", 200}, {"playlist", {{"id", 1}, {"name", "large"}, {"trackCount", 600}, {"trackIds", ids}}}}.dump();
    auto song = json::parse(fixture("detail-5257138.json"))["songs"][0];
    std::optional<std::string> cursor;
    std::vector<int> actual;
    for (std::size_t offset = 0; offset < 507; offset += 100) {
        upstream->respond(200, body);
        const auto end = std::min(offset + 100, std::size_t{507});
        std::size_t missing = 0;
        const auto first_call = upstream->calls.size();
        std::vector<std::string> expected_bodies;
        for (auto at = offset; at < end; at += 50) {
            auto songs = json::array();
            auto requested = json::array();
            for (auto i = at; i < std::min(at + 50, end); ++i) {
                const int id = ids[i]["id"];
                requested.push_back({{"id", id}});
                if (id == 100 || (id >= 201 && id <= 300)) { ++missing; continue; }
                auto s = song; s["id"] = id;
                songs.insert(songs.begin(), s); // 上游乱序，Provider 恢复 ID 清单顺序
            }
            upstream->respond(200, json{{"code", 200}, {"songs", songs}}.dump());
            expected_bodies.push_back(weapi_body_of(json{{"c", requested.dump()}, {"csrf_token", ""}}.dump()));
        }
        const auto page = unwrap(list->list(anonymous_context(), test::track("netease", "1"), {100, cursor}));
        CHECK(page.page.page.total == 600);
        CHECK(page.page.page.listed_total == 507);
        CHECK(page.page.page.unavailable_count == missing);
        CHECK(page.page.page.returned == end - offset - missing);
        CHECK(page.page.page.has_more == (end < 507));
        for (const auto& item : page.page.items) actual.push_back(std::stoi(item.ref.id.value()));
        REQUIRE(upstream->calls.size() == first_call + 1 + expected_bodies.size());
        CHECK(upstream->calls[first_call].request.max_response_bytes == 4 * 1024 * 1024);
        for (std::size_t n = 0; n < expected_bodies.size(); ++n) {
            const auto& call = upstream->calls[first_call + 1 + n];
            CHECK(call.request.body == expected_bodies[n]);
            CHECK(call.request.max_response_bytes == 2 * 1024 * 1024);
            CHECK(call.request.secret_headers.empty());
        }
        cursor = page.next_cursor;
    }
    CHECK_FALSE(cursor);
    std::vector<int> expected;
    for (const auto& v : ids) { const int id = v["id"]; if (id != 100 && (id < 201 || id > 300)) expected.push_back(id); }
    CHECK(actual == expected);
}

TEST_CASE("netease collection cursor expires on changed order and album pagination stays ordered", "[netease][collections-b]") {
    using nlohmann::json;
    auto upstream = std::make_shared<FakeUpstream>();
    auto list = unwrap(netease::make_album_tracks(setup(upstream)));
    const auto ref = test::track("netease", "512175");
    const auto body = fixture("collection-album.json");
    upstream->respond(200, body);
    const auto first = unwrap(list->list(anonymous_context(), ref, {10, {}}));
    REQUIRE(first.next_cursor);
    upstream->respond(200, body);
    const auto second = unwrap(list->list(anonymous_context(), ref, {10, first.next_cursor}));
    upstream->respond(200, body);
    const auto last = unwrap(list->list(anonymous_context(), ref, {10, second.next_cursor}));
    CHECK(first.page.items.size() == 10); CHECK(second.page.items.size() == 10); CHECK(last.page.items.size() == 6);
    CHECK_FALSE(last.next_cursor);
    auto changed = json::parse(body);
    std::swap(changed["songs"][0], changed["songs"][1]);
    upstream->respond(200, changed.dump());
    CHECK(list->list(anonymous_context(), ref, {10, first.next_cursor}).error().code == ErrorCode::cursor_expired);
    CHECK_FALSE(list->list(anonymous_context(), ref, {101, {}}));
    CHECK_FALSE(list->list(anonymous_context(), test::track("kuwo", "1"), {10, {}}));
    CHECK_FALSE(list->list(anonymous_context(), test::track("netease", "invalid"), {10, {}}));
}

TEST_CASE("netease collection factories search and detail through shared admission", "[netease][collections-b]") {
    FixedKey fixed;
    auto upstream = std::make_shared<FakeUpstream>();
    const auto s = setup(upstream);
    upstream->respond(200, fixture("collection-search-playlists.json"));
    auto ps = unwrap(netease::make_playlist_search(s));
    const auto p = unwrap(ps->search(anonymous_context(), {"周杰伦", 2, {}}));
    CHECK(p.next_cursor == "2");
    upstream->respond(200, fixture("collection-search-playlists.json"));
    CHECK(ps->search(anonymous_context(), {"周杰伦", 2, p.next_cursor}));
    CHECK(upstream->calls.back().request.body == weapi_body_of(R"({"s":"周杰伦","type":1000,"limit":2,"offset":2,"total":true,"csrf_token":""})"));
    upstream->respond(200, fixture("collection-search-albums.json"));
    CHECK(unwrap(netease::make_album_search(s))->search(anonymous_context(), {"叶惠美", 2, {}}));
    upstream->respond(200, fixture("collection-playlist.json"));
    CHECK(unwrap(netease::make_playlist_detail(s))->detail(anonymous_context(), test::track("netease", "3778678")));
    upstream->respond(200, fixture("collection-album.json"));
    CHECK(unwrap(netease::make_album_detail(s))->detail(anonymous_context(), test::track("netease", "512175")));
    for (const auto& call : upstream->calls) {
        CHECK(call.request.secret_headers.empty());
        CHECK(call.base.traffic_class == ports::TrafficClass::short_request);
        CHECK(call.base.rate_limit_group == "netease_web");
    }
    CHECK_FALSE(ps->search(anonymous_context(), {"x", 51, {}}));
    CHECK_FALSE(ps->search(anonymous_context(), {"", 2, {}}));
    CHECK_FALSE(ps->search(anonymous_context(), {"x", 2, "junk"}));
}

TEST_CASE("netease collection links preserve the entity kind", "[netease][collections-b]") {
    for (const auto* url : {"https://music.163.com/playlist?id=1", "http://music.163.com/#/playlist?id=1&userid=2",
                           "https://y.music.163.com/m/playlist?id=1", "https://music.163.com/playlist/1"}) {
        const auto link = unwrap(parsing::parse_link(url)); CHECK(link.kind == "playlist"); CHECK(link.id == "1");
    }
    CHECK(unwrap(parsing::parse_link("https://music.163.com/#/album?id=1")).kind == "album");
    CHECK(unwrap(parsing::parse_link("https://music.163.com/song?id=1")).kind == "track");
    for (const auto* url : {"https://music.163.com.evil/playlist?id=1", "https://163cn.tv/abc", "https://music.163.com/artist?id=01",
                           "https://music.163.com/album?id=01", "https://music.163.com/album/1/extra",
                           "https://music.163.com:9999/album?id=1", "https://music.163.com/album?id=1&id=2"}) CHECK_FALSE(parsing::parse_link(url));
}

TEST_CASE("netease player urls give evidence about the audio", "[providers][netease]") {
    const auto standard = unwrap(parsing::parse_audio(fixture("url-5257138-standard.json"), "5257138"));
    CHECK(standard.code == 200);
    // The CDN address is used over HTTPS only.
    CHECK(standard.url.starts_with("https://m801.music.126.net/"));
    CHECK(standard.container == "mp3");
    CHECK(standard.level == "standard");
    CHECK(standard.bitrate_bps == 128001);
    CHECK(standard.size_bytes == 5105833);
    CHECK(standard.duration_ms == 319039);
    CHECK(standard.expires_in_seconds == 1200);
    CHECK(standard.md5 == "9e257ff0610f84e4242cf0127dac6005");
    CHECK_FALSE(standard.trial);

    CHECK(unwrap(parsing::parse_audio(fixture("url-210049-vip.json"), "210049")).code == -110);
    CHECK(unwrap(parsing::parse_audio(fixture("url-186016-nocopyright.json"), "186016")).code == 404);
    CHECK(unwrap(parsing::parse_audio(fixture("url-missing.json"), "99999999999")).code == 404);

    CHECK(detail_reason(parsing::parse_audio(fixture("url-5257138-standard.json"), "1").error(), "protocol") ==
          "player url is about another song");
    CHECK(detail_reason(parsing::parse_audio(
                            R"({"code":200,"data":[{"id":1,"code":200,"url":"https://evil.example/a.mp3"}]})", "1")
                            .error(),
                        "protocol") == "player url is not on the netease CDN");
    const auto trial = unwrap(parsing::parse_audio(
        R"({"code":200,"data":[{"id":1,"code":200,"url":"http://m701.music.126.net/a.mp3","type":"mp3","level":"standard","br":128000,"time":30000,"freeTrialInfo":{"start":0,"end":30}}]})",
        "1"));
    CHECK(trial.trial);
}

TEST_CASE("netease lyrics, translations and pure music", "[providers][netease]") {
    const auto plain = unwrap(parsing::parse_lyrics(fixture("lyric-5257138.json")));
    CHECK_FALSE(plain.unavailable);
    REQUIRE(plain.lrc);
    CHECK(plain.lrc->find("[00:") != std::string::npos);

    const auto translated = unwrap(parsing::parse_lyrics(fixture("lyric-400876427-translated.json")));
    REQUIRE(translated.lrc);
    CHECK(translated.translated_lrc);

    // The "纯音乐，请欣赏" placeholder is not returned as lyrics.
    const auto pure = unwrap(parsing::parse_lyrics(fixture("lyric-5276808-puremusic.json")));
    CHECK(pure.unavailable);
    CHECK_FALSE(pure.lrc);
    CHECK(unwrap(parsing::parse_lyrics(R"({"code":200,"nolyric":true})")).unavailable);
    CHECK(unwrap(parsing::parse_lyrics(R"({"code":200,"lrc":{"lyric":"  "}})")).unavailable);
}

TEST_CASE("netease accounts: signed in or nobody", "[providers][netease]") {
    const auto nobody = unwrap(parsing::parse_account(fixture("account-anonymous.json")));
    CHECK_FALSE(nobody.signed_in);
    const auto user = unwrap(parsing::parse_account(fixture("account-user-synthetic.json")));
    CHECK(user.signed_in);
    CHECK(user.nickname == "测试用户");
    CHECK(user.vip_type == 11);
    CHECK(parsing::parse_account(R"({"code":200,"account":{"name":"x"}})").error().code == ErrorCode::upstream_failed);
}

TEST_CASE("netease song links", "[providers][netease]") {
    for (const auto* link : {"https://music.163.com/song?id=5257138", "https://music.163.com/#/song?id=5257138",
                             "http://music.163.com/song?id=5257138&userid=1", "https://music.163.com/song/5257138/",
                             "https://y.music.163.com/m/song?id=5257138&uct2=x", "https://music.163.com/m/song?id=5257138"}) {
        INFO(link);
        CHECK(unwrap(parsing::parse_track_link(link)) == "5257138");
    }
    for (const auto* link : {"https://music.163.com/playlist?id=1", "https://music.163.com/song?id=01",
                             "https://music.163.com/song?ids=1", "https://163cn.tv/abc", "https://evil.com/song?id=1",
                             "https://music.163.com.evil.com/song?id=1", "music.163.com/song?id=1"}) {
        INFO(link);
        CHECK(parsing::parse_track_link(link).error().code == ErrorCode::validation_failed);
    }
}

TEST_CASE("netease capabilities call the right endpoints", "[providers][netease]") {
    FixedKey key;
    auto upstream = std::make_shared<FakeUpstream>();
    const auto s = setup(upstream);

    auto search = unwrap(netease::make_search(s));
    upstream->respond(200, fixture("search-qingtian-o0-l3.json"));
    const auto page = unwrap(search->search_tracks(anonymous_context(), {"晴天", 3, std::nullopt}));
    CHECK(page.page.items.size() == 3);
    CHECK(page.next_cursor == "3");
    REQUIRE(upstream->calls.size() == 1);
    CHECK(upstream->calls[0].base.source.value() == "netease");
    CHECK(upstream->calls[0].base.rate_limit_group == "netease_web");
    CHECK(upstream->calls[0].request.secret_headers.empty());

    upstream->respond(200, fixture("search-empty.json"));
    const auto last = unwrap(search->search_tracks(anonymous_context(), {"晴天", 3, std::string{"3"}}));
    CHECK(last.page.items.empty());
    CHECK_FALSE(last.next_cursor);
    CHECK(upstream->calls[1].request.body ==
          weapi_body_of(R"({"s":"晴天","type":1,"limit":3,"offset":3,"total":true,"csrf_token":""})"));

    const auto calls = upstream->calls.size();
    CHECK(search->search_tracks(anonymous_context(), {"晴天", 3, std::string{"x"}}).error().code ==
          ErrorCode::cursor_mismatch);
    CHECK(search->search_tracks(anonymous_context(), {"", 3, std::nullopt}).error().code ==
          ErrorCode::validation_failed);
    CHECK(search->search_tracks(anonymous_context(), {"晴天", 51, std::nullopt}).error().code ==
          ErrorCode::validation_failed);
    CHECK(upstream->calls.size() == calls);

    auto detail = unwrap(netease::make_detail(s));
    upstream->respond(200, fixture("detail-5257138.json"));
    CHECK(unwrap(detail->track_detail(anonymous_context(), test::track("netease", "5257138"))).title == "屋顶");
    CHECK(detail->track_detail(anonymous_context(), test::track("kuwo", "5257138")).error().code ==
          ErrorCode::validation_failed);
    CHECK(detail->track_detail(anonymous_context(), test::track("netease", "abc")).error().code ==
          ErrorCode::validation_failed);

    auto lyrics = unwrap(netease::make_lyrics(s));
    upstream->respond(200, fixture("lyric-5276808-puremusic.json"));
    CHECK(unwrap(lyrics->lyrics(anonymous_context(), test::track("netease", "5276808"))).unavailable);

    // HTTP errors and admission.
    upstream->respond(503, "");
    const auto busy = detail->track_detail(anonymous_context(), test::track("netease", "5257138"));
    CHECK(busy.error().code == ErrorCode::upstream_failed);
    CHECK(busy.error().retryable);
    upstream->defer("source");
    const auto deferred = detail->track_detail(anonymous_context(), test::track("netease", "5257138"));
    CHECK(deferred.error().code == ErrorCode::service_busy);
    CHECK(detail_reason(deferred.error(), "admission") == "source");
}

TEST_CASE("netease audio: standard and access evidence", "[providers][netease]") {
    FixedKey key;
    auto upstream = std::make_shared<FakeUpstream>();
    auto resolver = unwrap(netease::make_resolver(setup(upstream)));

    upstream->respond(200, fixture("url-5257138-standard.json"));
    const auto audio = unwrap(resolver->resolve_audio(anonymous_context(), test::track("netease", "5257138"), {}));
    CHECK(audio.actual_track == test::track("netease", "5257138"));
    CHECK(audio.actual_quality == domain::Quality::standard);
    CHECK(audio.completeness == domain::Completeness::full);
    CHECK(audio.container == "mp3");
    CHECK(audio.bitrate_bps == 128001);
    CHECK(audio.duration_ms == 319039);
    CHECK(audio.url.starts_with("https://"));
    REQUIRE(audio.expires_at);
    CHECK(*audio.expires_at > std::chrono::system_clock::now() + std::chrono::minutes(19));
    CHECK_FALSE(audio.shareable);
    CHECK(upstream->calls.back().request.secret_headers.empty()); // anonymous: no cookie

    // Hi-Res 请求必须实际发送；上游降级不能满足 strict。
    const auto calls = upstream->calls.size();
    upstream->respond(200, fixture("url-5257138-standard.json"));
    const auto strict_high =
        unwrap(resolver->resolve_audio(anonymous_context(), test::track("netease", "5257138"),
                                {domain::Quality::hires, domain::QualityPolicy::strict, false}));
    CHECK(domain::evaluate_resource({domain::Quality::hires, domain::QualityPolicy::strict, false},
        {strict_high.actual_quality, strict_high.completeness}).error().code == ErrorCode::quality_unavailable);
    CHECK(upstream->calls.size() == calls + 1);

    upstream->respond(200, fixture("url-210049-vip.json"));
    const auto vip = resolver->resolve_audio(anonymous_context(), test::track("netease", "210049"), {});
    CHECK(vip.error().code == ErrorCode::credentials_required);
    CHECK(detail_reason(vip.error(), "netease_song_code") == "-110");

    upstream->respond(200, fixture("url-186016-nocopyright.json"));
    CHECK(resolver->resolve_audio(anonymous_context(), test::track("netease", "186016"), {}).error().code ==
          ErrorCode::not_found);

    // With an account the cookie goes along as a secret header, with its csrf token.
    upstream->respond(200, fixture("url-5257138-standard.json"));
    unwrap(resolver->resolve_audio(user_context("MUSIC_U=secret-u; __csrf=tok"), test::track("netease", "5257138"),
                                   {}));
    const auto& sent = upstream->calls.back();
    REQUIRE(sent.request.secret_headers.size() == 1);
    CHECK(sent.request.secret_headers[0].value == "MUSIC_U=secret-u; __csrf=tok");
    CHECK(sent.request.url.ends_with("?csrf_token=tok"));
    CHECK(sent.request.body.find("secret-u") == std::string::npos);
    CHECK(sent.base.identity == user_identity());

    // A trial clip for an account without access is a preview, never the song.
    upstream->respond(
        200,
        R"({"code":200,"data":[{"id":210049,"code":200,"url":"http://m701.music.126.net/a.mp3","type":"mp3","level":"standard","br":128000,"time":30000,"expi":1200,"freeTrialInfo":{"start":0,"end":30}}]})");
    const auto trial = unwrap(
        resolver->resolve_audio(user_context("MUSIC_U=free-user"), test::track("netease", "210049"), {}));
    CHECK(trial.completeness == domain::Completeness::preview);
    CHECK_FALSE(trial.actual_quality);
}

TEST_CASE("netease cookie verification", "[providers][netease]") {
    FixedKey key;
    auto upstream = std::make_shared<FakeUpstream>();
    auto verifier = unwrap(netease::make_verifier(setup(upstream)));
    const ports::OperationContext operation{std::chrono::steady_clock::now() + std::chrono::seconds(10), {}};
    const auto identity = user_identity();

    // No MUSIC_U: invalid without asking the platform.
    const ports::SecretBytes no_session{std::string_view{"NMTID=abc"}};
    CHECK(unwrap(verifier->verify({identity, no_session}, operation)).status == ports::VerificationStatus::invalid);
    CHECK(upstream->calls.empty());

    const ports::SecretBytes cookie{std::string_view{"MUSIC_U=session; __csrf=tok"}};
    upstream->respond(200, fixture("account-user-synthetic.json"));
    upstream->respond(200, R"({"code":200,"data":{"redplus":{"expireTime":0},"associator":{"expireTime":4102444800000},"musicPackage":{"expireTime":4102444800000}}})");
    const auto valid = unwrap(verifier->verify({identity, cookie}, operation));
    CHECK(valid.status == ports::VerificationStatus::valid);
    CHECK(valid.account_hint == "测试用户");
    CHECK(valid.scopes.empty());
    CHECK(valid.membership.type == "vip");
    CHECK(valid.membership.name == "黑胶VIP");
    REQUIRE(upstream->calls.size() == 2);
    CHECK_FALSE(upstream->calls[1].request.allow_redirects);
    CHECK(upstream->calls[1].request.url.find("music-vip-membership/front/vip/info") != std::string::npos);
    CHECK(upstream->calls[0].request.operation == "verify_credentials");
    CHECK(upstream->calls[0].request.secret_headers.at(0).value == "MUSIC_U=session; __csrf=tok");

    // A session the platform no longer knows answers account null.
    upstream->respond(200, fixture("account-anonymous.json"));
    CHECK(unwrap(verifier->verify({identity, cookie}, operation)).status == ports::VerificationStatus::invalid);

    upstream->respond(503, "");
    CHECK(verifier->verify({identity, cookie}, operation).error().retryable);
}

TEST_CASE("the netease bundle registers its capabilities and cookie profile", "[providers][netease]") {
    auto bundle = netease::make_bundle();
    CHECK(bundle.source.value() == "netease");
    std::map<std::string, application::config::ProviderConfig> config;
    config["netease"] = {};
    std::vector<ports::ProviderBundle> bundles;
    bundles.push_back(netease::make_bundle());
    auto registry = unwrap(providers::ProviderRegistry::build(std::move(bundles), config));
    const auto view = unwrap(registry->describe("netease"));
    std::vector<std::string> keys;
    for (const auto& capability : view.capabilities) {
        keys.push_back(capability.spec.key);
    }
    for (const auto* key : {"search_tracks", "track_detail", "parse_links", "lyrics", "resolve_audio",
                            "verify_credentials", "categories", "category_playlists", "recommend_playlists",
                            "charts", "chart_detail", "chart_tracks", "user_playlists", "artist_tracks",
                            "podcast_detail", "podcast_episodes", "episode_detail"}) {
        INFO(key);
        CHECK(std::find(keys.begin(), keys.end(), key) != keys.end());
    }
    REQUIRE(view.auth_methods.size() == 1);
    CHECK(view.auth_methods[0].auth_profile == "web_cookie");
    CHECK(view.auth_methods[0].kind == ports::CredentialKind::cookie);
    CHECK(view.auth_methods[0].supports_verify);
    CHECK(bundle.quality_hints == std::vector<domain::Quality>{domain::Quality::standard, domain::Quality::high,
                                                            domain::Quality::lossless, domain::Quality::hires});
}

TEST_CASE("netease C quality uses response evidence and shared downgrade policy", "[providers][netease][stage-c]") {
    FixedKey key;
    auto upstream = std::make_shared<FakeUpstream>();
    auto resolver = unwrap(netease::make_resolver(setup(upstream)));
    auto response = nlohmann::json::parse(fixture("url-5257138-standard.json"));
    auto& row = response["data"][0];
    const domain::ResourceRequirement high{domain::Quality::high, domain::QualityPolicy::strict, false};
    const domain::ResourceRequirement lossless{domain::Quality::lossless, domain::QualityPolicy::strict, false};
    auto facts = [](const ports::ResolvedAudio& a) { return domain::ResourceFacts{a.actual_quality, a.completeness}; };
    row["level"] = "exhigh"; row["br"] = 320001;
    upstream->respond(200, response.dump());
    const auto mp3 = unwrap(resolver->resolve_audio(anonymous_context(), test::track("netease", "5257138"), high));
    CHECK(mp3.actual_quality == domain::Quality::high);
    CHECK(unwrap(domain::evaluate_resource(high, facts(mp3))).warnings.empty());
    CHECK(upstream->calls.back().request.body == weapi_body_of(
        R"({"ids":"[5257138]","level":"exhigh","encodeType":"flac","csrf_token":""})"));

    row["level"] = "lossless"; row["br"] = 1619359; row["type"] = "flac";
    upstream->respond(200, response.dump());
    const auto flac = unwrap(resolver->resolve_audio(user_context("MUSIC_U=member; __csrf=tok"),
        test::track("netease", "5257138"), lossless));
    CHECK(flac.actual_quality == domain::Quality::lossless);
    CHECK(flac.container == "flac");
    CHECK(flac.request_headers == std::vector<std::pair<std::string, std::string>>{{"User-Agent", std::string{protocol::user_agent}}});
    CHECK(upstream->calls.back().request.body == weapi_body_of(
        R"({"ids":"[5257138]","level":"lossless","encodeType":"flac","csrf_token":"tok"})"));
    upstream->respond(200, response.dump());
    auto hires = domain::ResourceRequirement{domain::Quality::hires, domain::QualityPolicy::allow_lower, false};
    auto lowered = unwrap(resolver->resolve_audio(user_context("MUSIC_U=member"), test::track("netease", "5257138"), hires));
    CHECK(lowered.actual_quality == domain::Quality::lossless);
    CHECK(unwrap(domain::evaluate_resource(hires, facts(lowered))).warnings ==
          std::vector<domain::ResourceWarning>{domain::ResourceWarning::quality_lowered});

    // Ordinary/anonymous account: upstream can return a lower quality even though the request succeeded.
    upstream->respond(200, fixture("url-5257138-standard.json"));
    const auto low = unwrap(resolver->resolve_audio(user_context("MUSIC_U=ordinary"), test::track("netease", "5257138"), lossless));
    CHECK(domain::evaluate_resource(lossless, facts(low)).error().code == ErrorCode::quality_unavailable);
    auto allow = lossless; allow.policy = domain::QualityPolicy::allow_lower;
    CHECK(unwrap(domain::evaluate_resource(allow, facts(low))).warnings ==
          std::vector<domain::ResourceWarning>{domain::ResourceWarning::quality_lowered});
    // Contradictory/unknown quality facts cannot satisfy a requested high tier by claiming unverified success.
    row["type"] = "mp3";
    upstream->respond(200, response.dump());
    CHECK(resolver->resolve_audio(anonymous_context(), test::track("netease", "5257138"), lossless).error().code ==
          ErrorCode::quality_unavailable);
    row["freeTrialInfo"] = {{"start", 0}, {"end", 30}};
    upstream->respond(200, response.dump());
    auto preview = unwrap(resolver->resolve_audio(user_context("MUSIC_U=ordinary"), test::track("netease", "5257138"), high));
    CHECK(domain::evaluate_resource(high, facts(preview)).error().code == ErrorCode::preview_rejected);
}

TEST_CASE("netease C entitlement and session failures stay distinct", "[providers][netease][stage-c]") {
    FixedKey key;
    auto upstream = std::make_shared<FakeUpstream>();
    auto resolver = unwrap(netease::make_resolver(setup(upstream)));
    for (const int fee : {1, 4}) {
        auto denied = nlohmann::json::parse(fixture("url-210049-vip.json"));
        denied["data"][0]["fee"] = fee;
        upstream->respond(200, denied.dump());
        auto result = resolver->resolve_audio(user_context("MUSIC_U=no-entitlement"), test::track("netease", "210049"), {});
        REQUIRE_FALSE(result);
        CHECK(result.error().code == ErrorCode::credentials_required);
        CHECK(detail_reason(result.error(), "netease_fee") == std::to_string(fee));
    }
    for (const auto code : {301, 403, 405, -460, -462}) {
        upstream->respond(200, nlohmann::json{{"code", code}}.dump());
        auto result = resolver->resolve_audio(anonymous_context(), test::track("netease", "5257138"), {});
        CHECK(result.error().code == (code == 301 || code == 403 ? ErrorCode::credentials_required :
                                     code == 405 ? ErrorCode::upstream_rate_limited : ErrorCode::upstream_failed));
    }
    auto verifier = unwrap(netease::make_verifier(setup(upstream)));
    const ports::SecretBytes cookie{std::string_view{"MUSIC_U=test"}};
    for (const int code : {301, 403}) {
        upstream->respond(200, nlohmann::json{{"code", code}}.dump());
        CHECK(unwrap(verifier->verify({user_identity(), cookie}, anonymous_context().operation)).status == ports::VerificationStatus::invalid);
    }
    upstream->respond(200, R"({"code":200,"account":{"id":123,"vipType":0},"profile":{"vipType":0}})");
    upstream->respond(503, "");
    auto ordinary = unwrap(verifier->verify({user_identity(), cookie}, anonymous_context().operation));
    CHECK(ordinary.status == ports::VerificationStatus::valid);
    CHECK(ordinary.membership.status == "unknown");
    CHECK(ordinary.status == ports::VerificationStatus::valid);
    CHECK(ordinary.scopes.empty());
}

TEST_CASE("netease C discovery protocol vectors are anonymous and bounded", "[providers][netease][stage-c]") {
    const protocol::Call call{std::string{fixed_key}, ""};
    CHECK(protocol::categories(call).body == weapi_body_of(R"({"csrf_token":""})"));
    CHECK(protocol::charts(call).url == "https://music.163.com/weapi/toplist?csrf_token=");
    CHECK(protocol::category_playlists("华语", 2, 2, call).body == weapi_body_of(
        R"({"cat":"华语","order":"hot","limit":2,"offset":2,"total":true,"csrf_token":""})"));
    CHECK(protocol::recommendations(call).body == weapi_body_of(
        R"({"limit":30,"total":true,"n":1000,"csrf_token":""})"));
    for (const auto& r : {protocol::categories(call), protocol::charts(call), protocol::recommendations(call),
                         protocol::category_playlists("华语", 0, 2, call)}) {
        CHECK(r.secret_headers.empty());
        CHECK(r.max_response_bytes <= 2 * 1024 * 1024);
    }
}

TEST_CASE("netease C directory schemas preserve totals and reject malformed lists", "[providers][netease][stage-c]") {
    auto categories = unwrap(parsing::parse_categories(fixture("discovery-categories.json")));
    REQUIRE(categories.size() == 3);
    CHECK(categories.front().id == "全部");
    CHECK(categories.front().name == "全部歌单");
    auto charts = unwrap(parsing::parse_charts(fixture("discovery-charts.json")));
    REQUIRE(charts.size() == 1);
    CHECK(charts[0].ref == test::track("netease", "3778678"));
    CHECK(charts[0].track_count > 10);
    CHECK(charts[0].updated_at);
    auto recommendations = unwrap(parsing::parse_discovery_playlists(fixture("discovery-recommendations.json"), true));
    REQUIRE(recommendations.items.size() == 2);
    CHECK_FALSE(recommendations.total);
    CHECK(recommendations.items.front().play_count);
    CHECK_FALSE(recommendations.items.front().creator); // copywriter is promotional copy, not the creator.
    CHECK(unwrap(parsing::parse_categories(R"({"code":200,"categories":{},"sub":[]})")).empty());
    CHECK(unwrap(parsing::parse_charts(R"({"code":200,"list":[]})")).empty());
    CHECK(unwrap(parsing::parse_discovery_playlists(R"({"code":200,"result":[]})", true)).items.empty());
    CHECK(unwrap(parsing::parse_discovery_playlists(R"({"code":200,"playlists":[],"total":0,"more":false})", false)).items.empty());
    CHECK_FALSE(parsing::parse_charts(R"({"code":200,"list":[{"id":1}]})"));
    CHECK_FALSE(parsing::parse_discovery_playlists(R"({"code":200,"playlists":[],"total":3,"more":true})", false));
    auto excessive = nlohmann::json::parse(fixture("discovery-charts.json"));
    excessive["list"] = std::vector<nlohmann::json>(257, excessive["list"][0]);
    CHECK_FALSE(parsing::parse_charts(excessive.dump()));
    for (const int code : {404, 405}) {
        const auto body = nlohmann::json{{"code", code}}.dump();
        const auto expected = code == 404 ? ErrorCode::not_found : ErrorCode::upstream_rate_limited;
        CHECK(parsing::parse_categories(body).error().code == expected);
        CHECK(parsing::parse_charts(body).error().code == expected);
        CHECK(parsing::parse_discovery_playlists(body, true).error().code == expected);
        CHECK(parsing::parse_discovery_playlists(body, false).error().code == expected);
    }
}

TEST_CASE("netease C category pages validate directory membership and advance", "[providers][netease][stage-c]") {
    FixedKey key;
    auto upstream = std::make_shared<FakeUpstream>();
    auto categories = unwrap(netease::make_categories(setup(upstream)));
    upstream->respond(200, fixture("discovery-categories.json"));
    CHECK(unwrap(categories->categories(user_context("MUSIC_U=never-send"))).size() == 3);
    auto lists = unwrap(netease::make_category_playlists(setup(upstream)));
    upstream->respond(200, fixture("discovery-categories.json"));
    upstream->respond(200, fixture("discovery-category-page.json"));
    const auto page = unwrap(lists->list(user_context("MUSIC_U=never-send"), test::track("netease", "华语"), {2, {}}));
    CHECK(page.page.page.total == 1950);
    CHECK(page.page.page.returned == 2);
    CHECK(page.next_cursor == "2");
    upstream->respond(200, fixture("discovery-categories.json"));
    upstream->respond(200, R"({"code":200,"playlists":[],"total":1950,"more":false})");
    CHECK_FALSE(unwrap(lists->list(anonymous_context(), test::track("netease", "华语"), {2, page.next_cursor})).next_cursor);
    upstream->respond(200, fixture("discovery-categories.json"));
    CHECK(lists->list(anonymous_context(), test::track("netease", "unknown"), {2, {}}).error().code == ErrorCode::not_found);
    const auto count = upstream->calls.size();
    CHECK(lists->list(anonymous_context(), test::track("kuwo", "华语"), {2, {}}).error().code == ErrorCode::validation_failed);
    CHECK(lists->list(anonymous_context(), test::track("netease", "华语"), {2, "10001"}).error().code == ErrorCode::cursor_mismatch);
    CHECK(upstream->calls.size() == count);
    for (const auto& call : upstream->calls) CHECK(call.request.secret_headers.empty());
}

TEST_CASE("netease C public recommendation snapshots are stable bounded and identity isolated", "[providers][netease][stage-c]") {
    FixedKey key;
    auto upstream = std::make_shared<FakeUpstream>();
    auto recommendations = unwrap(netease::make_recommendations(setup(upstream)));
    upstream->respond(200, fixture("discovery-recommendations.json"));
    const auto first = unwrap(recommendations->recommend_playlists(user_context("MUSIC_U=never-send"), {1, {}}));
    REQUIRE(first.next_cursor);
    CHECK_FALSE(first.page.page.total);
    CHECK(first.page.page.listed_total == 2);
    CHECK(recommendations->recommend_playlists(anonymous_context(), {1, first.next_cursor}).error().code == ErrorCode::cursor_expired);
    const auto second = unwrap(recommendations->recommend_playlists(user_context("MUSIC_U=never-send"), {1, first.next_cursor}));
    CHECK_FALSE(second.next_cursor);
    CHECK(first.page.items.front().ref != second.page.items.front().ref);
    auto changed = nlohmann::json::parse(fixture("discovery-recommendations.json"));
    changed["result"][0]["id"] = 111;
    upstream->respond(200, changed.dump());
    const auto fresh = unwrap(recommendations->recommend_playlists(user_context("MUSIC_U=never-send"), {1, {}}));
    CHECK(fresh.page.items.front().ref.id.value() == "111");
    CHECK(fresh.next_cursor != first.next_cursor);
    CHECK(unwrap(recommendations->recommend_playlists(user_context("MUSIC_U=never-send"), {1, first.next_cursor})).page.items.front().ref == second.page.items.front().ref);
    CHECK(upstream->calls.size() == 2);
    for (int i = 0; i < 63; ++i) {
        upstream->respond(200, fixture("discovery-recommendations.json"));
        unwrap(recommendations->recommend_playlists(anonymous_context(), {1, {}}));
    }
    CHECK(recommendations->recommend_playlists(user_context("MUSIC_U=never-send"), {1, first.next_cursor}).error().code == ErrorCode::cursor_expired);
    for (const auto& call : upstream->calls) CHECK(call.request.secret_headers.empty());
}

TEST_CASE("netease C charts use a validated playlist with bounded ordered track pages", "[providers][netease][stage-c]") {
    FixedKey key;
    auto upstream = std::make_shared<FakeUpstream>();
    auto directory = unwrap(netease::make_charts(setup(upstream)));
    auto detail = unwrap(netease::make_chart_detail(setup(upstream)));
    auto tracks = unwrap(netease::make_chart_tracks(setup(upstream)));
    upstream->respond(200, fixture("discovery-charts.json"));
    CHECK(unwrap(directory->charts(anonymous_context())).size() == 1);
    upstream->respond(200, fixture("discovery-charts.json"));
    CHECK(unwrap(detail->detail(anonymous_context(), test::track("netease", "3778678"))).track_count > 2);
    for (const bool track_port : {false, true}) {
        upstream->respond(200, fixture("discovery-charts.json"));
        if (track_port) CHECK(tracks->list(anonymous_context(), test::track("netease", "111"), {2, {}}).error().code == ErrorCode::not_found);
        else CHECK(detail->detail(anonymous_context(), test::track("netease", "111")).error().code == ErrorCode::not_found);
    }
    CHECK(tracks->list(anonymous_context(), test::track("kuwo", "3778678"), {2, {}}).error().code == ErrorCode::validation_failed);
    CHECK(detail->detail(anonymous_context(), test::track("kuwo", "3778678")).error().code == ErrorCode::validation_failed);
    auto playlist = nlohmann::json::parse(fixture("collection-playlist.json"));
    playlist["playlist"]["id"] = 3778678;
    playlist["playlist"]["trackIds"] = {{{"id", 5257138}}, {{"id", 5276808}}};
    playlist["playlist"]["trackCount"] = 200;
    upstream->respond(200, fixture("discovery-charts.json"));
    upstream->respond(200, playlist.dump());
    upstream->respond(200, fixture("detail-5257138.json"));
    auto first = unwrap(tracks->list(anonymous_context(), test::track("netease", "3778678"), {1, {}}));
    CHECK(first.page.items.front().ref.id.value() == "5257138");
    CHECK(first.page.page.total == 200);
    CHECK(first.page.page.listed_total == 2);
    REQUIRE(first.next_cursor);
    upstream->respond(200, fixture("discovery-charts.json"));
    upstream->respond(200, playlist.dump());
    auto second_song = nlohmann::json::parse(fixture("detail-5257138.json"));
    second_song["songs"][0]["id"] = 5276808;
    upstream->respond(200, second_song.dump());
    auto second = unwrap(tracks->list(anonymous_context(), test::track("netease", "3778678"), {1, first.next_cursor}));
    CHECK(second.page.items.front().ref.id.value() == "5276808");
    CHECK_FALSE(second.next_cursor);
    for (std::size_t i = upstream->calls.size() - 6; i < upstream->calls.size(); ++i) {
        CHECK(upstream->calls[i].request.operation == "chart_tracks");
        CHECK(upstream->calls[i].request.secret_headers.empty());
    }
}

TEST_CASE("netease D personal playlists require the bound account and retain relationships", "[providers][netease][stage-d]") {
    FixedKey key;
    auto upstream = std::make_shared<FakeUpstream>();
    auto port = unwrap(netease::make_user_playlists(setup(upstream)));
    CHECK(port->user_playlists(anonymous_context(), {2, {}}).error().code == ErrorCode::credentials_required);
    CHECK(upstream->calls.empty());
    const auto account = R"({"code":200,"account":{"id":99},"profile":{"userId":123}})";
    const auto lists = R"({"code":200,"playlist":[{"id":1,"name":"mine","creator":{"userId":123}},
        {"id":2,"name":"saved","creator":{"userId":456},"subscribed":true}],"more":true})";
    upstream->respond(200, account);
    upstream->respond(200, lists);
    auto first = unwrap(port->user_playlists(user_context("MUSIC_U=test; __csrf=abc"), {2, {}}));
    CHECK(first.page.items[0].relationship == "created");
    CHECK(first.page.items[1].relationship == "subscribed");
    CHECK(first.next_cursor == "2");
    CHECK_FALSE(first.page.page.total);
    CHECK(upstream->calls[1].request.body == weapi_body_of(R"({"uid":123,"offset":0,"limit":2,"includeVideo":false,"csrf_token":"abc"})"));
    for (const auto& call : upstream->calls) {
        CHECK(call.request.operation == "user_playlists");
        CHECK(header(call.request.secret_headers, "Cookie"));
        CHECK_FALSE(header(call.request.headers, "Cookie"));
    }
    upstream->respond(200, account);
    upstream->respond(200, R"({"code":200,"playlist":[],"more":false})");
    CHECK_FALSE(unwrap(port->user_playlists(user_context("MUSIC_U=test"), {2, first.next_cursor})).next_cursor);
    upstream->respond(200, R"({"code":200,"account":null})");
    CHECK(port->user_playlists(user_context("MUSIC_U=expired"), {2, {}}).error().code == ErrorCode::credentials_required);
}

TEST_CASE("netease D private playlists send bound cookies to both catalog requests", "[providers][netease][stage-d]") {
    FixedKey key;
    auto upstream = std::make_shared<FakeUpstream>();
    auto port = unwrap(netease::make_playlist_tracks(setup(upstream)));
    auto p = nlohmann::json::parse(fixture("collection-playlist.json"));
    p["playlist"]["trackIds"] = {{{"id", 5257138}}};
    upstream->respond(200, p.dump());
    upstream->respond(200, fixture("detail-5257138.json"));
    const auto id = std::to_string(p["playlist"]["id"].get<std::int64_t>());
    CHECK(unwrap(port->list(user_context("MUSIC_U=private"), test::track("netease", id), {1, {}})).page.items.size() == 1);
    REQUIRE(upstream->calls.size() == 2);
    for (const auto& call : upstream->calls) CHECK(header(call.request.secret_headers, "Cookie"));
}

TEST_CASE("netease D artist hot list uses stable IDs and rejects changed pagination", "[providers][netease][stage-d]") {
    FixedKey key;
    auto upstream = std::make_shared<FakeUpstream>();
    auto port = unwrap(netease::make_artist_tracks(setup(upstream)));
    nlohmann::json data{{"code",200},{"artist",{{"id",1},{"name","same name"}}},
        {"hotSongs", {{{"id",10},{"name","one"}},{{"id",11},{"name","two"}}}}};
    upstream->respond(200, data.dump());
    auto first = unwrap(port->list(anonymous_context(), test::track("netease","1"), {1,{}}));
    REQUIRE(first.next_cursor);
    CHECK_FALSE(first.page.page.total);
    CHECK(first.page.page.listed_total == 2);
    upstream->respond(200, data.dump());
    CHECK(unwrap(port->list(anonymous_context(), test::track("netease","1"), {1,first.next_cursor})).page.items.front().ref.id.value() == "11");
    CHECK_FALSE(parsing::parse_artist_tracks(data.dump(), "2")); // same display name is irrelevant
    data["hotSongs"][1]["id"] = 12;
    upstream->respond(200, data.dump());
    CHECK(port->list(anonymous_context(), test::track("netease","1"), {1,first.next_cursor}).error().code == ErrorCode::cursor_expired);
    data["hotSongs"] = nlohmann::json::array();
    CHECK(unwrap(parsing::parse_artist_tracks(data.dump(),"1")).tracks.empty());
    CHECK(unwrap(parsing::parse_link("https://music.163.com/#/artist?id=1")).kind == "artist");
}

TEST_CASE("netease D YRC preserves independent variants and bounds malformed timing", "[providers][netease][stage-d]") {
    nlohmann::json data{{"code",200},{"lrc",{{"lyric","[00:01.00]原文"}}},
        {"tlyric",{{"lyric","[00:05.00]translation"}}},{"romalrc",{{"lyric","[00:01.25]romaji"}}},
        {"yrc",{{"lyric","[1000,1000](1000,400,0)原(1400,600,0)文\n[9223372036854775807,1](0,1,0)bad\n[-1,1](0,1,0)bad"}}}};
    auto lyrics = unwrap(parsing::parse_lyrics(data.dump()));
    REQUIRE(lyrics.karaoke.size() == 1);
    CHECK(lyrics.karaoke[0].words[1].start_ms == 1400);
    CHECK(lyrics.karaoke[0].words[1].duration_ms == 600);
    CHECK(lyrics.karaoke[0].words[1].text == "文");
    CHECK(lyrics.lrc == "[00:01.00]原文");
    CHECK(lyrics.translated_lrc == "[00:05.00]translation");
    CHECK(lyrics.romanized_lrc == "[00:01.25]romaji");
    CHECK(lyrics.warnings == std::vector<std::string>{"YRC_MALFORMED"});
    data.erase("lrc");
    CHECK_FALSE(unwrap(parsing::parse_lyrics(data.dump())).unavailable);
    data["yrc"]["lyric"] = "[1000,10](1000,20,0)outside";
    CHECK(unwrap(parsing::parse_lyrics(data.dump())).karaoke.empty());
    data["yrc"]["lyric"] = 123;
    CHECK_FALSE(unwrap(parsing::parse_lyrics(data.dump())).warnings.empty());
    data["yrc"]["lyric"] = " \t\n[1000,20](1000,20,0)one\n[1000,20](1000,20,0)two";
    auto repeated = unwrap(parsing::parse_lyrics(data.dump()));
    CHECK(repeated.karaoke.size() == 2);
    CHECK(repeated.warnings.empty());
}

namespace {
nlohmann::json program_fixture(int id = 100) {
    return {{"id",id},{"name","episode"},{"mainSong",{{"id",200}}},{"duration",150000},
        {"radio",{{"id",300},{"name","podcast"},{"programCount",230}}},
        {"dj",{{"nickname","presenter"}}},{"programFeeType",0},{"buyed",false}};
}
}

TEST_CASE("netease D episodes page beyond 100 without confusing radio program and audio IDs", "[providers][netease][stage-d]") {
    FixedKey key;
    auto upstream = std::make_shared<FakeUpstream>();
    auto port = unwrap(netease::make_podcast_episodes(setup(upstream)));
    std::optional<std::string> cursor;
    std::vector<std::string> ids;
    for (int at = 0; at < 230; at += 100) {
        auto items = nlohmann::json::array();
        for (int i = at; i < std::min(at+100,230); ++i) items.push_back(program_fixture(100+i));
        upstream->respond(200, nlohmann::json{{"code",200},{"programs",items},{"count",230},{"more",at+100<230}}.dump());
        auto page = unwrap(port->list(anonymous_context(), test::track("netease","300"), {100,cursor}));
        CHECK(page.page.page.total == 230);
        for (const auto& e : page.page.items) ids.push_back(e.ref.id.value());
        cursor = page.next_cursor;
    }
    CHECK(ids.size() == 230);
    CHECK(ids.front() == "100"); CHECK(ids.back() == "329"); CHECK_FALSE(cursor);
    CHECK(upstream->calls.back().request.body == weapi_body_of(R"({"radioId":300,"offset":200,"limit":100,"asc":true,"csrf_token":""})"));
    auto e = unwrap(parsing::parse_episode(nlohmann::json{{"code",200},{"program",program_fixture()}}.dump(), "100"));
    CHECK(e.presenter == "presenter"); CHECK(e.audio_ref->id.value() == "program:100:200");
    CHECK(unwrap(parsing::parse_link("https://music.163.com/#/djradio?id=300")).kind == "podcast");
    CHECK(unwrap(parsing::parse_link("https://music.163.com/program?id=100")).kind == "episode");
    upstream->respond(200, nlohmann::json{{"code",200},{"programs",{program_fixture()}},{"count",10001},{"more",true}}.dump());
    CHECK(port->list(anonymous_context(), test::track("netease","300"), {1,"9999"}).error().code == ErrorCode::upstream_failed);
    CHECK(parsing::parse_episodes(R"({"code":200,"programs":[]})", "300").error().code == ErrorCode::upstream_failed);
}

TEST_CASE("netease D program resolver validates mapping purchase preview and metadata", "[providers][netease][stage-d]") {
    FixedKey key;
    auto upstream = std::make_shared<FakeUpstream>();
    auto resolver = unwrap(netease::make_resolver(setup(upstream)));
    auto details = unwrap(netease::make_detail(setup(upstream)));
    const auto ref = test::track("netease","program:100:200");
    const auto program = nlohmann::json{{"code",200},{"program",program_fixture()}};
    upstream->respond(200, program.dump());
    auto detail = unwrap(details->track_detail(anonymous_context(),ref));
    CHECK(detail.artists.empty()); CHECK(detail.album->title == "podcast"); CHECK(detail.ref == ref);
    auto paid = program; paid["program"]["programFeeType"] = 1;
    upstream->respond(200, paid.dump());
    CHECK(resolver->resolve_audio(anonymous_context(),ref,{}).error().code == ErrorCode::credentials_required);
    auto replaced = program; replaced["program"]["mainSong"]["id"] = 201;
    upstream->respond(200, replaced.dump());
    CHECK(resolver->resolve_audio(anonymous_context(),ref,{}).error().code == ErrorCode::not_found);
    for (bool preview : {false,true}) {
        upstream->respond(200,program.dump());
        auto entry = nlohmann::json{{"id",200},{"code",200},
            {"url","https://m1.music.126.net/test.mp3"},{"type","mp3"},{"br",128000},{"level",nullptr},
            {"time",0},{"freeTrialInfo",preview ? nlohmann::json{{"start",0},{"end",30}} : nlohmann::json{}}};
        auto audio = nlohmann::json{{"code",200},{"data",nlohmann::json::array({entry})}};
        upstream->respond(200,audio.dump());
        auto result = unwrap(resolver->resolve_audio(user_context("MUSIC_U=bound"),ref,{}));
        CHECK(result.actual_track == ref);
        CHECK(result.completeness == (preview ? domain::Completeness::preview : domain::Completeness::full));
        CHECK(std::none_of(result.request_headers.begin(), result.request_headers.end(), [](const auto& h) { return h.first == "Cookie"; }));
        CHECK(upstream->calls.back().request.body == weapi_body_of(R"({"ids":"[200]","level":"standard","encodeType":"flac","csrf_token":""})"));
    }
}

TEST_CASE("netease artist search and albums validate identity pagination and protocol", "[netease][stage-e]") {
    FixedKey key;
    auto upstream = std::make_shared<FakeUpstream>();
    auto search = unwrap(netease::make_artist_search(setup(upstream)));
    auto albums = unwrap(netease::make_artist_albums(setup(upstream)));
    const auto first = R"({"code":200,"result":{"artistCount":2,"artists":[{"id":1,"name":"歌手甲"}]}})";
    upstream->respond(200, first);
    auto found = unwrap(search->search(anonymous_context(), {"歌手",1,{}}));
    REQUIRE(found.page.items.size() == 1);
    CHECK(found.page.items[0].name == "歌手甲");
    CHECK(found.next_cursor == "1");
    CHECK(upstream->calls.back().request.body == weapi_body_of(
        R"({"s":"歌手","type":100,"offset":0,"limit":1,"total":true,"csrf_token":""})"));
    upstream->respond(200, R"({"code":200,"result":{"artistCount":2,"artists":[{"id":2,"name":"歌手乙"}]}})");
    CHECK_FALSE(unwrap(search->search(anonymous_context(), {"歌手",1,found.next_cursor})).next_cursor);
    const auto page = R"({"code":200,"artist":{"id":1},"hotAlbums":[{"id":10,"name":"专辑"}],"more":true})";
    upstream->respond(200, page);
    auto list = unwrap(albums->list(anonymous_context(), test::track("netease","1"), {1,{}}));
    CHECK(list.next_cursor == "1");
    CHECK_FALSE(list.page.page.total); // artist.albumSize 可能包含未在本目录公开的项目。
    CHECK(list.page.items[0].ref.id.value() == "10");
    CHECK(upstream->calls.back().request.url.find("artist/albums/1") != std::string::npos);
    for (const auto* bad : {R"({"code":200,"artist":{"id":2},"hotAlbums":[],"more":false})",
        R"({"code":200,"artist":{"id":1},"hotAlbums":[]})",
        R"({"code":200,"artist":{"id":1},"hotAlbums":[],"more":true})"}) {
        CHECK_FALSE(parsing::parse_artist_albums(bad,"1"));
    }
    CHECK_FALSE(search->search(anonymous_context(), {"x",51,{}}));
    CHECK_FALSE(albums->list(anonymous_context(), test::track("netease","1"), {1,"10001"}));
    upstream->respond(200,page);
    CHECK_FALSE(albums->list(anonymous_context(), test::track("netease","1"), {1,"9999"}));
}

TEST_CASE("netease hires requires explicit upstream level and preserves account downgrades", "[netease][stage-e]") {
    FixedKey key;
    auto upstream = std::make_shared<FakeUpstream>();
    auto resolver = unwrap(netease::make_resolver(setup(upstream)));
    auto data = nlohmann::json::parse(fixture("url-5257138-standard.json"));
    data["data"][0]["level"] = "hires";
    data["data"][0]["type"] = "flac";
    data["data"][0]["br"] = 2500000;
    upstream->respond(200, data.dump());
    auto result = unwrap(resolver->resolve_audio(user_context("MUSIC_U=synthetic"), test::track("netease","5257138"),
        {domain::Quality::hires,domain::QualityPolicy::strict,false}));
    CHECK(result.actual_quality == domain::Quality::hires);
    CHECK(upstream->calls.back().request.body == weapi_body_of(
        R"({"ids":"[5257138]","level":"hires","encodeType":"flac","csrf_token":""})"));
}

TEST_CASE("netease live protocol probe", "[providers][online-netease]") {
    if (infrastructure::config::process_environment("MUSIC_SERVICE_NETEASE_ONLINE") != "1") {
        SKIP("set MUSIC_SERVICE_NETEASE_ONLINE=1 to run");
    }
    auto clock = std::make_shared<infrastructure::SystemClock>();
    auto transport = unwrap(infrastructure::transport::HttplibTransport::create({}));
    auto ledger = std::make_shared<runtime::AdmissionLedger>(runtime::AdmissionLimits{}, clock);
    auto upstream = std::make_shared<application::transport::UpstreamClient>(ledger, transport, clock);
    const auto live = setup(upstream);

    auto search = unwrap(netease::make_search(live));
    const auto page = unwrap(search->search_tracks(anonymous_context(), {"周杰伦", 10, std::nullopt}));
    REQUIRE_FALSE(page.page.items.empty());
    WARN("search: " << page.page.items.size() << " of " << *page.page.page.total);
    const auto next = unwrap(search->search_tracks(anonymous_context(), {"周杰伦", 10, page.next_cursor}));
    CHECK(next.page.items.front().ref != page.page.items.front().ref);

    auto detail = unwrap(netease::make_detail(live));
    const auto roof = unwrap(detail->track_detail(anonymous_context(), test::track("netease", "5257138")));
    CHECK(roof.title == "屋顶");

    auto lyrics = unwrap(netease::make_lyrics(live));
    CHECK(unwrap(lyrics->lyrics(anonymous_context(), test::track("netease", "5257138"))).lrc);
    CHECK(unwrap(lyrics->lyrics(anonymous_context(), test::track("netease", "5276808"))).unavailable);

    auto resolver = unwrap(netease::make_resolver(live));
    const auto audio = unwrap(resolver->resolve_audio(anonymous_context(), test::track("netease", "5257138"), {}));
    CHECK(audio.actual_quality == domain::Quality::standard);
    CHECK(audio.completeness == domain::Completeness::full);
    WARN("audio: " << audio.url.substr(0, 40) << " " << *audio.bitrate_bps);
    CHECK(resolver->resolve_audio(anonymous_context(), test::track("netease", "210049"), {}).error().code ==
          ErrorCode::credentials_required);
    CHECK(resolver->resolve_audio(anonymous_context(), test::track("netease", "186016"), {}).error().code ==
          ErrorCode::not_found);

    // An invalid session is reported invalid, not as an error.
    auto verifier = unwrap(netease::make_verifier(live));
    const ports::SecretBytes stale{std::string_view{"MUSIC_U=0000invalid0000"}};
    CHECK(unwrap(verifier->verify({user_identity(), stale},
                                  {std::chrono::steady_clock::now() + std::chrono::seconds(10), {}}))
              .status == ports::VerificationStatus::invalid);
}
