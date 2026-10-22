// 5sing protocol package against desensitized responses recorded on 2026-10-22
// (tests/fixtures/providers/fivesing) and a fake upstream; no network. The opt-in
// [online-fivesing] case probes the live service.

#include "../domain/test_support.hpp"

#include "application/transport/http_rules.hpp"
#include "application/transport/upstream_client.hpp"
#include "application/transport/url.hpp"
#include "infrastructure/clock.hpp"
#include "infrastructure/config/config_loader.hpp"
#include "infrastructure/transport/httplib_transport.hpp"
#include "providers/fivesing/bundle.hpp"
#include "providers/fivesing/capabilities.hpp"
#include "providers/fivesing/parsing/parsers.hpp"
#include "providers/fivesing/protocol/requests.hpp"
#include "providers/registry/provider_registry.hpp"
#include "runtime/admission/admission_ledger.hpp"

#include <catch2/catch_test_macros.hpp>

#include <deque>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace music_service;
using namespace music_service::test;
namespace ports = application::ports;
namespace parsing = providers::fivesing::parsing;
namespace protocol = providers::fivesing::protocol;
using domain::ErrorCode;

namespace {

std::string fixture(const char* name) {
    std::ifstream file(std::filesystem::path{MUSIC_SERVICE_TEST_FIXTURES} / "providers" / "fivesing" / name,
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

// Answers queued responses in order and records what was asked. An answer may be a
// transport error instead of a response.
class FakeUpstream final : public ports::UpstreamHttp {
public:
    struct Call {
        ports::HttpRequest request;
        ports::AdmissionRequest base;
    };

    void respond(int status, std::string body, std::vector<ports::HttpHeader> headers = {}) {
        answers_.push_back(ports::HttpResponse{{status, std::move(headers), {}}, std::move(body)});
    }
    // A HEAD answer with the given length.
    void respond_length(int status, std::uint64_t length) {
        respond(status, {}, {{"Content-Length", std::to_string(length)}, {"Content-Type", "audio/mpeg"}});
    }
    void fail(domain::Error error) { answers_.push_back(std::move(error)); }
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
        if (auto* error = std::get_if<domain::Error>(&answer)) {
            return std::move(*error);
        }
        return ports::HopResult<ports::HttpResponse>{std::get<ports::HttpResponse>(std::move(answer))};
    }
    domain::Result<ports::HopResult<ports::HttpResponseHead>> stream(ports::HttpRequest, const ports::AdmissionRequest&,
                                                                     const ports::OperationContext&,
                                                                     const ports::HeadConsumer&,
                                                                     const ports::ChunkConsumer&) override {
        return domain::Error{ErrorCode::internal, "not used", false, {}};
    }

    std::vector<Call> calls;

private:
    std::deque<std::variant<ports::HttpResponse, domain::Error>> answers_;
    std::optional<std::string> deferred_;
};

ports::ProviderContext context() {
    return {{std::chrono::steady_clock::now() + std::chrono::seconds(10), {}},
            unwrap(domain::AuthIdentityKey::make(anonymous("prn_a", "fivesing"), {})),
            std::nullopt};
}

ports::ProviderSetup setup(std::shared_ptr<ports::UpstreamHttp> upstream) {
    return {source("fivesing"), "{}", std::move(upstream), "fivesing_web"};
}

domain::Error timeout() {
    domain::Error error{ErrorCode::upstream_failed, "read timed out", true, {}};
    error.details.push_back({"transport", "timeout"});
    return error;
}

} // namespace

TEST_CASE("5sing work ids pair the type with the number", "[providers][fivesing]") {
    const auto key = unwrap(parsing::parse_work_key("fc-16394286"));
    CHECK(key.type == "fc");
    CHECK(key.id == "16394286");
    CHECK(key.platform_id() == "fc-16394286");
    // The same number under another type is another work.
    CHECK(unwrap(parsing::parse_work_key("yc-16394286")) != key);
    for (const auto* bad : {"16394286", "fc16394286", "fc-", "-1", "xx-1", "fc-01", "fc-1a", "FC-1", "fc-1-2",
                            "fc-1234567890123"}) {
        INFO(bad);
        CHECK(parsing::parse_work_key(bad).error().code == ErrorCode::validation_failed);
    }
}

TEST_CASE("5sing search responses map to works", "[providers][fivesing]") {
    const auto page = unwrap(parsing::parse_search(fixture("search-qiannianjiu-p1.json")));
    CHECK(page.page == 1);
    CHECK(page.total == 80579);
    CHECK(page.total_pages == 8058);
    REQUIRE(page.tracks.size() == 10);

    const auto& first = page.tracks[0];
    CHECK(first.ref == test::track("fivesing", "fc-16394286"));
    CHECK(first.title == "千年食谱颂");
    REQUIRE(first.artists.size() == 1);
    CHECK(first.artists[0].name == "琴酒蜀黍"); // the uploader who sings it
    CHECK_FALSE(first.artists[0].ref);
    CHECK_FALSE(first.album);
    CHECK_FALSE(first.duration_ms); // never estimated from songSize
    CHECK(first.version.kind == domain::VersionKind::cover);
    CHECK(first.version.label == "翻唱");

    // An original and an accompaniment keep their own types.
    const auto& original = page.tracks[2];
    CHECK(original.ref.id.value() == "yc-1150287");
    CHECK(original.version.kind == domain::VersionKind::unknown);
    CHECK(original.version.label == "原创");
    const auto accompaniment = std::find_if(page.tracks.begin(), page.tracks.end(), [](const domain::Track& t) {
        return t.ref.id.value().rfind("bz-", 0) == 0;
    });
    REQUIRE(accompaniment != page.tracks.end());
    CHECK(accompaniment->version.kind == domain::VersionKind::instrumental);
    for (const auto& track : page.tracks) {
        CHECK(track.title.find('<') == std::string::npos); // highlight markup removed
        CHECK_FALSE(track.duration_ms);
    }

    const auto empty = unwrap(parsing::parse_search(fixture("search-empty.json")));
    CHECK(empty.tracks.empty());
    CHECK(empty.total == 0);

    CHECK(parsing::parse_search("[]").error().code == ErrorCode::upstream_failed);
    CHECK(parsing::parse_search(R"({"list":[]})").error().code == ErrorCode::upstream_failed);
    const auto broken = parsing::parse_search(R"({"list":[{"songName":"x"}],"pageInfo":{"cur":1,"totalCount":1,"totalPages":1}})");
    CHECK(detail_reason(broken.error(), "protocol") == "search item lacks songId or songName");
    // Entries of another kind are left out, not mistaken for works.
    const auto other = unwrap(parsing::parse_search(
        R"({"list":[{"songId":5,"songName":"x","typeEname":"mv"}],"pageInfo":{"cur":1,"totalCount":1,"totalPages":1}})"));
    CHECK(other.tracks.empty());
}

TEST_CASE("5sing text cleaning", "[providers][fivesing]") {
    CHECK(parsing::clean_text(R"( <em class="keyword">千年</em>（伴奏） )") == "千年（伴奏）");
    CHECK(parsing::clean_text("A &amp; B &#39;C&#39; &#x4E2D;") == "A & B 'C' 中");
    CHECK(parsing::clean_text("a & b") == "a & b");
}

TEST_CASE("5sing song answers give details and lyrics of exactly that work", "[providers][fivesing]") {
    const auto cover = test::track("fivesing", "fc-16394286");
    const auto info = unwrap(parsing::parse_song_info(fixture("newget-fc-16394286.json"), {"fc", "16394286"}));
    CHECK(info.track.ref == cover);
    CHECK(info.track.title == "千年食谱颂");
    REQUIRE(info.track.artists.size() == 1);
    CHECK(info.track.artists[0].name == "琴酒蜀黍");
    CHECK_FALSE(info.track.album);
    CHECK_FALSE(info.track.duration_ms);
    CHECK(info.track.version.kind == domain::VersionKind::cover);
    CHECK_FALSE(info.lyrics.unavailable);
    CHECK(info.lyrics.lrc == "[ti:placeholder]\n[00:00.00]line one\n[00:03.37]line two");

    const auto silent = unwrap(parsing::parse_song_info(fixture("newget-yc-1150287.json"), {"yc", "1150287"}));
    CHECK(silent.lyrics.unavailable);
    CHECK_FALSE(silent.lyrics.lrc);

    CHECK(parsing::parse_song_info(fixture("newget-missing.json"), {"fc", "999999999"}).error().code ==
          ErrorCode::not_found);
    // The answer must be about the requested work: same number, same type.
    const auto other = parsing::parse_song_info(fixture("newget-fc-16394286.json"), {"yc", "16394286"});
    CHECK(detail_reason(other.error(), "protocol") == "song response is about another work");
    const auto refused = parsing::parse_song_info(R"({"success":false,"code":5,"data":[]})", {"fc", "1"});
    CHECK(refused.error().code == ErrorCode::upstream_failed);
    CHECK(detail_reason(refused.error(), "fivesing_code") == "5");
    // Untimed text is not LRC.
    CHECK(unwrap(parsing::parse_song_info(
                     R"({"success":true,"data":{"ID":1,"SK":"fc","SN":"x","dynamicWords":"just words"}})", {"fc", "1"}))
              .lyrics.unavailable);
}

TEST_CASE("5sing audio answers list tiers with primary and backup addresses", "[providers][fivesing]") {
    const auto tiers = unwrap(parsing::parse_song_urls(fixture("url-bz-2493423.json"), {"bz", "2493423"}));
    REQUIRE(tiers.size() == 3);
    CHECK(tiers[0].name == "sq");
    CHECK(tiers[0].size == 7592289);
    CHECK(tiers[0].ext == "mp3");
    CHECK(tiers[0].md5 == "a103704da701aab8da47f046fe1fb81f");
    CHECK(tiers[0].primary.rfind("https://wsaudiobssdlbig.kugou.com/", 0) == 0);
    CHECK(tiers[0].backup.rfind("https://wsaudiobssdlbig.cloud.kugou.com/", 0) == 0);
    CHECK(tiers[1].name == "hq");
    CHECK(tiers[2].name == "lq");

    const auto lq_only = unwrap(parsing::parse_song_urls(fixture("url-bz-3177109.json"), {"bz", "3177109"}));
    REQUIRE(lq_only.size() == 1);
    CHECK(lq_only[0].name == "lq");

    CHECK(parsing::parse_song_urls(fixture("url-missing.json"), {"fc", "999999999"}).error().code ==
          ErrorCode::not_found);
    CHECK(detail_reason(parsing::parse_song_urls(fixture("url-fc-16394286.json"), {"yc", "16394286"}).error(),
                        "protocol") == "audio response is about another work");
}

TEST_CASE("5sing song links", "[providers][fivesing]") {
    CHECK(unwrap(parsing::parse_track_link("http://5sing.kugou.com/fc/16394286.html")) ==
          parsing::WorkKey{"fc", "16394286"});
    CHECK(unwrap(parsing::parse_track_link("https://5sing.kugou.com/yc/1150287.html?from=share")) ==
          parsing::WorkKey{"yc", "1150287"});
    for (const auto* bad : {"http://5sing.kugou.com/fc/down/16394286", "http://5sing.kugou.com/mv/1.html",
                            "http://5sing.kugou.com/fc/01.html", "http://evil.example/fc/1.html",
                            "http://5sing.kugou.com.evil.example/fc/1.html", "5sing.kugou.com/fc/1.html",
                            "http://5sing.kugou.com/fc/.html"}) {
        INFO(bad);
        CHECK(parsing::parse_track_link(bad).error().code == ErrorCode::validation_failed);
    }
}

TEST_CASE("5sing API requests are registered plaintext endpoints without secrets", "[providers][fivesing]") {
    const auto bundle = providers::fivesing::make_bundle();
    CHECK(bundle.plaintext_endpoints.size() == 4);
    for (const auto& request : {protocol::search_tracks("千年", 1), protocol::song_info("fc", "1", "track_detail"),
                                protocol::song_info("fc", "1", "lyrics"), protocol::song_urls("fc", "1")}) {
        INFO(request.url);
        CHECK(request.url.rfind("http://", 0) == 0);
        CHECK(request.secret_headers.empty());
        CHECK(application::transport::check_request(request).has_value());
    }
    // The registration is exact: another operation or path on the same host is refused.
    auto wrong_operation = protocol::song_urls("fc", "1");
    wrong_operation.operation = "track_detail";
    CHECK_FALSE(application::transport::check_request(wrong_operation).has_value());
    auto wrong_path = protocol::song_info("fc", "1", "track_detail");
    wrong_path.url = "http://mobileapi.5sing.kugou.com/user/get?id=1";
    CHECK_FALSE(application::transport::check_request(wrong_path).has_value());
    auto with_cookie = protocol::song_urls("fc", "1");
    with_cookie.secret_headers.push_back({"Cookie", "a=b"});
    CHECK_FALSE(application::transport::check_request(with_cookie).has_value());
    // Audio probes go to HTTPS only.
    auto probe = protocol::audio_probe("http://wsaudiobssdlbig.kugou.com/a.mp3");
    CHECK_FALSE(application::transport::check_request(probe).has_value());

    // The registry accepts the bundle next to the other built-in sources.
    std::vector<ports::ProviderBundle> bundles;
    bundles.push_back(providers::fivesing::make_bundle());
    const auto registry = unwrap(providers::ProviderRegistry::build(std::move(bundles), {}));
    CHECK(registry->plaintext_endpoints("fivesing").size() == 4);
    const auto view = unwrap(registry->describe("fivesing"));
    CHECK(view.supports("lyrics"));
    CHECK_FALSE(view.supports("search_playlists"));
    CHECK(view.auth_methods.empty());
}

TEST_CASE("5sing search pages through the upstream's fixed pages", "[providers][fivesing]") {
    auto upstream = std::make_shared<FakeUpstream>();
    auto search = unwrap(providers::fivesing::make_search(setup(upstream)));

    // A limit below the page size: the cursor continues within the page.
    upstream->respond(200, fixture("search-qiannianjiu-p1.json"));
    const auto first = unwrap(search->search_tracks(context(), {"千年酒", 4, std::nullopt}));
    REQUIRE(upstream->calls.size() == 1);
    const auto& call = upstream->calls[0];
    CHECK(call.request.url ==
          "http://search.5sing.kugou.com/home/json?keyword=%E5%8D%83%E5%B9%B4%E9%85%92&sort=1&page=1&filter=0&type=0");
    CHECK(call.request.operation == "search_tracks");
    CHECK(call.request.secret_headers.empty());
    CHECK(call.base.principal_id == "prn_a");
    CHECK(call.base.source.value() == "fivesing");
    CHECK(call.base.rate_limit_group == "fivesing_web");
    CHECK(first.page.items.size() == 4);
    CHECK(first.page.page.total == 80579);
    CHECK(first.page.page.has_more == true);
    CHECK(first.next_cursor == "1:4");
    CHECK(first.page.items[0].ref.id.value() == "fc-16394286");

    upstream->respond(200, fixture("search-qiannianjiu-p1.json"));
    const auto second = unwrap(search->search_tracks(context(), {"千年酒", 4, std::string{"1:4"}}));
    CHECK(second.page.items[0].ref == unwrap(parsing::parse_search(fixture("search-qiannianjiu-p1.json"))).tracks[4].ref);
    CHECK(second.next_cursor == "1:8");
    upstream->respond(200, fixture("search-qiannianjiu-p1.json"));
    const auto third = unwrap(search->search_tracks(context(), {"千年酒", 4, std::string{"1:8"}}));
    CHECK(third.page.items.size() == 2); // the rest of the page
    CHECK(third.next_cursor == "2");

    // A larger limit takes the whole upstream page in one call.
    upstream->respond(200, fixture("search-qiannianjiu-p1.json"));
    const auto whole = unwrap(search->search_tracks(context(), {"千年酒", 30, std::nullopt}));
    CHECK(whole.page.items.size() == 10);
    CHECK(whole.page.page.limit == 30);
    CHECK(whole.page.page.returned == 10);
    CHECK(whole.next_cursor == "2");

    // An answer for another page is not taken as the requested one.
    upstream->respond(200, fixture("search-qiannianjiu-p1.json"));
    const auto wrong = search->search_tracks(context(), {"千年酒", 10, std::string{"2"}});
    CHECK(detail_reason(wrong.error(), "protocol") == "search response is for another page");
    CHECK(upstream->calls.back().request.url.find("&page=2&") != std::string::npos);

    upstream->respond(200, fixture("search-empty.json"));
    const auto none = unwrap(search->search_tracks(context(), {"zzqq", 10, std::nullopt}));
    CHECK(none.page.items.empty());
    CHECK(none.page.page.has_more == false);
    CHECK_FALSE(none.next_cursor);

    const auto before = upstream->calls.size();
    for (const auto* cursor : {"x", "0", "1:0", "1:10", "1:", ":3", "10001"}) {
        INFO(cursor);
        CHECK(search->search_tracks(context(), {"千年", 10, std::string{cursor}}).error().code ==
              ErrorCode::cursor_mismatch);
    }
    CHECK(search->search_tracks(context(), {" <em></em> ", 10, std::nullopt}).error().code ==
          ErrorCode::validation_failed);
    CHECK(search->search_tracks(context(), {"a", 51, std::nullopt}).error().code == ErrorCode::validation_failed);
    CHECK(upstream->calls.size() == before); // invalid input never reaches the network

    upstream->respond(429, "slow down", {{"Retry-After", "7"}});
    const auto limited = search->search_tracks(context(), {"千年", 10, std::nullopt});
    CHECK(limited.error().code == ErrorCode::upstream_rate_limited);
    CHECK(detail_reason(limited.error(), "retry_after_seconds") == "7");
    upstream->respond(502, "bad gateway");
    CHECK(search->search_tracks(context(), {"千年", 10, std::nullopt}).error().retryable);
    upstream->defer("origin");
    const auto deferred = search->search_tracks(context(), {"千年", 10, std::nullopt});
    CHECK(deferred.error().code == ErrorCode::service_busy);
    CHECK(detail_reason(deferred.error(), "admission") == "origin");
}

TEST_CASE("5sing details, lyrics and links", "[providers][fivesing]") {
    auto upstream = std::make_shared<FakeUpstream>();
    auto detail = unwrap(providers::fivesing::make_detail(setup(upstream)));
    auto lyrics = unwrap(providers::fivesing::make_lyrics(setup(upstream)));
    const auto cover = test::track("fivesing", "fc-16394286");

    upstream->respond(200, fixture("newget-fc-16394286.json"));
    CHECK(unwrap(detail->track_detail(context(), cover)).title == "千年食谱颂");
    CHECK(upstream->calls[0].request.url == "http://mobileapi.5sing.kugou.com/song/newget?songid=16394286&songtype=fc");
    CHECK(upstream->calls[0].request.operation == "track_detail");

    upstream->respond(200, fixture("newget-fc-16394286.json"));
    CHECK(unwrap(lyrics->lyrics(context(), cover)).lrc.has_value());
    CHECK(upstream->calls[1].request.operation == "lyrics");
    upstream->respond(200, fixture("newget-yc-1150287.json"));
    CHECK(unwrap(lyrics->lyrics(context(), test::track("fivesing", "yc-1150287"))).unavailable);

    upstream->respond(200, fixture("newget-missing.json"));
    CHECK(detail->track_detail(context(), test::track("fivesing", "fc-999999999")).error().code ==
          ErrorCode::not_found);
    CHECK(detail->track_detail(context(), test::track("kuwo", "fc-1")).error().code == ErrorCode::validation_failed);
    CHECK(detail->track_detail(context(), test::track("fivesing", "16394286")).error().code ==
          ErrorCode::validation_failed);
    CHECK(upstream->calls.size() == 4);

    auto links = unwrap(providers::fivesing::make_link_parser(setup(nullptr)));
    const auto parsed = unwrap(links->parse_link("http://5sing.kugou.com/fc/16394286.html"));
    CHECK(parsed.kind == "track");
    CHECK(parsed.ref == cover);
    CHECK(providers::fivesing::make_search(setup(nullptr)).error().code == ErrorCode::internal);
    CHECK(providers::fivesing::make_resolver(setup(nullptr)).error().code == ErrorCode::internal);
}

TEST_CASE("5sing resolves a tier's file, its backup only for the same file", "[providers][fivesing]") {
    auto upstream = std::make_shared<FakeUpstream>();
    auto resolver = unwrap(providers::fivesing::make_resolver(setup(upstream)));
    const auto work = test::track("fivesing", "bz-2493423");
    const auto tiers = unwrap(parsing::parse_song_urls(fixture("url-bz-2493423.json"), {"bz", "2493423"}));

    SECTION("standard is lq at 128 kbit/s, checked with HEAD") {
        upstream->respond(200, fixture("url-bz-2493423.json"));
        upstream->respond_length(200, 3036952);
        const auto audio = unwrap(resolver->resolve_audio(context(), work, {}));
        REQUIRE(upstream->calls.size() == 2);
        CHECK(upstream->calls[0].request.url ==
              "http://mobileapi.5sing.kugou.com/song/getSongUrl?songid=2493423&songtype=bz");
        CHECK(upstream->calls[0].request.operation == "resolve_audio");
        CHECK(upstream->calls[1].request.method == ports::HttpMethod::head);
        CHECK(upstream->calls[1].request.url == tiers[2].primary);
        CHECK(audio.url == tiers[2].primary);
        CHECK(audio.actual_track == work);
        CHECK(audio.actual_quality == domain::Quality::standard);
        CHECK(audio.bitrate_bps == 128000);
        CHECK(audio.container == "mp3");
        CHECK(audio.completeness == domain::Completeness::full);
        CHECK_FALSE(audio.duration_ms); // 5sing never states it
        CHECK_FALSE(audio.expires_at);
        CHECK(unwrap(domain::evaluate_resource({}, {audio.actual_quality, audio.completeness})).warnings.empty());
    }
    SECTION("high is sq at 320 kbit/s") {
        upstream->respond(200, fixture("url-bz-2493423.json"));
        upstream->respond_length(200, 7592289);
        const auto audio =
            unwrap(resolver->resolve_audio(context(), work, {domain::Quality::high, domain::QualityPolicy::strict, false}));
        CHECK(audio.url == tiers[0].primary);
        CHECK(audio.actual_quality == domain::Quality::high);
        CHECK(audio.bitrate_bps == 320000);
    }
    SECTION("the backup serves when the primary does not answer") {
        upstream->respond(200, fixture("url-bz-2493423.json"));
        upstream->fail(timeout());
        upstream->respond_length(200, 3036952);
        const auto audio = unwrap(resolver->resolve_audio(context(), work, {}));
        CHECK(upstream->calls[2].request.url == tiers[2].backup);
        CHECK(audio.url == tiers[2].backup);
        CHECK(audio.completeness == domain::Completeness::full);
    }
    SECTION("a primary answering with another file is not mixed with the backup") {
        upstream->respond(200, fixture("url-bz-2493423.json"));
        upstream->respond_length(200, 1000);    // primary: not the declared size
        upstream->respond_length(200, 3036952); // backup: the declared file
        const auto audio = unwrap(resolver->resolve_audio(context(), work, {}));
        CHECK(audio.url == tiers[2].backup);
    }
    SECTION("both failing is an error, never a guess") {
        upstream->respond(200, fixture("url-bz-2493423.json"));
        upstream->respond(404, {});
        upstream->respond(503, {});
        const auto failed = resolver->resolve_audio(context(), work, {});
        CHECK(failed.error().code == ErrorCode::upstream_failed);
        CHECK(failed.error().retryable);
        CHECK(detail_reason(failed.error(), "http_status") == "503");
    }
    SECTION("a backup naming another file is never tried") {
        auto body = fixture("url-bz-2493423.json");
        const auto backup_file = std::string{"/b0f1a4ac9b4ffb1f5f8a4c1cc32cd8b1.mp3"};
        const auto lq_backup = body.find("lqurl_backup");
        const auto file_at = body.find(".mp3", lq_backup) - 33;
        body.replace(file_at, backup_file.size(), backup_file);
        upstream->respond(200, body);
        upstream->fail(timeout());
        const auto failed = resolver->resolve_audio(context(), work, {});
        CHECK(failed.error().code == ErrorCode::upstream_failed);
        CHECK(upstream->calls.size() == 2); // the primary only
    }
    SECTION("admission refusal stops at the primary") {
        upstream->respond(200, fixture("url-bz-2493423.json"));
        upstream->fail(domain::Error{ErrorCode::service_busy, "busy", true, {}});
        CHECK(resolver->resolve_audio(context(), work, {}).error().code == ErrorCode::service_busy);
        CHECK(upstream->calls.size() == 2);
    }
    SECTION("an address off kugou.com or over HTTP is refused") {
        for (const auto* host : {"http://wsaudiobssdlbig.kugou.com/", "https://evil.example/",
                                 "https://wsaudiobssdlbig.kugou.com:8443/"}) {
            INFO(host);
            auto body = fixture("url-bz-3177109.json");
            for (auto at = body.find("https://wsaudiobssdlbig.kugou.com/"); at != std::string::npos;
                 at = body.find("https://wsaudiobssdlbig.kugou.com/", at + 1)) {
                body.replace(at, std::string_view{"https://wsaudiobssdlbig.kugou.com/"}.size(), host);
            }
            auto cloud = body.find("https://wsaudiobssdlbig.cloud.kugou.com/");
            body.replace(cloud, std::string_view{"https://wsaudiobssdlbig.cloud.kugou.com/"}.size(), host);
            upstream->respond(200, body);
            const auto refused = resolver->resolve_audio(context(), test::track("fivesing", "bz-3177109"), {});
            CHECK(detail_reason(refused.error(), "protocol") ==
                  "audio address is not an HTTPS kugou.com address of the tier's file");
        }
    }
    SECTION("quality follows what the work has") {
        // Only lq: high is unavailable when strict, standard with allow_lower.
        upstream->respond(200, fixture("url-bz-3177109.json"));
        CHECK(resolver->resolve_audio(context(), test::track("fivesing", "bz-3177109"),
                                      {domain::Quality::high, domain::QualityPolicy::strict, false})
                  .error()
                  .code == ErrorCode::quality_unavailable);
        CHECK(upstream->calls.size() == 1);
        upstream->respond(200, fixture("url-bz-3177109.json"));
        upstream->respond_length(200, 4914408);
        const auto lowered = unwrap(resolver->resolve_audio(
            context(), test::track("fivesing", "bz-3177109"), {domain::Quality::high, domain::QualityPolicy::allow_lower, false}));
        CHECK(lowered.actual_quality == domain::Quality::standard);
        // Lossless is never offered: 5sing's best tier is a 320 kbit/s MP3.
        upstream->respond(200, fixture("url-bz-2493423.json"));
        CHECK(resolver->resolve_audio(context(), work, {domain::Quality::lossless, domain::QualityPolicy::strict, false})
                  .error()
                  .code == ErrorCode::quality_unavailable);
    }
    SECTION("an unknown length leaves completeness unknown") {
        upstream->respond(200, fixture("url-bz-2493423.json"));
        upstream->respond(200, {});
        const auto audio = unwrap(resolver->resolve_audio(context(), work, {}));
        CHECK(audio.completeness == domain::Completeness::unknown);
        CHECK(unwrap(domain::evaluate_resource({}, {audio.actual_quality, audio.completeness})).warnings ==
              std::vector{domain::ResourceWarning::full_length_unverified});
    }
    SECTION("a missing work") {
        upstream->respond(200, fixture("url-missing.json"));
        CHECK(resolver->resolve_audio(context(), test::track("fivesing", "fc-999999999"), {}).error().code ==
              ErrorCode::not_found);
    }
}

// Live probe of the real service, opt-in: MUSIC_SERVICE_FIVESING_ONLINE=1. Results are
// recorded in the T22 record.
TEST_CASE("5sing live protocol probe", "[providers][online-fivesing]") {
    if (infrastructure::config::process_environment("MUSIC_SERVICE_FIVESING_ONLINE") != "1") {
        SKIP("set MUSIC_SERVICE_FIVESING_ONLINE=1 to run");
    }
    auto clock = std::make_shared<infrastructure::SystemClock>();
    auto transport = unwrap(infrastructure::transport::HttplibTransport::create({}));
    auto ledger = std::make_shared<runtime::AdmissionLedger>(runtime::AdmissionLimits{}, clock);
    auto upstream = std::make_shared<application::transport::UpstreamClient>(ledger, transport, clock);
    const auto live = setup(upstream);

    auto search = unwrap(providers::fivesing::make_search(live));
    const auto page = unwrap(search->search_tracks(context(), {"千年", 10, std::nullopt}));
    REQUIRE_FALSE(page.page.items.empty());
    WARN("search: " << page.page.items.size() << " of " << *page.page.page.total);
    const auto next = unwrap(search->search_tracks(context(), {"千年", 10, page.next_cursor}));
    CHECK(next.page.items.front().ref != page.page.items.front().ref);

    auto resolver = unwrap(providers::fivesing::make_resolver(live));
    auto detail = unwrap(providers::fivesing::make_detail(live));
    int full = 0;
    for (const auto& track : page.page.items) {
        auto audio = resolver->resolve_audio(context(), track.ref, {});
        if (!audio) {
            WARN(track.ref.id.value() << ": " << domain::to_string(audio.error().code));
            continue;
        }
        if (audio->completeness == domain::Completeness::full) {
            ++full;
        }
        if (full == 1) {
            const auto details = unwrap(detail->track_detail(context(), track.ref));
            CHECK(details.title == track.title);
            WARN("full standard: " << track.ref.id.value() << " " << details.title << " " << audio->url.substr(0, 40));
        }
    }
    WARN("full " << full << " of " << page.page.items.size());
    CHECK(full > 0);
    // high where a work has sq, standard otherwise: never a lower tier claimed as high.
    int high = 0;
    for (const auto& track : page.page.items) {
        auto audio = resolver->resolve_audio(context(), track.ref,
                                             {domain::Quality::high, domain::QualityPolicy::allow_lower, false});
        if (audio && audio->actual_quality == domain::Quality::high) {
            CHECK(audio->bitrate_bps == 320000);
            ++high;
        }
    }
    WARN("high " << high << " of " << page.page.items.size());
    CHECK(ledger->usage().total == 0);
}
