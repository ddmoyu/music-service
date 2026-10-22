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
          weapi_body_of(R"({"id":5257138,"lv":-1,"kv":-1,"tv":-1,"csrf_token":""})"));

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
    CHECK(track.version.kind == domain::VersionKind::unknown);

    CHECK(parsing::parse_detail(fixture("detail-missing.json"), "99999999999").error().code == ErrorCode::not_found);
    CHECK(detail_reason(parsing::parse_detail(fixture("detail-5257138.json"), "186016").error(), "protocol") ==
          "detail is about another song");
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

TEST_CASE("netease audio: standard anonymous, accounts for the rest", "[providers][netease]") {
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

    // A higher tier is not implemented: strict fails without a call, allow_lower falls back.
    const auto calls = upstream->calls.size();
    const auto strict_high =
        resolver->resolve_audio(anonymous_context(), test::track("netease", "5257138"),
                                {domain::Quality::high, domain::QualityPolicy::strict, false});
    CHECK(strict_high.error().code == ErrorCode::quality_unavailable);
    CHECK(upstream->calls.size() == calls);

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
    const auto valid = unwrap(verifier->verify({identity, cookie}, operation));
    CHECK(valid.status == ports::VerificationStatus::valid);
    CHECK(valid.account_hint == "测试用户");
    CHECK(valid.scopes == std::vector<std::string>{"vip"});
    REQUIRE(upstream->calls.size() == 1);
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
                            "verify_credentials"}) {
        INFO(key);
        CHECK(std::find(keys.begin(), keys.end(), key) != keys.end());
    }
    REQUIRE(view.auth_methods.size() == 1);
    CHECK(view.auth_methods[0].auth_profile == "web_cookie");
    CHECK(view.auth_methods[0].kind == ports::CredentialKind::cookie);
    CHECK(view.auth_methods[0].supports_verify);
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
