// 合成协议样本，不访问网络、不使用真实账号或签名资源。
#include "../domain/test_support.hpp"
#include "application/transport/http_rules.hpp"
#include "providers/bilibili/internal.hpp"
#include "providers/registry/provider_registry.hpp"

#include <catch2/catch_test_macros.hpp>
#include <deque>
#include <stop_token>

using namespace music_service;
using namespace music_service::test;
namespace ports = application::ports;
namespace bili = providers::bilibili;
using Json = nlohmann::json;
using domain::ErrorCode;
namespace {
constexpr auto bv = "BV1xx411c7mD";
constexpr auto bv2 = "BV1xx411c7mE";
constexpr auto secret_cookie = "SESSDATA=synthetic-session; bili_jct=synthetic-csrf";

class FakeUpstream final : public ports::UpstreamHttp {
  public:
    std::deque<ports::HttpResponse> answers;
    std::vector<ports::HttpRequest> calls;
    bool defer{false};
    void raw(Json doc, int status = 200) { answers.push_back({{status, {}, {}}, doc.dump()}); }
    void data(Json doc) { raw({{"code", 0}, {"data", std::move(doc)}}); }
    void search(Json doc) {
        data({{"b_3", "synthetic-browser"}, {"b_4", "synthetic-device+/="}});
        data(std::move(doc));
    }
    domain::Result<ports::HopResult<ports::HttpResponse>>
    fetch(ports::HttpRequest request, const ports::AdmissionRequest& base,
          const ports::OperationContext&) override {
        REQUIRE(application::transport::check_request(request));
        CHECK(base.source.value() == "bilibili");
        CHECK(base.operation == request.operation);
        CHECK(request.max_response_bytes == 4 * 1024 * 1024);
        calls.push_back(request);
        if (defer)
            return ports::HopResult<ports::HttpResponse>{ports::PendingHop{
                std::move(request), {"source", std::chrono::steady_clock::now()}}};
        REQUIRE_FALSE(answers.empty());
        auto response = std::move(answers.front());
        answers.pop_front();
        return ports::HopResult<ports::HttpResponse>{std::move(response)};
    }
    domain::Result<ports::HopResult<ports::HttpResponseHead>>
    stream(ports::HttpRequest, const ports::AdmissionRequest&, const ports::OperationContext&,
           const ports::HeadConsumer&, const ports::ChunkConsumer&) override {
        return domain::Error{ErrorCode::internal, "unexpected stream", false, {}};
    }
};
ports::ProviderContext context(bool auth = false) {
    auto identity = auth
                        ? unwrap(domain::AuthIdentityKey::make(user("prn_a", "bilibili", "cre_1"),
                                                               {1, {}, {}, {}}))
                        : unwrap(domain::AuthIdentityKey::make(anonymous("prn_a", "bilibili"), {}));
    return {{std::chrono::steady_clock::now() + std::chrono::seconds{10}, {}},
            identity,
            {},
            auth ? std::make_shared<ports::SecretBytes>(secret_cookie) : nullptr};
}
struct Fixture {
    std::shared_ptr<FakeUpstream> http = std::make_shared<FakeUpstream>();
    std::shared_ptr<const providers::ProviderRegistry> registry =
        unwrap(providers::ProviderRegistry::build({bili::make_bundle()}, {}, {http}));
    template <class Port> std::shared_ptr<Port> port() {
        return unwrap(registry->require<Port>("bilibili"));
    }
};
Json view_data() {
    return {{"bvid", bv},
            {"title", "视频标题"},
            {"pic", "http://i0.hdslb.com/bfs/archive/test.jpg"},
            {"owner", {{"name", "上传者不是歌手"}}},
            {"duration", 300},
            {"rights", {{"ugc_pay", 0}}},
            {"pages",
             Json::array({{{"cid", 123}, {"page", 1}, {"part", "第一首"}, {"duration", 120}},
                          {{"cid", 456}, {"page", 2}, {"part", "第二首"}, {"duration", 180}}})}};
}
Json audio_row(int id = 30232, int bandwidth = 130000) {
    return {{"id", id},
            {"bandwidth", bandwidth},
            {"mimeType", "audio/mp4"},
            {"codecs", "mp4a.40.2"},
            {"baseUrl",
             "https://upos-test.bilivideo.com/audio.m4s?deadline=4102444800&sign=synthetic"}};
}
Json play_data() {
    return {{"timelength", 179800},
            {"dash", {{"duration", 180}, {"audio", Json::array({audio_row()})}}}};
}
Json search_data(int page = 1) {
    return {{"page", page},
            {"pagesize", 2},
            {"numResults", 5},
            {"numPages", 3},
            {"result", Json::array({{{"bvid", bv},
                                     {"title", "<em class=\"keyword\">中文</em>&amp;音乐"},
                                     {"author", "UP主"},
                                     {"duration", "5:00"}},
                                    {{"bvid", bv2}, {"title", "另一首"}}})}};
}
} // namespace

TEST_CASE("Bilibili advertises only implemented native capabilities", "[providers][bilibili]") {
    Fixture f;
    CHECK(f.registry->instantiated() == 0);
    auto bundle = bili::make_bundle();
    CHECK(bundle.capabilities.size() == 5);
    CHECK(bundle.quality_hints == std::vector{domain::Quality::standard});
    CHECK(bundle.plaintext_endpoints.empty());
    CHECK(unwrap(f.registry->describe_capability("bilibili", "search_tracks")).credentials ==
          ports::CredentialRequirement::optional);
    CHECK_FALSE(f.registry->require<ports::ILyrics>("bilibili"));
    CHECK(f.http->calls.empty());
}

TEST_CASE("Bilibili paid UGC does not imply subscription or download entitlement", "[providers][bilibili][pay]") {
    Fixture f; auto doc = view_data(); doc["rights"]["ugc_pay"] = 1;
    f.http->data(doc);
    const auto value = unwrap(f.port<ports::ITrackDetail>()->track_detail(context(), track("bilibili", std::string{bv} + ":p1")));
    CHECK(value.pay.pay_play == 1); CHECK_FALSE(value.pay.pay_down); CHECK_FALSE(value.pay.pay_month);
}

TEST_CASE("Bilibili search preserves video semantics and advances short pages",
          "[providers][bilibili]") {
    Fixture f;
    auto first = search_data();
    first["result"].erase(1);
    f.http->search(first);
    auto out = unwrap(f.port<ports::ITrackSearch>()->search_tracks(context(), {"中文音乐", 2, {}}));
    REQUIRE(out.page.items.size() == 1);
    CHECK(out.page.items[0].title == "中文&音乐");
    CHECK(out.page.items[0].artists.empty());
    CHECK_FALSE(out.page.items[0].album);
    CHECK_FALSE(out.page.items[0].duration_ms);
    CHECK_FALSE(out.page.items[0].pay.pay_play);
    CHECK_FALSE(out.page.items[0].pay.pay_month);
    CHECK(out.page.items[0].ref.id.value() == std::string{bv} + ":p1");
    CHECK(out.next_cursor == "2:2");
    CHECK(out.page.page.total == 5);
    f.http->search(search_data(2));
    CHECK(unwrap(f.port<ports::ITrackSearch>()->search_tracks(context(),
                                                              {"中文音乐", 2, out.next_cursor}))
              .next_cursor == "3:2");
    CHECK(f.http->calls.back().url.find("page=2&page_size=2") != std::string::npos);
    CHECK(f.http->calls[1].url.find("%E4%B8%AD") != std::string::npos);
    CHECK(f.http->calls[0].secret_headers.empty());
    CHECK(f.http->calls[0].url.ends_with("/x/frontend/finger/spi"));
    REQUIRE(f.http->calls[1].secret_headers.size() == 1);
    CHECK(f.http->calls[1].secret_headers[0].value ==
          "buvid3=synthetic-browser; buvid4=synthetic-device+/=");
}

TEST_CASE("Bilibili validates search pagination and distinguishes empty results",
          "[providers][bilibili]") {
    Fixture f;
    auto port = f.port<ports::ITrackSearch>();
    for (const ports::TrackSearchQuery query : {ports::TrackSearchQuery{"x", 0, {}},
                                                {"", 2, {}},
                                                {"x", 51, {}},
                                                {"x", 2, "2:3"},
                                                {"x", 2, "1001:2"},
                                                {"x", 2, "2junk:2"}})
        CHECK_FALSE(port->search_tracks(context(), query));
    CHECK(f.http->calls.empty());
    auto data = search_data();
    SECTION("empty") {
        data["result"] = Json::array();
        data["numResults"] = 0;
        data["numPages"] = 0;
        f.http->search(data);
        auto out = unwrap(port->search_tracks(context(), {"missing", 2, {}}));
        CHECK(out.page.items.empty());
        CHECK_FALSE(out.next_cursor);
    }
    SECTION("wrong page") {
        data["page"] = 2;
        f.http->search(data);
        CHECK_FALSE(port->search_tracks(context(), {"x", 2, {}}));
    }
    SECTION("missing rows") {
        data.erase("result");
        f.http->search(data);
        CHECK_FALSE(port->search_tracks(context(), {"x", 2, {}}));
    }
    SECTION("negative count") {
        data["numResults"] = -1;
        f.http->search(data);
        CHECK_FALSE(port->search_tracks(context(), {"x", 2, {}}));
    }
}

TEST_CASE("Bilibili part selection becomes stable cid identity without artist fabrication",
          "[providers][bilibili]") {
    Fixture f;
    f.http->data(view_data());
    auto out = unwrap(f.port<ports::ITrackDetail>()->track_detail(
        context(), track("bilibili", std::string{bv} + ":p2")));
    CHECK(out.ref.id.value() == std::string{bv} + ":cid456");
    CHECK(out.duration_ms == 180000);
    CHECK(out.title == "视频标题 - 第二首");
    CHECK(out.artists.empty());
    CHECK_FALSE(out.album);
    CHECK(out.artwork == "https://i0.hdslb.com/bfs/archive/test.jpg");
    CHECK_FALSE(out.track_number);
    CHECK_FALSE(out.track_total);
    auto reordered = view_data();
    reordered["pages"][0]["page"] = 2;
    reordered["pages"][1]["page"] = 1;
    f.http->data(reordered);
    auto same = unwrap(f.port<ports::ITrackDetail>()->track_detail(context(), out.ref));
    CHECK(same.ref == out.ref);
    CHECK(same.duration_ms == out.duration_ms);
}

TEST_CASE("Bilibili never falls back to another part for invalid or removed identity",
          "[providers][bilibili]") {
    Fixture f;
    auto port = f.port<ports::ITrackDetail>();
    for (const auto& id : {"bad", "BV1xx411c7mD:p0", "BV1xx411c7mD:p1001", "BV1xx411c7mD:cid0",
                           "BV1xx411c7mD:p2extra"})
        CHECK_FALSE(port->track_detail(context(), track("bilibili", id)));
    CHECK(f.http->calls.empty());
    auto data = view_data();
    SECTION("missing page") {
        f.http->data(data);
        auto out = port->track_detail(context(), track("bilibili", std::string{bv} + ":p3"));
        REQUIRE_FALSE(out);
        CHECK(out.error().code == ErrorCode::not_found);
    }
    SECTION("missing cid") {
        f.http->data(data);
        auto out = port->track_detail(context(), track("bilibili", std::string{bv} + ":cid789"));
        REQUIRE_FALSE(out);
        CHECK(out.error().code == ErrorCode::not_found);
    }
    SECTION("wrong video") {
        data["bvid"] = bv2;
        f.http->data(data);
        CHECK_FALSE(port->track_detail(context(), track("bilibili", bv)));
    }
    SECTION("duplicate cid") {
        data["pages"][1]["cid"] = 123;
        f.http->data(data);
        CHECK_FALSE(port->track_detail(context(), track("bilibili", bv)));
    }
}

TEST_CASE("Bilibili links are offline with explicit page and strict host validation",
          "[providers][bilibili]") {
    Fixture f;
    auto links = f.port<ports::ILinkParser>();
    CHECK(unwrap(links->parse_link(std::string{"https://www.bilibili.com/video/"} + bv +
                                   "/?p=2&spm_id_from=333"))
              .ref.id.value() == std::string{bv} + ":p2");
    CHECK(unwrap(links->parse_link(std::string{"bilibili:"} + bv)).ref.id.value() ==
          std::string{bv} + ":p1");
    CHECK(unwrap(links->parse_link(std::string{bv} + ":cid123")).ref.id.value() ==
          std::string{bv} + ":cid123");
    for (const auto& link :
         {"https://www.bilibili.com.evil.test/video/BV1xx411c7mD",
          "https://user@www.bilibili.com/video/BV1xx411c7mD",
          "https://www.bilibili.com:444/video/BV1xx411c7mD",
          "https://www.bilibili.com/video/BV1xx411c7mD?p=0",
          "https://www.bilibili.com/video/BV1xx411c7mD?p=2&p=3",
          "https://www.bilibili.com/video/BV1xx411c7mD?cid=123",
          "https://www.bilibili.com/video/BV1xx411c7mD?%70=2", "https://b23.tv/abcdef",
          "https://www.bilibili.com/video/BV1xx411c7mD/other"})
        CHECK_FALSE(links->parse_link(link));
    CHECK(f.http->calls.empty());
}

TEST_CASE("Bilibili resolves independent AAC and keeps cookies away from CDN",
          "[providers][bilibili]") {
    Fixture f;
    auto data = play_data();
    data["dash"]["audio"].push_back(audio_row(30280, 200000));
    f.http->data(view_data());
    f.http->data(data);
    auto out = unwrap(f.port<ports::IAssetResolver>()->resolve_audio(
        context(true), track("bilibili", std::string{bv} + ":p2"), {}));
    CHECK(out.actual_track.id.value() == std::string{bv} + ":cid456");
    CHECK(out.container == "m4a");
    CHECK(out.actual_quality == domain::Quality::standard);
    CHECK(out.completeness == domain::Completeness::full);
    CHECK(out.duration_ms == 179800);
    CHECK(out.bitrate_bps == 200000);
    CHECK_FALSE(out.shareable);
    CHECK(out.expires_at.has_value());
    CHECK(f.http->calls.back().url.find("cid=456&fnval=16") != std::string::npos);
    for (const auto& request : f.http->calls) {
        CHECK(request.url.find("synthetic-session") == std::string::npos);
        REQUIRE(request.secret_headers.size() == 1);
        CHECK(request.secret_headers[0].value == secret_cookie);
    }
    for (const auto& [name, value] : out.request_headers) {
        CHECK(name != "Cookie");
        CHECK(value.find("synthetic-session") == std::string::npos);
    }
}

TEST_CASE("Bilibili strict quality and explicit fallback use only verified tiers",
          "[providers][bilibili]") {
    Fixture f;
    auto port = f.port<ports::IAssetResolver>();
    for (const auto quality :
         {domain::Quality::high, domain::Quality::lossless, domain::Quality::hires}) {
        auto out = port->resolve_audio(context(), track("bilibili", bv),
                                       {quality, domain::QualityPolicy::strict});
        REQUIRE_FALSE(out);
        CHECK(out.error().code == ErrorCode::quality_unavailable);
    }
    CHECK(f.http->calls.empty());
    f.http->data(view_data());
    f.http->data(play_data());
    auto out = unwrap(
        port->resolve_audio(context(), track("bilibili", std::string{bv} + ":p2"),
                            {domain::Quality::lossless, domain::QualityPolicy::allow_lower}));
    CHECK(out.actual_quality == domain::Quality::standard);
}

TEST_CASE("Bilibili rejects trials or shortened audio unless preview is allowed",
          "[providers][bilibili]") {
    Fixture f;
    auto data = play_data();
    auto metadata = view_data();
    SECTION("short audio") { data["timelength"] = 30000; }
    SECTION("explicit trial") { data["is_preview"] = true; }
    SECTION("paid video") { metadata["rights"]["ugc_pay"] = 1; }
    auto port = f.port<ports::IAssetResolver>();
    f.http->data(metadata);
    f.http->data(data);
    auto out = port->resolve_audio(context(), track("bilibili", std::string{bv} + ":p2"), {});
    REQUIRE_FALSE(out);
    CHECK(out.error().code == ErrorCode::preview_rejected);
    f.http->data(metadata);
    f.http->data(data);
    CHECK(unwrap(
              port->resolve_audio(context(), track("bilibili", std::string{bv} + ":p2"),
                                  {domain::Quality::standard, domain::QualityPolicy::strict, true}))
              .completeness == domain::Completeness::preview);
}

TEST_CASE("Bilibili rejects video fallback unsupported codec expired or foreign audio",
          "[providers][bilibili]") {
    Fixture f;
    auto data = play_data();
    auto code = ErrorCode::upstream_failed;
    SECTION("durl video") {
        data.erase("dash");
        data["durl"] = Json::array({{{"url", "https://example.test/video.flv"}}});
        code = ErrorCode::capability_unsupported;
    }
    SECTION("no audio") {
        data["dash"]["audio"] = nullptr;
        code = ErrorCode::not_found;
    }
    SECTION("unsupported codec") {
        data["dash"]["audio"][0]["codecs"] = "ec-3";
        code = ErrorCode::quality_unavailable;
    }
    SECTION("expired") {
        data["dash"]["audio"][0]["baseUrl"] = "https://cdn.bilivideo.com/a?deadline=1";
        code = ErrorCode::resource_expired;
    }
    SECTION("foreign host") {
        data["dash"]["audio"][0]["baseUrl"] = "https://bilivideo.com.evil.test/a";
    }
    SECTION("plaintext") { data["dash"]["audio"][0]["baseUrl"] = "http://cdn.bilivideo.com/a"; }
    SECTION("wrong cid") { data["cid"] = 123; }
    SECTION("excess duration") { data["timelength"] = 300000; }
    f.http->data(view_data());
    f.http->data(data);
    auto out = f.port<ports::IAssetResolver>()->resolve_audio(
        context(), track("bilibili", std::string{bv} + ":p2"), {});
    REQUIRE_FALSE(out);
    CHECK(out.error().code == code);
}

TEST_CASE("Bilibili maps upstream errors without leaking response or credentials",
          "[providers][bilibili]") {
    for (const auto& [code, expected] : {std::pair{-101, ErrorCode::credentials_required},
                                         {-404, ErrorCode::not_found},
                                         {-412, ErrorCode::upstream_rate_limited},
                                         {-352, ErrorCode::upstream_rate_limited},
                                         {999, ErrorCode::upstream_failed}}) {
        Fixture f;
        f.http->raw({{"code", code}, {"message", "sensitive-upstream-body"}});
        auto out = f.port<ports::ITrackDetail>()->track_detail(context(), track("bilibili", bv));
        REQUIRE_FALSE(out);
        CHECK(out.error().code == expected);
        CHECK(out.error().message.find("sensitive") == std::string::npos);
    }
}

TEST_CASE("Bilibili propagates admission cancellation deadline and HTTP failure",
          "[providers][bilibili]") {
    Fixture f;
    auto c = context();
    auto expected = ErrorCode::canceled;
    std::stop_source stop;
    SECTION("cancel") {
        stop.request_stop();
        c.operation.stop = stop.get_token();
    }
    SECTION("deadline") {
        c.operation.deadline = std::chrono::steady_clock::now();
        expected = ErrorCode::deadline_exceeded;
    }
    SECTION("admission") {
        f.http->defer = true;
        expected = ErrorCode::service_busy;
    }
    SECTION("HTTP rate limit") {
        f.http->raw({}, 429);
        expected = ErrorCode::upstream_rate_limited;
    }
    auto out = f.port<ports::ITrackDetail>()->track_detail(c, track("bilibili", bv));
    REQUIRE_FALSE(out);
    CHECK(out.error().code == expected);
    if (expected == ErrorCode::canceled || expected == ErrorCode::deadline_exceeded)
        CHECK(f.http->calls.empty());
}

TEST_CASE("Bilibili verifier reports login validity without claiming membership",
          "[providers][bilibili]") {
    Fixture f;
    auto verifier = unwrap(
        f.registry->require_verifier("bilibili", "web_cookie", ports::CredentialKind::cookie));
    const auto c = context(true);
    ports::SecretBytes secret{secret_cookie};
    SECTION("valid") {
        f.http->data({{"isLogin", true}, {"uname", "private-name"}, {"vipStatus", 1}});
        auto out = unwrap(verifier->verify({c.identity, secret}, c.operation));
        CHECK(out.status == ports::VerificationStatus::valid);
        CHECK_FALSE(out.account_hint);
        CHECK(out.scopes.empty());
    }
    SECTION("logged out") {
        f.http->raw({{"code", -101}, {"data", {{"isLogin", false}}}});
        CHECK(unwrap(verifier->verify({c.identity, secret}, c.operation)).status ==
              ports::VerificationStatus::invalid);
    }
    SECTION("missing session") {
        ports::SecretBytes empty{"buvid3=anonymous"};
        CHECK(unwrap(verifier->verify({c.identity, empty}, c.operation)).status ==
              ports::VerificationStatus::invalid);
        CHECK(f.http->calls.empty());
    }
    SECTION("risk control is not invalid credentials") {
        f.http->raw({{"code", -412}});
        auto out = verifier->verify({c.identity, secret}, c.operation);
        REQUIRE_FALSE(out);
        CHECK(out.error().code == ErrorCode::upstream_rate_limited);
    }
}

TEST_CASE("Bilibili bounds JSON responses and rejects malformed envelopes",
          "[providers][bilibili]") {
    Fixture f;
    std::string body;
    SECTION("HTML") { body = "<html>not a JSON response</html>"; }
    SECTION("oversized") { body = std::string(4 * 1024 * 1024 + 1, ' '); }
    SECTION("deep JSON") {
        body = "{\"code\":0,\"data\":" + std::string(40, '[') + "0" + std::string(40, ']') + "}";
    }
    SECTION("missing code") { body = "{\"data\":{}}"; }
    SECTION("nonintegral code") { body = "{\"code\":0.0,\"data\":{}}"; }
    f.http->answers.push_back({{200, {}, {}}, body});
    auto out = f.port<ports::ITrackDetail>()->track_detail(context(), track("bilibili", bv));
    REQUIRE_FALSE(out);
    CHECK(out.error().code == ErrorCode::upstream_failed);
}

TEST_CASE("Bilibili accepts alternate DASH field spelling and preserves unknown length",
          "[providers][bilibili]") {
    Fixture f;
    auto metadata = view_data();
    metadata["pages"][1]["duration"] = 0;
    auto data = play_data();
    auto& row = data["dash"]["audio"][0];
    row["base_url"] = row["baseUrl"];
    row.erase("baseUrl");
    row["mime_type"] = row["mimeType"];
    row.erase("mimeType");
    f.http->data(metadata);
    f.http->data(data);
    auto out = unwrap(f.port<ports::IAssetResolver>()->resolve_audio(
        context(), track("bilibili", std::string{bv} + ":p2"), {}));
    CHECK(out.completeness == domain::Completeness::unknown);
    CHECK(out.duration_ms == 179800);
    CHECK(out.container == "m4a");
}

TEST_CASE("Bilibili visitor session stays private and does not mutate account credentials",
          "[providers][bilibili]") {
    Fixture f;
    auto c = context(true);
    SECTION("merge temporary visitor") {
        f.http->search(search_data());
        REQUIRE(f.port<ports::ITrackSearch>()->search_tracks(c, {"x", 2, {}}));
        REQUIRE(f.http->calls.size() == 2);
        CHECK(c.secret->reveal() == secret_cookie);
        CHECK(f.http->calls[1].secret_headers[0].value.find("buvid3=synthetic-browser") !=
              std::string::npos);
        CHECK(f.http->calls[1].secret_headers[0].value.find("SESSDATA=synthetic-session") !=
              std::string::npos);
        CHECK(f.http->calls[1].url.find("synthetic") == std::string::npos);
    }
    SECTION("existing visitor does not bootstrap") {
        c.secret = std::make_shared<ports::SecretBytes>("buvid3=existing-browser");
        f.http->data(search_data());
        REQUIRE(f.port<ports::ITrackSearch>()->search_tracks(c, {"x", 2, {}}));
        REQUIRE(f.http->calls.size() == 1);
        CHECK(f.http->calls[0].secret_headers[0].value == "buvid3=existing-browser");
    }
    SECTION("reject cookie injection from bootstrap") {
        f.http->data({{"b_3", "bad; SESSDATA=untrusted"}});
        auto out = f.port<ports::ITrackSearch>()->search_tracks(c, {"x", 2, {}});
        REQUIRE_FALSE(out);
        CHECK(out.error().code == ErrorCode::upstream_failed);
        CHECK(f.http->calls.size() == 1);
    }
}
