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

#include <deque>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace music_service;
using namespace music_service::test;
namespace ports = application::ports;

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

TEST_CASE("kuwo song pages give details for exactly that song", "[providers][kuwo]") {
    const auto page = fixture("play-detail-78932517.html");
    const auto track = unwrap(parsing::parse_track_page(page, "78932517"));
    CHECK(track.ref.id.value() == "78932517");
    CHECK(track.title == "晴天");
    REQUIRE(track.artists.size() == 1);
    CHECK(track.artists[0].name == "蓝心羽");
    CHECK_FALSE(track.album); // the page shows 未知 (unknown)
    CHECK(track.duration_ms == 176000);

    CHECK(parsing::parse_track_page(fixture("play-detail-missing.html"), "999999999999").error().code ==
          ErrorCode::not_found);
    // A page about another song is not accepted as this one.
    const auto other = parsing::parse_track_page(page, "1");
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
    upstream->respond(200, fixture("play-detail-78932517.html"));
    const auto track = unwrap(detail->track_detail(context(), test::track("kuwo", "78932517")));
    CHECK(track.title == "晴天");
    CHECK(upstream->calls[0].request.url == "https://www.kuwo.cn/play_detail/78932517");

    upstream->respond(430, fixture("play-detail-missing.html"));
    CHECK(detail->track_detail(context(), test::track("kuwo", "999999999999")).error().code == ErrorCode::not_found);
    CHECK(detail->track_detail(context(), test::track("qq", "1")).error().code == ErrorCode::validation_failed);
    CHECK(detail->track_detail(context(), test::track("kuwo", "../x")).error().code == ErrorCode::validation_failed);

    auto links = unwrap(providers::kuwo::make_link_parser(setup(nullptr)));
    const auto parsed = unwrap(links->parse_link("https://www.kuwo.cn/play_detail/78932517"));
    CHECK(parsed.kind == "track");
    CHECK(parsed.ref == test::track("kuwo", "78932517"));
    CHECK(providers::kuwo::make_search(setup(nullptr)).error().code == ErrorCode::internal);
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

    // Higher quality: strict fails before any request; allow_lower settles for standard.
    const auto before = upstream->calls.size();
    CHECK(resolver->resolve_audio(context(), free_song, {domain::Quality::lossless, domain::QualityPolicy::strict, false})
              .error()
              .code == ErrorCode::quality_unavailable);
    CHECK(upstream->calls.size() == before);
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
