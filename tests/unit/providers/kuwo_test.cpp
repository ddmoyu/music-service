#include "providers/kuwo/parsing/artists.hpp"
// Kuwo protocol package against desensitized responses recorded on 2026-10-20
// (tests/fixtures/providers/kuwo) and a fake upstream; no network. The opt-in
// [online-kuwo] case probes the live service.

#include "../domain/test_support.hpp"

#include "application/transport/upstream_client.hpp"
#include "infrastructure/clock.hpp"
#include "infrastructure/config/config_loader.hpp"
#include "infrastructure/transport/httplib_transport.hpp"
#include "providers/kuwo/bundle.hpp"
#include "providers/kuwo/capabilities.hpp"
#include "providers/kuwo/parsing/parsers.hpp"
#include "runtime/admission/admission_ledger.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <deque>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <tuple>

using namespace music_service;
using namespace music_service::test;
namespace ports = application::ports;

TEST_CASE("kuwo payment flags distinguish playback download and membership", "[kuwo][pay]") {
    using Json = nlohmann::json;
    namespace p = providers::kuwo::parsing;
    auto value = unwrap(p::parse_pay(Json{{"tpay", "0"}, {"fpay", "1"},
        {"payInfo", {{"feeType", {{"vip", "1"}}}}}}));
    CHECK(value.pay_play == 0); CHECK(value.pay_down == 1); CHECK(value.pay_month == 1);
    auto detail = unwrap(p::parse_track_info(Json{{"code", 200}, {"data", {{"rid", 1}, {"name", "test"}, {"artist", "artist"}, {"isListenFee", true}}}}.dump(), "1"));
    CHECK(detail.pay.pay_play == 1); CHECK_FALSE(detail.pay.pay_down); CHECK_FALSE(detail.pay.pay_month);
    value = unwrap(p::parse_pay(Json{{"tpay", 2}, {"fpay", nullptr}, {"payInfo", {{"feeType", {{"vip", 0}}}}}}));
    CHECK_FALSE(value.pay_play); CHECK_FALSE(value.pay_down); CHECK(value.pay_month == 0);
    for (const auto& invalid : {Json{{"tpay", -1}}, Json{{"fpay", "invalid"}}, Json{{"payInfo", Json::array()}}})
        CHECK_FALSE(p::parse_pay(invalid));
}

TEST_CASE("kuwo collection links distinguish playlists albums and tracks", "[kuwo][collections-b]") {
    namespace p = providers::kuwo::parsing;
    CHECK(unwrap(p::parse_link("https://www.kuwo.cn/playlist_detail/2234580349?from=share")).kind == "playlist");
    CHECK(unwrap(p::parse_link("http://kuwo.cn/album_detail/1293/")).id == "1293");
    CHECK(unwrap(p::parse_link("https://www.kuwo.cn/album_detail/1")).kind == "album");
    CHECK(unwrap(p::parse_link("https://www.kuwo.cn/play_detail/1")).kind == "track");
    for (const auto* link : {"https://www.kuwo.cn.evil/playlist_detail/1", "https://www.kuwo.cn/playlist_detail/../1",
        "https://www.kuwo.cn/album_detail/abc", "https://www.kuwo.cn/playlist_detail/1/more",
        "https://www.kuwo.cn/artist_detail/1", "https://www.kuwo.cn:9999/album_detail/1"}) CHECK_FALSE(p::parse_link(link));
}
namespace parsing = providers::kuwo::parsing;
using domain::ErrorCode;

namespace {

std::string fixture(const char* name) {
    std::ifstream file(std::filesystem::path{MUSIC_SERVICE_TEST_FIXTURES} / "providers" / "kuwo" / name,
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

// Answers queued responses in order and records what was asked.
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

ports::ProviderContext context() {
    return {{std::chrono::steady_clock::now() + std::chrono::seconds(10), {}},
            unwrap(domain::AuthIdentityKey::make(anonymous("prn_a", "kuwo"), {})),
            std::nullopt};
}

ports::ProviderSetup setup(std::shared_ptr<ports::UpstreamHttp> upstream) {
    return {source("kuwo"), "{}", std::move(upstream), "kuwo_web"};
}

} // namespace

TEST_CASE("kuwo cookie verifier checks identity before querying membership", "[providers][kuwo][membership]") {
    auto http = std::make_shared<FakeUpstream>();
    auto bundle = providers::kuwo::make_bundle();
    REQUIRE(bundle.auth_profiles.size() == 1);
    auto verifier = unwrap(bundle.auth_profiles[0].verifier(setup(http)));
    const auto identity = unwrap(domain::AuthIdentityKey::make(user("prn_a", "kuwo", "cre_1"), {1, {}, {}, {}}));
    ports::SecretBytes cookie{"userid=12345; websid=synthetic-session"};
    SECTION("valid with membership") {
        http->respond(200, R"({"meta":{"code":200},"data":{"UID":"12345"}})");
        http->respond(200, R"({"meta":{"code":200},"ctime":1800000000000,"data":{"svipExpire":"2000000000000","vipLuxuryExpire":"2000000005000","vipExpire":"0","vipmExpire":"0"}})");
        auto result = unwrap(verifier->verify({identity, cookie}, context().operation));
        CHECK(result.status == ports::VerificationStatus::valid);
        CHECK(result.membership.type == "svip");
        CHECK_FALSE(result.account_hint);
        REQUIRE(http->calls.size() == 2);
        for (const auto& call : http->calls) {
            CHECK(call.request.method == ports::HttpMethod::post);
            CHECK_FALSE(call.request.allow_redirects);
            CHECK(call.request.url.find("synthetic-session") == std::string::npos);
            CHECK(call.request.max_response_bytes == 256 * 1024);
        }
    }
    SECTION("missing login") {
        http->respond(200, R"({"meta":{"code":200},"data":null})");
        CHECK(unwrap(verifier->verify({identity, cookie}, context().operation)).status == ports::VerificationStatus::invalid);
        CHECK(http->calls.size() == 1);
    }
    SECTION("wrong account") {
        http->respond(200, R"({"meta":{"code":200},"data":{"UID":"67890"}})");
        CHECK(unwrap(verifier->verify({identity, cookie}, context().operation)).status == ports::VerificationStatus::invalid);
        CHECK(http->calls.size() == 1);
    }
    SECTION("membership unavailable") {
        http->respond(200, R"({"meta":{"code":200},"data":{"UID":"12345"}})");
        http->respond(503, "private response");
        auto result = unwrap(verifier->verify({identity, cookie}, context().operation));
        CHECK(result.status == ports::VerificationStatus::valid);
        CHECK(result.membership.status == "unknown");
    }
}

TEST_CASE("kuwo search responses map to tracks", "[providers][kuwo]") {
    const auto page = unwrap(parsing::parse_search(fixture("search-qingtian-pn0-rn3.json")));
    CHECK(page.total == 3552);
    CHECK(page.page == 0);
    REQUIRE(page.tracks.size() == 3);

    const auto& first = page.tracks[0];
    CHECK(first.ref.source.value() == "kuwo");
    CHECK(first.ref.id.value() == "228908"); // the MUSIC_ prefix is dropped
    CHECK(first.title == "晴天");
    REQUIRE(first.artists.size() == 1);
    CHECK(first.artists[0].name == "周杰伦");
    CHECK(first.artists[0].ref->id.value() == "336");
    REQUIRE(first.album);
    CHECK(first.album->title == "叶惠美");
    CHECK(first.album->ref->id.value() == "1293");
    CHECK(first.duration_ms == 269000);
    CHECK(first.version.kind == domain::VersionKind::unknown); // not guessed from "(Live)"

    // An entry without an album keeps it empty rather than inventing one.
    CHECK(page.tracks[2].ref.id.value() == "78932517");
    CHECK_FALSE(page.tracks[2].album);

    for (const auto* broken : {"<html>blocked</html>", "{}", R"({"abslist":[{"SONGNAME":"x"}],"TOTAL":"1","PN":"0"})",
                               R"({"abslist":{},"TOTAL":"1","PN":"0"})"}) {
        INFO(broken);
        const auto result = parsing::parse_search(broken);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ErrorCode::upstream_failed);
        CHECK_FALSE(detail_reason(result.error(), "protocol").empty());
    }
}

TEST_CASE("kuwo text decoding", "[providers][kuwo]") {
    CHECK(parsing::decode_text("  Tom&amp;Jerry&nbsp;") == "Tom&Jerry");
    CHECK(parsing::decode_text("&#26228;&#x5929;") == "晴天");
    CHECK(parsing::decode_text("a &unknown; b & c") == "a &unknown; b & c");
    CHECK(parsing::decode_text("&#xD800;") == "&#xD800;");
}

TEST_CASE("kuwo compressed GB18030 lyrics are bounded and reach the lyrics capability", "[kuwo][stage-e]") {
    const auto blob = fixture("lyric-synthetic.bin");
    const auto parsed = unwrap(parsing::parse_lyrics(blob));
    CHECK(parsed.lrc == "[ti:测试]\n[00:01.000]中文歌词\n[00:02.500]另一行\n");
    CHECK_FALSE(parsed.unavailable);
    CHECK(parsed.karaoke.empty()); // 非标准字标记不伪装成已验证的字级时间。
    CHECK(unwrap(parsing::parse_lyrics("tp=none\r\ncand_lrc_count=0\r\n\r\n")).unavailable);
    for (const auto& bad : {std::string{"html"}, blob.substr(0, blob.size()-1), blob + "trailing",
        fixture("lyric-invalid-encoding.bin"), fixture("lyric-oversized.bin"), std::string(512*1024+1, 'x')}) {
        const auto result = parsing::parse_lyrics(bad);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == ErrorCode::upstream_failed);
    }
    auto upstream = std::make_shared<FakeUpstream>();
    upstream->respond(200, blob);
    auto port = unwrap(providers::kuwo::make_lyrics(setup(upstream)));
    CHECK(unwrap(port->lyrics(context(), test::track("kuwo", "123"))).lrc == parsed.lrc);
    REQUIRE(upstream->calls.size() == 1);
    CHECK(upstream->calls[0].request.url.starts_with("https://newlyric.kuwo.cn/newlyric.lrc?"));
    CHECK(upstream->calls[0].request.secret_headers.empty());
    CHECK(upstream->calls[0].request.max_response_bytes == 512*1024);
    CHECK_FALSE(port->lyrics(context(), test::track("netease", "123")));
    CHECK(upstream->calls.size() == 1);
}

TEST_CASE("kuwo high and lossless follow response evidence and refuse substituted identity", "[kuwo][stage-e]") {
    auto upstream = std::make_shared<FakeUpstream>();
    auto resolver = unwrap(providers::kuwo::make_resolver(setup(upstream)));
    const domain::ResourceRequirement wanted{domain::Quality::lossless, domain::QualityPolicy::strict, false};
    for (const auto& [format, rate, quality] : std::vector<std::tuple<std::string,int,domain::Quality>>{
        {"mp3",128,domain::Quality::standard}, {"mp3",320,domain::Quality::high}, {"flac",2000,domain::Quality::lossless}}) {
        upstream->respond(200, "{\"code\":200,\"data\":{\"rid\":123,\"type\":0,\"format\":\"" + format +
            "\",\"bitrate\":" + std::to_string(rate) + ",\"duration\":200,\"url\":\"https://cdn.kuwo.cn/synthetic\"}}");
        const auto audio = unwrap(resolver->resolve_audio(context(), test::track("kuwo","123"), wanted));
        CHECK(audio.actual_quality == quality);
        CHECK(upstream->calls.back().request.url.find("br=2000kflac") != std::string::npos);
        CHECK(domain::evaluate_resource(wanted,{audio.actual_quality,audio.completeness}).has_value() == (quality == domain::Quality::lossless));
        auto lower = wanted; lower.policy = domain::QualityPolicy::allow_lower;
        CHECK(domain::evaluate_resource(lower,{audio.actual_quality,audio.completeness}));
    }
    upstream->respond(200, fixture("audio-228908-substitute.json"));
    auto preview = unwrap(resolver->resolve_audio(context(), test::track("kuwo","228908"), wanted));
    CHECK_FALSE(preview.actual_quality);
    CHECK(preview.completeness == domain::Completeness::preview);
}

TEST_CASE("kuwo song JSON gives details for exactly that song", "[providers][kuwo]") {
    const auto page = fixture("track-info-78932517.json");
    const auto track = unwrap(parsing::parse_track_info(page, "78932517"));
    CHECK(track.ref.id.value() == "78932517");
    CHECK(track.title == "晴天");
    REQUIRE(track.artists.size() == 1);
    CHECK(track.artists[0].name == "蓝心羽");
    CHECK_FALSE(track.album); // the API has no album identity
    CHECK(track.duration_ms == 176000);

    CHECK(parsing::parse_track_info(R"({"code":-1,"data":null})", "999999999999").error().code ==
          ErrorCode::not_found);
    // A JSON response for another song is not accepted as this one.
    const auto other = parsing::parse_track_info(page, "1");
    CHECK(other.error().code == ErrorCode::upstream_failed);
}

TEST_CASE("kuwo audio answers become evidence, substitutes included", "[providers][kuwo]") {
    const auto standard = unwrap(parsing::parse_audio(fixture("audio-78932517-standard.json"), "78932517"));
    CHECK(standard.url.rfind("https://kw-lv.kuwo.cn/", 0) == 0); // upgraded from http
    CHECK(standard.format == "mp3");
    CHECK(standard.bitrate_kbps == 128);
    CHECK(standard.duration_seconds == 176);
    CHECK_FALSE(standard.substitute);
    CHECK(standard.regular);

    // Paid song: an 11 s clip of another rid.
    const auto clip = unwrap(parsing::parse_audio(fixture("audio-228908-substitute.json"), "228908"));
    CHECK(clip.substitute);
    CHECK_FALSE(clip.regular);
    CHECK(clip.duration_seconds == 11);

    const auto missing = parsing::parse_audio(fixture("audio-missing.json"), "999999999999");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == ErrorCode::upstream_failed);
    CHECK(detail_reason(missing.error(), "kuwo_code") == "4013");

    const auto foreign = parsing::parse_audio(
        R"({"code":200,"data":{"url":"http://evil.example/a.mp3","rid":1,"type":0,"bitrate":128,"format":"mp3"}})", "1");
    CHECK(detail_reason(foreign.error(), "protocol") == "audio address is not on a kuwo.cn host");
    const auto lookalike = parsing::parse_audio(
        R"({"code":200,"data":{"url":"http://evilkuwo.cn/a.mp3","rid":1,"type":0}})", "1");
    CHECK_FALSE(lookalike.has_value());
    CHECK(parsing::parse_audio("<html/>", "1").error().code == ErrorCode::upstream_failed);
}

TEST_CASE("kuwo song links", "[providers][kuwo]") {
    CHECK(unwrap(parsing::parse_track_link("http://www.kuwo.cn/play_detail/78932517")) == "78932517");
    CHECK(unwrap(parsing::parse_track_link("https://kuwo.cn/play_detail/228908/")) == "228908");
    CHECK(unwrap(parsing::parse_track_link("https://m.kuwo.cn/newh5app/play_detail/228908?from=share")) == "228908");
    for (const auto* bad : {"https://www.kuwo.cn/album_detail/1293", "https://www.kuwo.cn/play_detail/abc",
                            "https://www.kuwo.cn/play_detail/0123", "https://evil.example/play_detail/1",
                            "https://www.kuwo.cn.evil.example/play_detail/1", "kuwo.cn/play_detail/1"}) {
        INFO(bad);
        CHECK(parsing::parse_track_link(bad).error().code == ErrorCode::validation_failed);
    }
}

TEST_CASE("kuwo search requests go through the upstream client", "[providers][kuwo]") {
    auto upstream = std::make_shared<FakeUpstream>();
    auto search = unwrap(providers::kuwo::make_search(setup(upstream)));
    upstream->respond(200, fixture("search-qingtian-pn0-rn3.json"));

    const auto page = unwrap(search->search_tracks(context(), {"晴天", 3, std::nullopt}));
    REQUIRE(upstream->calls.size() == 1);
    const auto& call = upstream->calls[0];
    CHECK(call.request.url.rfind("https://www.kuwo.cn/search/searchMusicBykeyWord?", 0) == 0);
    CHECK(call.request.url.find("&pn=0&rn=3&all=%E6%99%B4%E5%A4%A9") != std::string::npos);
    CHECK(call.request.secret_headers.empty()); // anonymous
    CHECK(call.request.operation == "search_tracks");
    CHECK(call.base.principal_id == "prn_a");
    CHECK(call.base.source.value() == "kuwo");
    CHECK(call.base.traffic_class == ports::TrafficClass::short_request);
    CHECK(call.base.rate_limit_group == "kuwo_web");
    CHECK(call.base.identity.has_value());
    CHECK(page.page.items.size() == 3);
    CHECK(page.page.page.total == 3552);
    CHECK(page.page.page.has_more == true);
    CHECK(page.next_cursor == "1");

    auto second_page = fixture("search-qingtian-pn0-rn3.json");
    const auto pn = second_page.find(R"("PN": "0")");
    REQUIRE(pn != std::string::npos);
    second_page.replace(pn, 9, R"("PN": "1")");
    upstream->respond(200, second_page);
    CHECK(unwrap(search->search_tracks(context(), {"晴天", 3, std::string{"1"}})).next_cursor == "2");
    CHECK(upstream->calls[1].request.url.find("&pn=1&rn=3&") != std::string::npos);

    // An answer for another page is not taken as the requested one.
    upstream->respond(200, fixture("search-qingtian-pn0-rn3.json"));
    const auto wrong_page = search->search_tracks(context(), {"晴天", 3, std::string{"1"}});
    CHECK(wrong_page.error().code == ErrorCode::upstream_failed);
    CHECK(detail_reason(wrong_page.error(), "protocol") == "search response is for another page");

    CHECK(search->search_tracks(context(), {"晴天", 3, std::string{"x"}}).error().code == ErrorCode::cursor_mismatch);
    CHECK(search->search_tracks(context(), {"  ", 3, std::nullopt}).error().code == ErrorCode::validation_failed);
    CHECK(search->search_tracks(context(), {"a", 51, std::nullopt}).error().code == ErrorCode::validation_failed);
    CHECK(upstream->calls.size() == 3); // invalid input never reaches the network

    upstream->respond(429, "slow down");
    const auto limited = search->search_tracks(context(), {"晴天", 3, std::nullopt});
    CHECK(limited.error().code == ErrorCode::upstream_rate_limited);
    CHECK(limited.error().retryable);
    CHECK(detail_reason(limited.error(), "http_status") == "429");

    upstream->respond(503, "busy");
    const auto unavailable = search->search_tracks(context(), {"晴天", 3, std::nullopt});
    CHECK(unavailable.error().code == ErrorCode::upstream_failed);
    CHECK(unavailable.error().retryable);
    CHECK(detail_reason(unavailable.error(), "http_status") == "503");

    upstream->defer("origin");
    const auto deferred = search->search_tracks(context(), {"晴天", 3, std::nullopt});
    CHECK(deferred.error().code == ErrorCode::service_busy);
    CHECK(deferred.error().retryable);
    CHECK(detail_reason(deferred.error(), "admission") == "origin");
}

TEST_CASE("kuwo details and links", "[providers][kuwo]") {
    auto upstream = std::make_shared<FakeUpstream>();
    auto detail = unwrap(providers::kuwo::make_detail(setup(upstream)));
    upstream->respond(200, fixture("track-info-78932517.json"));
    const auto track = unwrap(detail->track_detail(context(), test::track("kuwo", "78932517")));
    CHECK(track.title == "晴天");
    CHECK(upstream->calls[0].request.url == "https://wapi.kuwo.cn/api/www/music/musicInfo?mid=78932517&httpsStatus=1&plat=web_www");

    upstream->respond(200, R"({"code":-1,"msg":"歌曲不存在","data":null})");
    CHECK(detail->track_detail(context(), test::track("kuwo", "999999999999")).error().code == ErrorCode::not_found);
    CHECK(detail->track_detail(context(), test::track("qq", "1")).error().code == ErrorCode::validation_failed);
    CHECK(detail->track_detail(context(), test::track("kuwo", "../x")).error().code == ErrorCode::validation_failed);

    auto links = unwrap(providers::kuwo::make_link_parser(setup(nullptr)));
    const auto parsed = unwrap(links->parse_link("https://www.kuwo.cn/play_detail/78932517"));
    CHECK(parsed.kind == "track");
    CHECK(parsed.ref == test::track("kuwo", "78932517"));
    CHECK(providers::kuwo::make_search(setup(nullptr)).error().code == ErrorCode::internal);
}

TEST_CASE("kuwo audio keeps account cookies scoped to each resolver request", "[providers][kuwo][cookie-audio]") {
    auto upstream = std::make_shared<FakeUpstream>();
    auto resolver = unwrap(providers::kuwo::make_resolver(setup(upstream)));
    const auto song = test::track("kuwo", "78932517");
    const auto signed_in = [](const std::string& id, const std::string& cookie) {
        auto ctx = context();
        ctx.identity = unwrap(domain::AuthIdentityKey::make(user("prn_a", "kuwo", id), {1, {}, {}, {}}));
        ctx.secret = std::make_shared<const ports::SecretBytes>(cookie);
        return ctx;
    };
    const auto first = signed_in("cre_first", "userid=12345; websid=synthetic-first");
    const auto second = signed_in("cre_second", "userid=23456; websid=synthetic-second");
    for (const auto& ctx : {context(), first, second, context()}) {
        upstream->respond(200, fixture("audio-78932517-standard.json"));
        const auto audio = unwrap(resolver->resolve_audio(ctx, song, {}));
        const auto& call = upstream->calls.back();
        CHECK(call.base.identity == ctx.identity);
        REQUIRE(call.request.secret_headers.size() == (ctx.secret ? 1 : 0));
        if (ctx.secret) {
            CHECK(call.request.secret_headers[0].name == "Cookie");
            CHECK(call.request.secret_headers[0].value == ctx.secret->reveal());
            CHECK_FALSE(call.request.allow_redirects);
        }
        CHECK(call.request.url.find("websid") == std::string::npos);
        CHECK(call.request.body.empty());
        for (const auto& header : call.request.headers) CHECK(header.name != "Cookie");
        for (const auto& [name, value] : audio.request_headers) {
            CHECK(name != "Cookie");
            CHECK(value.find("synthetic-") == std::string::npos);
        }
    }
    // Metadata calls do not send the audio account, even if a caller supplies that context.
    upstream->respond(200, fixture("track-info-78932517.json"));
    REQUIRE(providers::kuwo::make_detail(setup(upstream)).value()->track_detail(first, song));
    CHECK(upstream->calls.back().request.secret_headers.empty());
    upstream->respond(200, fixture("search-qingtian-pn0-rn3.json"));
    REQUIRE(providers::kuwo::make_search(setup(upstream)).value()->search_tracks(first, {"test", 3, {}}));
    CHECK(upstream->calls.back().request.secret_headers.empty());
}

TEST_CASE("kuwo cookie audio preserves the bound account through quality and failure paths", "[providers][kuwo][cookie-audio]") {
    auto upstream = std::make_shared<FakeUpstream>();
    auto resolver = unwrap(providers::kuwo::make_resolver(setup(upstream)));
    auto ctx = context();
    ctx.identity = unwrap(domain::AuthIdentityKey::make(user("prn_a", "kuwo", "cre_bound"), {1, {}, {}, {}}));
    const std::string cookie = "userid=12345; websid=synthetic-bound";
    ctx.secret = std::make_shared<const ports::SecretBytes>(cookie);
    const auto song = test::track("kuwo", "78932517");
    SECTION("all qualities") {
        for (const auto quality : {domain::Quality::standard, domain::Quality::high,
                                  domain::Quality::lossless, domain::Quality::hires}) {
            const bool mp3 = quality <= domain::Quality::high;
            const auto bitrate = quality == domain::Quality::standard ? 128 : quality == domain::Quality::high ? 320 :
                quality == domain::Quality::lossless ? 2000 : 23000;
            upstream->respond(200, nlohmann::json{{"code", 200}, {"data", {{"url", "https://cdn.kuwo.cn/synthetic"},
                {"rid", 78932517}, {"type", 0}, {"format", mp3 ? "mp3" : "flac"},
                {"bitrate", bitrate}, {"duration", 176}}}}.dump());
            CHECK(unwrap(resolver->resolve_audio(ctx, song, {quality})).actual_quality == quality);
        }
        REQUIRE(upstream->calls.size() == 4);
    }
    SECTION("explicit quality fallback retains the same account") {
        upstream->respond(200, R"({"code":-1})");
        upstream->respond(200, R"({"code":200,"data":{"url":"https://cdn.kuwo.cn/synthetic","rid":78932517,"type":0,"format":"flac","bitrate":2000,"duration":176}})");
        const auto audio = unwrap(resolver->resolve_audio(ctx, song,
            {domain::Quality::hires, domain::QualityPolicy::allow_lower, false}));
        CHECK(audio.actual_quality == domain::Quality::lossless);
        REQUIRE(upstream->calls.size() == 2);
    }
    SECTION("upstream failures are not retried anonymously") {
        upstream->respond(503, "unavailable");
        const auto result = resolver->resolve_audio(ctx, song,
            {domain::Quality::hires, domain::QualityPolicy::allow_lower, false});
        REQUIRE_FALSE(result);
        CHECK(result.error().code == ErrorCode::upstream_failed);
        REQUIRE(upstream->calls.size() == 1);
    }
    SECTION("paid previews remain previews") {
        upstream->respond(200, fixture("audio-228908-substitute.json"));
        const auto audio = unwrap(resolver->resolve_audio(ctx, test::track("kuwo", "228908"), {}));
        CHECK(audio.completeness == domain::Completeness::preview);
        CHECK_FALSE(domain::evaluate_resource({}, {audio.actual_quality, audio.completeness}));
    }
    SECTION("a bound account without its secret cannot turn anonymous") {
        ctx.secret.reset();
        const auto result = resolver->resolve_audio(ctx, song, {});
        REQUIRE_FALSE(result);
        CHECK(result.error().code == ErrorCode::credentials_required);
        CHECK(upstream->calls.empty());
    }
    for (const auto& call : upstream->calls) {
        REQUIRE(call.request.secret_headers.size() == 1);
        CHECK(call.request.secret_headers[0].value == cookie);
        CHECK(call.base.identity == ctx.identity);
        CHECK_FALSE(call.request.allow_redirects);
    }
}

TEST_CASE("kuwo resolves standard quality and reports substitutes as previews", "[providers][kuwo]") {
    auto upstream = std::make_shared<FakeUpstream>();
    auto resolver = unwrap(providers::kuwo::make_resolver(setup(upstream)));
    const auto free_song = test::track("kuwo", "78932517");

    upstream->respond(200, fixture("audio-78932517-standard.json"));
    const auto audio = unwrap(resolver->resolve_audio(context(), free_song, {}));
    const auto& url = upstream->calls[0].request.url;
    CHECK(url.rfind("https://mobi.kuwo.cn/mobi.s?", 0) == 0);
    CHECK(url.find("&br=128kmp3&rid=78932517&user=C_APK_guanwang_") != std::string::npos);
    CHECK(upstream->calls[0].request.operation == "resolve_audio");
    CHECK(audio.actual_track == free_song);
    CHECK(audio.actual_quality == domain::Quality::standard);
    CHECK(audio.completeness == domain::Completeness::full);
    CHECK(audio.container == "mp3");
    CHECK(audio.bitrate_bps == 128000);
    CHECK(audio.url.rfind("https://kw-lv.kuwo.cn/", 0) == 0);
    CHECK(audio.request_headers.size() == 1);
    CHECK(unwrap(domain::evaluate_resource({}, {audio.actual_quality, audio.completeness})).warnings.empty());

    // 黑胶请求返回普通档位时保持真实档位，strict 由统一品质判定拒绝。
    const auto before = upstream->calls.size();
    upstream->respond(200, fixture("audio-78932517-standard.json"));
    auto lower = unwrap(resolver->resolve_audio(context(), free_song, {domain::Quality::hires}));
    CHECK(lower.actual_quality == domain::Quality::standard);
    CHECK(domain::evaluate_resource({domain::Quality::hires}, {lower.actual_quality, lower.completeness})
              .error().code == ErrorCode::quality_unavailable);
    CHECK(upstream->calls.size() == before + 1);
    upstream->respond(200, fixture("audio-78932517-standard.json"));
    CHECK(resolver->resolve_audio(context(), free_song, {domain::Quality::high, domain::QualityPolicy::allow_lower, false})
              .has_value());

    // Paid song: the clip is a preview with no quality claim, rejected unless previews are allowed.
    upstream->respond(200, fixture("audio-228908-substitute.json"));
    const auto clip = unwrap(resolver->resolve_audio(context(), test::track("kuwo", "228908"), {}));
    CHECK(clip.completeness == domain::Completeness::preview);
    CHECK_FALSE(clip.actual_quality);
    CHECK(domain::evaluate_resource({}, {clip.actual_quality, clip.completeness}).error().code ==
          ErrorCode::preview_rejected);

    upstream->respond(200, fixture("audio-missing.json"));
    CHECK(resolver->resolve_audio(context(), test::track("kuwo", "999999999999"), {}).error().code ==
          ErrorCode::upstream_failed);

    // Without a readable type the address is not declared as the whole song.
    for (const auto* type : {"", R"("type":null,)", R"("type":"x",)", R"("type":false,)"}) {
        INFO(type);
        upstream->respond(200, std::string{R"({"code":200,"data":{)"} + type +
                                   R"("url":"http://kw-lv.kuwo.cn/a.mp3","rid":78932517,"bitrate":128,)"
                                   R"("format":"mp3","duration":176}})");
        const auto unconfirmed = unwrap(resolver->resolve_audio(context(), free_song, {}));
        CHECK(unconfirmed.completeness == domain::Completeness::unknown);
        CHECK(unconfirmed.actual_quality == domain::Quality::standard);
        const auto verdict =
            unwrap(domain::evaluate_resource({}, {unconfirmed.actual_quality, unconfirmed.completeness}));
        CHECK(verdict.warnings == std::vector{domain::ResourceWarning::full_length_unverified});
    }

    upstream->respond(429, "slow down");
    const auto limited = resolver->resolve_audio(context(), free_song, {});
    CHECK(limited.error().code == ErrorCode::upstream_rate_limited);
    CHECK(limited.error().retryable);
}

TEST_CASE("kuwo vinyl maps to hires and requires native full FLAC verification", "[providers][kuwo][kuwo-vinyl]") {
    auto upstream = std::make_shared<FakeUpstream>();
    auto resolver = unwrap(providers::kuwo::make_resolver(setup(upstream)));
    const auto song = test::track("kuwo", "492445332");
    upstream->respond(200, R"({"code":200,"data":{"url":"https://kw-lv.kuwo.cn/vinyl.flac","rid":492445332,"type":0,"format":"flac","bitrate":23000,"duration":296}})");
    const auto audio = unwrap(resolver->resolve_audio(context(), song, {domain::Quality::hires}));
    CHECK(audio.actual_quality == domain::Quality::hires);
    CHECK(audio.completeness == domain::Completeness::full);
    CHECK_FALSE(audio.bitrate_bps); // 23000 是档位，不是码率。
    CHECK(audio.processing == ports::ProcessingPlan{"audio.kuwo-flac", 1, {}});
    CHECK(upstream->calls.front().request.url.find("&br=23000kflac&rid=492445332&") != std::string::npos);
    const auto bundle = providers::kuwo::make_bundle();
    CHECK(std::find(bundle.quality_hints.begin(), bundle.quality_hints.end(), domain::Quality::hires) != bundle.quality_hints.end());
}

TEST_CASE("kuwo vinyl downgrade is explicit bounded and never hides network failures", "[providers][kuwo][kuwo-vinyl]") {
    const auto missing = R"({"code":200,"data":{"url":"None"}})";
    const auto flac = R"({"code":200,"data":{"url":"https://kw-lv.kuwo.cn/normal.flac","rid":492445332,"type":0,"format":"flac","bitrate":2000,"duration":296}})";
    const auto opaque = R"({"code":200,"data":{"url":"https://kw-lv.kuwo.cn/opaque.mflac","rid":492445332,"type":0,"format":"mflac","bitrate":23000,"duration":296}})";
    const auto song = test::track("kuwo", "492445332");
    for (const auto* unavailable : {missing, opaque}) {
        auto upstream = std::make_shared<FakeUpstream>();
        auto resolver = unwrap(providers::kuwo::make_resolver(setup(upstream)));
        upstream->respond(200, unavailable);
        CHECK(resolver->resolve_audio(context(), song, {domain::Quality::hires}).error().code == ErrorCode::quality_unavailable);
        CHECK(upstream->calls.size() == 1);
        upstream->respond(200, unavailable); upstream->respond(200, flac);
        const auto lower = unwrap(resolver->resolve_audio(context(), song, {domain::Quality::hires, domain::QualityPolicy::allow_lower}));
        CHECK(lower.actual_quality == domain::Quality::lossless);
        CHECK(lower.processing == ports::ProcessingPlan{});
        CHECK_FALSE(lower.bitrate_bps);
        CHECK(upstream->calls.size() == 3);
        CHECK(upstream->calls.back().request.url.find("&br=2000kflac&") != std::string::npos);
        CHECK(unwrap(domain::evaluate_resource({domain::Quality::hires, domain::QualityPolicy::allow_lower},
              {lower.actual_quality, lower.completeness})).warnings == std::vector{domain::ResourceWarning::quality_lowered});
    }
    for (const int status : {200, 429, 503}) {
        auto upstream = std::make_shared<FakeUpstream>();
        auto resolver = unwrap(providers::kuwo::make_resolver(setup(upstream)));
        upstream->respond(status, "malformed response");
        CHECK_FALSE(resolver->resolve_audio(context(), song, {domain::Quality::hires, domain::QualityPolicy::allow_lower}));
        CHECK(upstream->calls.size() == 1);
    }
    // 上游黑胶回落到标准音源、而中间档不存在时，仍保留已得到的同曲标准资源。
    auto upstream = std::make_shared<FakeUpstream>();
    auto resolver = unwrap(providers::kuwo::make_resolver(setup(upstream)));
    upstream->respond(200, R"({"code":200,"data":{"url":"https://kw-lv.kuwo.cn/a.mp3","rid":492445332,"type":0,"format":"mp3","bitrate":128,"duration":296}})");
    upstream->respond(200, missing); upstream->respond(200, missing);
    const auto standard = unwrap(resolver->resolve_audio(context(), song, {domain::Quality::hires, domain::QualityPolicy::allow_lower}));
    CHECK(standard.actual_quality == domain::Quality::standard);
    CHECK(upstream->calls.size() == 3);
    upstream->respond(200, R"({"code":500,"data":null})");
    const auto failed = resolver->resolve_audio(context(), song, {domain::Quality::hires, domain::QualityPolicy::allow_lower});
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == ErrorCode::upstream_failed);
    CHECK(upstream->calls.size() == 4);
}

// Live probe of the real service, opt-in: MUSIC_SERVICE_KUWO_ONLINE=1. Kuwo may limit
// access by region, so CI does not run it; results are recorded in the T12 record.
TEST_CASE("kuwo live protocol probe", "[providers][online-kuwo]") {
    if (infrastructure::config::process_environment("MUSIC_SERVICE_KUWO_ONLINE") != "1") {
        SKIP("set MUSIC_SERVICE_KUWO_ONLINE=1 to run");
    }
    auto clock = std::make_shared<infrastructure::SystemClock>();
    auto transport = unwrap(infrastructure::transport::HttplibTransport::create({}));
    auto ledger = std::make_shared<runtime::AdmissionLedger>(runtime::AdmissionLimits{}, clock);
    auto upstream = std::make_shared<application::transport::UpstreamClient>(ledger, transport, clock);
    const auto kuwo_setup = setup(upstream);

    auto search = unwrap(providers::kuwo::make_search(kuwo_setup));
    const auto page = unwrap(search->search_tracks(context(), {"晴天", 10, std::nullopt}));
    REQUIRE_FALSE(page.page.items.empty());
    WARN("search: " << page.page.items.size() << " of " << *page.page.page.total);

    auto resolver = unwrap(providers::kuwo::make_resolver(kuwo_setup));
    auto detail = unwrap(providers::kuwo::make_detail(kuwo_setup));
    int full = 0;
    int previews = 0;
    for (const auto& track : page.page.items) {
        auto audio = resolver->resolve_audio(context(), track.ref, {});
        if (!audio) {
            WARN(track.ref.id.value() << ": " << domain::to_string(audio.error().code));
            continue;
        }
        if (audio->completeness == domain::Completeness::preview) {
            ++previews;
            continue;
        }
        if (audio->completeness == domain::Completeness::full && audio->actual_quality == domain::Quality::standard) {
            ++full;
            if (full == 1) {
                const auto details = unwrap(detail->track_detail(context(), track.ref));
                CHECK(details.title == track.title);
                WARN("full standard: " << track.ref.id.value() << " " << details.title << " "
                                       << details.duration_ms.value_or(0) / 1000 << " s, " << audio->url.substr(0, 30));
            }
        }
    }
    WARN("full standard " << full << ", previews " << previews);
    CHECK(full + previews > 0);
    CHECK(ledger->usage().total == 0);
}
