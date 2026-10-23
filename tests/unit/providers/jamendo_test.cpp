#include "../domain/test_support.hpp"
#include "application/transport/http_rules.hpp"
#include "providers/jamendo/bundle.hpp"
#include "providers/jamendo/parsing.hpp"
#include "providers/registry/provider_registry.hpp"
#include <catch2/catch_test_macros.hpp>
#include <deque>
#include <fstream>

using namespace music_service;
using namespace music_service::test;
namespace ports = application::ports;
namespace jam = providers::jamendo;
using Json = nlohmann::json;
using domain::ErrorCode;
namespace {
class FakeUpstream final : public ports::UpstreamHttp {
public:
    std::deque<ports::HttpResponse> answers;
    std::vector<ports::HttpRequest> calls;
    std::optional<domain::Error> failure;
    bool deferred{false};
    void answer(Json data, int status = 200) { answers.push_back({{status, {}, {}}, data.dump()}); }
    domain::Result<ports::HopResult<ports::HttpResponse>> fetch(ports::HttpRequest request,
        const ports::AdmissionRequest& admission, const ports::OperationContext&) override {
        REQUIRE(application::transport::check_request(request));
        CHECK(admission.source.value() == "jamendo"); CHECK(admission.operation == request.operation);
        CHECK(request.max_response_bytes == 4 * 1024 * 1024); CHECK(request.secret_headers.empty());
        calls.push_back(request);
        if (failure) return *failure;
        if (deferred) return ports::HopResult<ports::HttpResponse>{ports::PendingHop{std::move(request), {"source", std::chrono::steady_clock::now()}}};
        REQUIRE_FALSE(answers.empty());
        auto value = std::move(answers.front()); answers.pop_front();
        return ports::HopResult<ports::HttpResponse>{std::move(value)};
    }
    domain::Result<ports::HopResult<ports::HttpResponseHead>> stream(ports::HttpRequest,
        const ports::AdmissionRequest&, const ports::OperationContext&, const ports::HeadConsumer&, const ports::ChunkConsumer&) override {
        return domain::Error{ErrorCode::internal, "unexpected stream", false, {}};
    }
};
ports::ProviderContext context() {
    return {{std::chrono::steady_clock::now() + std::chrono::seconds(10), {}},
            unwrap(domain::AuthIdentityKey::make(anonymous("prn_a", "jamendo"), {})), {}, {}};
}
struct Fixture {
    std::shared_ptr<FakeUpstream> upstream = std::make_shared<FakeUpstream>();
    std::shared_ptr<const providers::ProviderRegistry> registry = unwrap(providers::ProviderRegistry::build({jam::make_bundle()}, {}, {upstream}));
    template<class Port> std::shared_ptr<Port> port() { return unwrap(registry->require<Port>("jamendo")); }
};
Json track_json(int value = 1) {
    return {{"id", value}, {"type", "track"}, {"name", "曲目 &amp; 名称"}, {"duration", 16},
        {"albumId", 10}, {"artistId", 20}, {"album", {{"id", 10}, {"name", "专辑"}}},
        {"artist", {{"id", 20}, {"name", "歌手"}}}, {"status", {{"available", true}}},
        {"isDownloadable", 1}, {"download", {{"mp3", "https://prod-1.storage.jamendo.com/download/track/" + std::to_string(value) + "/mp32/"}}},
        {"stream", {{"mp33", "https://prod-1.storage.jamendo.com/?trackid=1&format=mp33"}}}, {"lyrics", ""}};
}
Json album_json() {
    return {{"id", 10}, {"type", "album"}, {"name", "专辑"}, {"artistId", 20},
        {"artist", {{"id", 20}, {"name", "歌手"}}}, {"tracks", Json::array({{{"id", 1}, {"position", 1}}, {{"id", 2}, {"position", 2}}})}};
}
Json playlist_json() {
    return {{"id", 30}, {"type", "playlist"}, {"name", "推荐 &amp; 精选"}, {"status", "public"},
        {"tracks", Json::array({{{"id", 1}, {"position", 0}}, {{"id", 1}, {"position", 1}}, {{"id", 2}, {"position", 2}}})}};
}
Json recorded(const char* name) {
    std::ifstream file(std::string{MUSIC_SERVICE_TEST_FIXTURES} + "/providers/jamendo/" + name);
    REQUIRE(file.good()); return Json::parse(file);
}
}

TEST_CASE("Jamendo registers only verified anonymous native capabilities", "[providers][jamendo]") {
    Fixture f; auto info = unwrap(f.registry->describe("jamendo"));
    CHECK(info.capabilities.size() == 20); CHECK(f.registry->instantiated() == 0);
    for (const auto& capability : info.capabilities) CHECK(capability.spec.credentials == ports::CredentialRequirement::none);
    CHECK(f.registry->require<ports::PlaylistSearch>("jamendo"));
    CHECK_FALSE(f.registry->require<ports::ArtistTracks>("jamendo"));
    CHECK(f.registry->require<ports::ICharts>("jamendo"));
    CHECK(f.registry->require<ports::CategoryTracks>("jamendo"));
    CHECK_FALSE(f.registry->require<ports::CategoryPlaylists>("jamendo")); CHECK(f.upstream->calls.empty());
}
TEST_CASE("Jamendo signature is SHA1 of path and decimal nonce", "[providers][jamendo]") {
    CHECK(jam::digest("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(jam::call_header("/api/search", "0.123456789") == "$19b51f1a66bb9e9c133e97d1a07b795aa57a3ed2*0.123456789~");
    Fixture f; f.upstream->answer(Json::array());
    REQUIRE(f.port<ports::ITrackSearch>()->search_tracks(context(), {"中文 & music", 2, {}}));
    const auto& request = f.upstream->calls.front();
    CHECK(request.url.find("query=%E4%B8%AD%E6%96%87%20%26%20music") != request.url.npos);
    CHECK(request.url.find("limit=3&offset=0") != request.url.npos);
    auto header = std::find_if(request.headers.begin(), request.headers.end(), [](const auto& h) { return h.name == "x-jam-call"; });
    REQUIRE(header != request.headers.end()); const auto star = header->value.find('*');
    CHECK(header->value == jam::call_header("/api/search", std::string_view{header->value}.substr(star + 1, header->value.size() - star - 2)));
}
TEST_CASE("Jamendo search uses lookahead and keeps unknown totals", "[providers][jamendo]") {
    Fixture f; f.upstream->answer(Json::array({track_json(), track_json(2), track_json(3)}));
    auto result = unwrap(f.port<ports::ITrackSearch>()->search_tracks(context(), {"love", 2, {}}));
    REQUIRE(result.page.items.size() == 2); CHECK(result.page.items[0].title == "曲目 & 名称");
    CHECK(result.page.items[0].duration_ms == 16000); CHECK(result.next_cursor == "2"); CHECK_FALSE(result.page.page.total);
    f.upstream->answer(Json::array({track_json(3)}));
    auto second = unwrap(f.port<ports::ITrackSearch>()->search_tracks(context(), {"love", 2, result.next_cursor}));
    CHECK(second.page.items[0].ref.id.value() == "3"); CHECK_FALSE(second.next_cursor);
    CHECK(f.upstream->calls.back().url.find("offset=2") != std::string::npos);
}
TEST_CASE("Jamendo album and artist search select their endpoint types", "[providers][jamendo]") {
    Fixture f;
    f.upstream->answer(Json::array({album_json()}));
    CHECK(unwrap(f.port<ports::AlbumSearch>()->search(context(), {"love", 2, {}})).page.items[0].ref.id.value() == "10");
    CHECK(f.upstream->calls.back().url.find("type=album") != std::string::npos);
    f.upstream->answer(Json::array({{{"id", 20}, {"type", "artist"}, {"name", "歌手"}, {"status", "visible"}}}));
    CHECK(unwrap(f.port<ports::ArtistSearch>()->search(context(), {"love", 2, {}})).page.items[0].name == "歌手");
    CHECK(f.upstream->calls.back().url.find("type=artist") != std::string::npos);
}
TEST_CASE("Jamendo empty search differs from malformed or wrong entity responses", "[providers][jamendo]") {
    Fixture f; Json data = Json::array(); bool success = false;
    SECTION("empty") { success = true; }
    SECTION("object") { data = {{"error", "private upstream text"}}; }
    SECTION("wrong type") { data = Json::array({album_json()}); }
    SECTION("oversized page") { data = Json::array({track_json(), track_json(2), track_json(3), track_json(4)}); }
    SECTION("negative duration") { auto t = track_json(); t["duration"] = -1; data.push_back(t); }
    f.upstream->answer(data); auto result = f.port<ports::ITrackSearch>()->search_tracks(context(), {"x", 2, {}});
    CHECK(static_cast<bool>(result) == success);
    if (success) { CHECK(result->page.items.empty()); CHECK_FALSE(result->next_cursor); }
    else { CHECK(result.error().code == ErrorCode::upstream_failed); CHECK(result.error().message.find("private") == std::string::npos); }
}
TEST_CASE("Jamendo validates refs queries and cursors before networking", "[providers][jamendo]") {
    Fixture f;
    for (const ports::SearchQuery q : {ports::SearchQuery{"", 1, {}}, {" ", 1, {}}, {"x\n", 1, {}}, {"x", 0, {}},
        {"x", 51, {}}, {"x", 1, "0"}, {"x", 1, "10000"}, {"x", 1, "1:invalid"}, {"x", 1, "01"}})
        CHECK_FALSE(f.port<ports::ITrackSearch>()->search_tracks(context(), q));
    CHECK_FALSE(f.port<ports::ITrackDetail>()->track_detail(context(), track("qq", "123")));
    CHECK_FALSE(f.port<ports::AlbumTracks>()->list(context(), jam::parsing::ref("10"), {101, {}}));
    CHECK(f.upstream->calls.empty());
}
TEST_CASE("Jamendo preserves admission cancellation timeout and rate errors", "[providers][jamendo]") {
    Fixture f; ErrorCode expected = ErrorCode::upstream_failed;
    SECTION("deferred") { f.upstream->deferred = true; expected = ErrorCode::service_busy; }
    SECTION("canceled") { expected = ErrorCode::canceled; f.upstream->failure = domain::Error{expected, "canceled", false, {}}; }
    SECTION("timeout") { expected = ErrorCode::upstream_timeout; f.upstream->failure = domain::Error{expected, "timeout", true, {}}; }
    SECTION("rate limit") { expected = ErrorCode::upstream_rate_limited; f.upstream->answer({}, 429); f.upstream->answers.front().head.headers.push_back({"Retry-After", "30"}); }
    SECTION("forbidden is not a fake login request") { f.upstream->answer({}, 403); }
    auto result = f.port<ports::ITrackSearch>()->search_tracks(context(), {"x", 2, {}});
    REQUIRE_FALSE(result); CHECK(result.error().code == expected);
    if (expected == ErrorCode::upstream_rate_limited) {
        CHECK(result.error().retryable); REQUIRE(result.error().details.size() == 2); CHECK(result.error().details[1].reason == "30");
    }
}
TEST_CASE("Jamendo recorded detail hydrates metadata and real album position", "[providers][jamendo]") {
    Fixture f;
    f.upstream->answer(recorded("track.json")); f.upstream->answer(recorded("album.json")); f.upstream->answer(recorded("artist.json"));
    auto result = unwrap(f.port<ports::ITrackDetail>()->track_detail(context(), jam::parsing::ref("49295")));
    CHECK(result.title == "Track 8 Gjacove"); REQUIRE(result.artists.size() == 1); CHECK(result.artists[0].name == "Lover303");
    REQUIRE(result.album); CHECK(result.album->title == "Modern Fairytales"); CHECK(result.track_number == 8); CHECK(result.track_total == 10);
    CHECK(result.artwork); CHECK(f.upstream->calls.size() == 3);
}
TEST_CASE("Jamendo entity identity missing and availability stay distinct", "[providers][jamendo]") {
    Fixture f; Json rows = Json::array(); auto expected = ErrorCode::not_found;
    SECTION("missing") {}
    SECTION("foreign id") { rows.push_back(track_json(2)); expected = ErrorCode::upstream_failed; }
    SECTION("hidden") { auto t = track_json(); t["status"]["available"] = false; rows.push_back(t); }
    SECTION("duplicate detail") { rows = Json::array({track_json(), track_json()}); expected = ErrorCode::upstream_failed; }
    f.upstream->answer(rows); auto result = f.port<ports::ITrackDetail>()->track_detail(context(), jam::parsing::ref("1"));
    REQUIRE_FALSE(result); CHECK(result.error().code == expected);
}
TEST_CASE("Jamendo playlist paging preserves duplicates and skips missing positions", "[providers][jamendo]") {
    Fixture f; auto port = f.port<ports::PlaylistTracks>(); auto parent = playlist_json();
    f.upstream->answer(Json::array({parent})); f.upstream->answer(Json::array({track_json()}));
    auto first = unwrap(port->list(context(), jam::parsing::ref("30"), {2, {}}));
    REQUIRE(first.page.items.size() == 2); CHECK(first.page.items[0].ref == first.page.items[1].ref);
    CHECK(first.page.page.total == 3); CHECK(first.page.page.listed_total == 3); CHECK(first.page.page.unavailable_count == 0);
    REQUIRE(first.next_cursor); CHECK(f.upstream->calls.back().url.find("id%5B%5D=1") != std::string::npos);
    f.upstream->answer(Json::array({parent})); f.upstream->answer(Json::array());
    auto second = unwrap(port->list(context(), jam::parsing::ref("30"), {2, first.next_cursor}));
    CHECK(second.page.items.empty()); CHECK(second.page.page.unavailable_count == 1); CHECK_FALSE(second.next_cursor);
}
TEST_CASE("Jamendo collection revision changes invalidate cursors", "[providers][jamendo]") {
    Fixture f; auto parent = playlist_json(); auto port = f.port<ports::PlaylistTracks>();
    f.upstream->answer(Json::array({parent})); f.upstream->answer(Json::array({track_json()}));
    auto first = unwrap(port->list(context(), jam::parsing::ref("30"), {1, {}}));
    parent["tracks"][1]["id"] = 3; f.upstream->answer(Json::array({parent}));
    auto second = port->list(context(), jam::parsing::ref("30"), {1, first.next_cursor});
    REQUIRE_FALSE(second); CHECK(second.error().code == ErrorCode::cursor_expired); CHECK(f.upstream->calls.size() == 3);
}
TEST_CASE("Jamendo batch replies reject unknown duplicate and wrong parent ids", "[providers][jamendo]") {
    Fixture f; auto parent = album_json(); Json rows;
    SECTION("foreign") { rows = Json::array({track_json(99)}); }
    SECTION("duplicate") { rows = Json::array({track_json(), track_json()}); }
    SECTION("wrong album") { auto t = track_json(); t["albumId"] = 99; rows = Json::array({t}); }
    f.upstream->answer(Json::array({parent})); f.upstream->answer(rows);
    auto result = f.port<ports::AlbumTracks>()->list(context(), jam::parsing::ref("10"), {2, {}});
    REQUIRE_FALSE(result); CHECK(result.error().code == ErrorCode::upstream_failed);
}
TEST_CASE("Jamendo album list uses upstream track numbers rather than playlist positions", "[providers][jamendo]") {
    Fixture f; f.upstream->answer(Json::array({album_json()})); f.upstream->answer(Json::array({track_json(2), track_json()}));
    auto result = unwrap(f.port<ports::AlbumTracks>()->list(context(), jam::parsing::ref("10"), {2, {}}));
    REQUIRE(result.page.items.size() == 2); CHECK(result.page.items[0].ref.id.value() == "1");
    CHECK(result.page.items[0].track_number == 1); CHECK(result.page.items[1].track_number == 2);
}
TEST_CASE("Jamendo artist albums are complete members and popular tracks stay unadvertised", "[providers][jamendo]") {
    Fixture f; auto parent = recorded("artist.json"); f.upstream->answer(parent);
    auto artist = unwrap(f.port<ports::ArtistDetail>()->detail(context(), jam::parsing::ref("6934")));
    CHECK(artist.album_count == 1); CHECK_FALSE(artist.track_count);
    f.upstream->answer(parent); f.upstream->answer(recorded("album.json"));
    auto result = unwrap(f.port<ports::ArtistAlbums>()->list(context(), jam::parsing::ref("6934"), {2, {}}));
    CHECK(result.page.items.size() == 1); CHECK(result.page.items[0].artists[0].name == "Lover303"); CHECK_FALSE(result.next_cursor);
}
TEST_CASE("Jamendo recommendations use the official featured endpoint", "[providers][jamendo]") {
    Fixture f; f.upstream->answer(Json::array({playlist_json()}));
    auto result = unwrap(f.port<ports::IRecommendedPlaylists>()->recommend_playlists(context(), {2, {}}));
    CHECK(result.page.items[0].name == "推荐 & 精选"); CHECK_FALSE(result.page.page.total);
    CHECK(f.upstream->calls[0].url.find("/api/playlists/featured?") != std::string::npos);
}
TEST_CASE("Jamendo audio uses authorized download MP3 and never stream fallback", "[providers][jamendo]") {
    Fixture f; auto row = track_json(); bool success = false;
    SECTION("download") { success = true; }
    SECTION("denied") { row["isDownloadable"] = 0; }
    SECTION("unknown permission") { row.erase("isDownloadable"); }
    SECTION("stream only") { row.erase("download"); }
    SECTION("no MP3") { row["download"] = {{"ogg", "https://prod-1.storage.jamendo.com/download/track/1/ogg/"}}; }
    SECTION("foreign host") { row["download"]["mp3"] = "https://jamendo.com.attacker.test/download/track/1/mp32/"; }
    SECTION("wrong track") { row["download"]["mp3"] = "https://prod-1.storage.jamendo.com/download/track/2/mp32/"; }
    SECTION("stream disguised as download") { row["download"]["mp3"] = row["stream"]["mp33"]; }
    SECTION("downgrade transport") { row["download"]["mp3"] = "http://prod-1.storage.jamendo.com/download/track/1/mp32/"; }
    f.upstream->answer(Json::array({row})); auto result = f.port<ports::IAssetResolver>()->resolve_audio(context(), jam::parsing::ref("1"), {});
    CHECK(static_cast<bool>(result) == success);
    if (success) { CHECK(result->actual_quality == domain::Quality::standard); CHECK_FALSE(result->bitrate_bps); CHECK(result->completeness == domain::Completeness::full); CHECK_FALSE(result->shareable); }
}
TEST_CASE("Jamendo strict quality refuses unsupported tiers and allow lower is explicit", "[providers][jamendo]") {
    Fixture f; auto port = f.port<ports::IAssetResolver>();
    for (const auto tier : {domain::Quality::high, domain::Quality::lossless, domain::Quality::hires}) {
        auto result = port->resolve_audio(context(), jam::parsing::ref("1"), {tier, domain::QualityPolicy::strict});
        REQUIRE_FALSE(result); CHECK(result.error().code == ErrorCode::quality_unavailable);
    }
    CHECK(f.upstream->calls.empty()); f.upstream->answer(Json::array({track_json()}));
    auto result = unwrap(port->resolve_audio(context(), jam::parsing::ref("1"), {domain::Quality::lossless, domain::QualityPolicy::allow_lower}));
    CHECK(result.actual_quality == domain::Quality::standard);
}
TEST_CASE("Jamendo lyrics preserve plain text and report absence", "[providers][jamendo]") {
    Fixture f; auto row = track_json(); row["lyrics"] = "First line\nSecond line"; f.upstream->answer(Json::array({row}));
    auto result = unwrap(f.port<ports::ILyrics>()->lyrics(context(), jam::parsing::ref("1")));
    CHECK(result.lrc == "First line\nSecond line"); CHECK_FALSE(result.unavailable);
    f.upstream->answer(Json::array({track_json()})); CHECK(unwrap(f.port<ports::ILyrics>()->lyrics(context(), jam::parsing::ref("1"))).unavailable);
}
TEST_CASE("Jamendo links accept official identities and reject ambiguity", "[providers][jamendo]") {
    Fixture f; auto port = f.port<ports::ILinkParser>();
    for (auto url : {"https://www.jamendo.com/track/1/song", "https://jamendo.com/en/album/1", "https://www.jamendo.com/artist/1/", "https://jamen.do/t/1"})
        CHECK(unwrap(port->parse_link(url)).ref.id.value() == "1");
    for (auto url : {"https://jamendo.com.attacker.test/track/1", "https://www.jamendo.com@attacker.test/track/1", "https://www.jamendo.com/track/01",
        "https://www.jamendo.com/track/1?id=2", "https://www.jamendo.com/track/1#2", "https://www.jamendo.com:8443/track/1",
        "https://www.jamendo.com/track/1/other/path", "https://jamen.do/a/1", "https://www.jamendo.com/track/1/%2f"})
        CHECK_FALSE(port->parse_link(url));
    CHECK(f.upstream->calls.empty());
}
TEST_CASE("Jamendo artwork restricts origin and metadata keeps unknown fields empty", "[providers][jamendo]") {
    auto row = track_json(); row["cover"] = {{"big", {{"size300", "https://attacker.test/cover.jpg"}}}};
    CHECK_FALSE(jam::parsing::track(row).artwork);
    row["cover"]["big"]["size300"] = "https://usercontent.jamendo.com?type=album&id=10&width=300";
    CHECK(jam::parsing::track(row).artwork);
    CHECK(jam::parsing::clean_text("A &amp; B &#x4e2d; &#25991;") == "A & B 中 文");
    CHECK(jam::parsing::clean_text("&amp;lt;") == "&lt;");
}

namespace {
Json tag_json() { return {{"id", 23}, {"name", "rock"}, {"category", "genre"}}; }
Json rank_json(int id, int position) { return {{"id", id}, {"position", position}, {"type", "track"}}; }
domain::PlatformRef chart_ref(std::string id = "genre:23") { return track("jamendo", std::move(id)); }
}
TEST_CASE("Jamendo featured search filters names and descriptions with scoped totals", "[providers][jamendo]") {
    Fixture f;
    auto first = playlist_json(); first["name"] = "Rock &amp; Roll";
    auto second = first; second["id"] = 31; second["name"] = "Mix"; second["description"] = "ROCK and 中文";
    auto other = first; other["id"] = 32; other["name"] = "Jazz";
    auto rows = Json::array({first, second, other});
    auto search = f.port<ports::PlaylistSearch>();
    f.upstream->answer(rows);
    auto result = unwrap(search->search(context(), {"rock", 1, {}}));
    REQUIRE(result.page.items.size() == 1); CHECK(result.page.items[0].name == "Rock & Roll");
    CHECK_FALSE(result.page.page.total); CHECK(result.page.page.listed_total == 2); CHECK(result.next_cursor); CHECK(f.upstream->calls.size() == 1);
    CHECK(f.upstream->calls[0].url.find("/api/playlists/featured?limit=100&offset=0") != std::string::npos);
    f.upstream->answer(rows);
    auto next = unwrap(search->search(context(), {"rock", 1, result.next_cursor}));
    REQUIRE(next.page.items.size() == 1); CHECK(next.page.items[0].ref.id.value() == "31"); CHECK_FALSE(next.next_cursor);
    f.upstream->answer(rows); CHECK(unwrap(search->search(context(), {"中文", 5, {}})).page.items.size() == 1);
    f.upstream->answer(rows); auto empty = unwrap(search->search(context(), {"not-present", 5, {}}));
    CHECK(empty.page.items.empty()); CHECK_FALSE(empty.page.page.total); CHECK(empty.page.page.listed_total == 0); CHECK_FALSE(empty.next_cursor);
}
TEST_CASE("Jamendo featured search scans later pages and never accepts repeated pages", "[providers][jamendo]") {
    Fixture f; Json rows = Json::array();
    for (int i = 1; i <= 100; ++i) { auto p = playlist_json(); p["id"] = i; p["name"] = "other"; rows.push_back(p); }
    auto last = playlist_json(); last["id"] = 101; last["name"] = "Wanted";
    bool repeated = false;
    SECTION("match beyond page one") { f.upstream->answer(rows); f.upstream->answer(Json::array({last})); }
    SECTION("ignored offset") { repeated = true; f.upstream->answer(rows); f.upstream->answer(rows); }
    const auto result = f.port<ports::PlaylistSearch>()->search(context(), {"wanted", 2, {}});
    CHECK(f.upstream->calls.size() == 2); CHECK(f.upstream->calls.back().url.ends_with("offset=100"));
    if (repeated) { REQUIRE_FALSE(result); CHECK(result.error().code == ErrorCode::upstream_failed); }
    else { REQUIRE(result); CHECK(result->page.items.front().ref.id.value() == "101"); CHECK(result->page.page.listed_total == 1); CHECK_FALSE(result->page.page.total); }
}
TEST_CASE("Jamendo featured search validates scope revision and rejects partial network results", "[providers][jamendo]") {
    Fixture f; auto a = playlist_json(); auto b = a; b["id"] = 31;
    f.upstream->answer(Json::array({a,b})); auto port = f.port<ports::PlaylistSearch>();
    auto first = unwrap(port->search(context(), {"精选", 1, {}})); REQUIRE(first.next_cursor);
    b["name"] = "改名"; f.upstream->answer(Json::array({a,b}));
    auto changed = port->search(context(), {"精选", 1, first.next_cursor});
    REQUIRE_FALSE(changed); CHECK(changed.error().code == ErrorCode::cursor_expired);
    const auto calls = f.upstream->calls.size();
    CHECK_FALSE(port->search(context(), {"", 1, {}})); CHECK_FALSE(port->search(context(), {"x", 51, {}}));
    CHECK_FALSE(port->search(context(), {"x", 1, "2"})); CHECK(f.upstream->calls.size() == calls);
    f.upstream->answer({}, 503); auto failed = port->search(context(), {"x", 1, {}});
    REQUIRE_FALSE(failed); CHECK(failed.error().code == ErrorCode::upstream_failed);
}
TEST_CASE("Jamendo live categories retain native grouping and list tracks", "[providers][jamendo]") {
    Fixture f; auto mood = tag_json(); mood["id"] = 1777; mood["name"] = "happy"; mood["category"] = "mood";
    f.upstream->answer(Json::array({tag_json(),mood}));
    auto categories = unwrap(f.port<ports::ICategories>()->categories(context()));
    REQUIRE(categories.size() == 2); CHECK(categories[0].id == "23"); CHECK(categories[0].lists == domain::ListedKind::track);
    CHECK(categories[1].group == "mood"); CHECK(f.upstream->calls[0].url.find("lang=en") != std::string::npos);
    auto native = track_json(2); native.erase("type"); // 实测 /api/tracks 不带搜索结果的 type 字段。
    f.upstream->answer(Json::array({tag_json()})); f.upstream->answer(Json::array({native,track_json(1),track_json(3)}));
    const auto page = unwrap(f.port<ports::CategoryTracks>()->list(context(), jam::parsing::ref("23"), {2,{}}));
    REQUIRE(page.page.items.size() == 2); CHECK(page.page.items[0].ref.id.value() == "2"); CHECK(page.next_cursor == "2");
    CHECK_FALSE(page.page.page.total); CHECK(f.upstream->calls.back().url.find("tagId=23&order=hotness&limit=3&offset=0") != std::string::npos);
}
TEST_CASE("Jamendo recorded category tracks omit type and hydrate only the requested window", "[providers][jamendo]") {
    Fixture f; f.upstream->answer(recorded("categories.json"));
    auto directory = unwrap(f.port<ports::ICategories>()->categories(context()));
    CHECK(directory.size() == 26); CHECK(directory.front().lists == domain::ListedKind::track);
    f.upstream->answer(recorded("categories.json")); f.upstream->answer(recorded("category-tracks.json"));
    f.upstream->answer(Json::array()); f.upstream->answer(Json::array());
    auto page = unwrap(f.port<ports::CategoryTracks>()->list(context(), jam::parsing::ref("23"), {1,{}}));
    REQUIRE(page.page.items.size() == 1); CHECK(page.page.items[0].ref.id.value() == "2345165"); CHECK(page.next_cursor == "1");
    CHECK(f.upstream->calls[3].url.ends_with("id%5B%5D=646938"));
    CHECK(f.upstream->calls[4].url.ends_with("id%5B%5D=508884"));
}
TEST_CASE("Jamendo categories reject malformed catalogs and unknown identifiers", "[providers][jamendo]") {
    Fixture f; auto row = tag_json(); Json data = Json::array({row});
    SECTION("duplicate") { data.push_back(row); }
    SECTION("unknown group") { data[0]["category"] = "not-a-category"; }
    SECTION("invalid id") { data[0]["id"] = "not-numeric"; }
    SECTION("empty name") { data[0]["name"] = ""; }
    f.upstream->answer(data); auto result = f.port<ports::ICategories>()->categories(context());
    REQUIRE_FALSE(result); CHECK(result.error().code == ErrorCode::upstream_failed);
    f.upstream->answer(Json::array({row}));
    auto missing = f.port<ports::CategoryTracks>()->list(context(), jam::parsing::ref("999"), {2,{}});
    REQUIRE_FALSE(missing); CHECK(missing.error().code == ErrorCode::not_found);
}
TEST_CASE("Jamendo category paging preserves unavailable positions and empty endpoints", "[providers][jamendo]") {
    Fixture f; auto t = track_json(); t["status"]["available"] = false;
    f.upstream->answer(Json::array({tag_json()})); f.upstream->answer(Json::array({t,track_json(2),track_json(3)}));
    auto port = f.port<ports::CategoryTracks>();
    auto page = unwrap(port->list(context(), jam::parsing::ref("23"), {2,{}}));
    CHECK(page.page.page.unavailable_count == 1); CHECK(page.next_cursor == "2");
    f.upstream->answer(Json::array({tag_json()})); f.upstream->answer(Json::array());
    auto end = unwrap(port->list(context(), jam::parsing::ref("23"), {2,page.next_cursor}));
    CHECK(end.page.items.empty()); CHECK_FALSE(end.next_cursor); CHECK(f.upstream->calls.back().url.ends_with("offset=2"));
}
TEST_CASE("Jamendo chart directory and details use stable keys without invented totals", "[providers][jamendo]") {
    Fixture f; f.upstream->answer(Json::array({tag_json()}));
    const auto result = unwrap(f.port<ports::ICharts>()->charts(context()));
    REQUIRE(result.size() == 3); CHECK(result[0].ref.id.value() == "bestof"); CHECK(result[1].ref.id.value() == "trending");
    CHECK(result[2].ref == chart_ref()); CHECK_FALSE(result[2].track_count); CHECK_FALSE(result[2].updated_at);
    f.upstream->answer(Json::array({tag_json()}));
    CHECK(unwrap(f.port<ports::ChartDetail>()->detail(context(), chart_ref())).name == result[2].name);
    f.upstream->answer(Json::array({tag_json()}));
    CHECK(f.port<ports::ChartDetail>()->detail(context(), chart_ref("genre:999")).error().code == ErrorCode::not_found);
    const auto calls = f.upstream->calls.size();
    CHECK_FALSE(f.port<ports::ChartTracks>()->list(context(), chart_ref("genre:23&x=y"), {2,{}}));
    CHECK(f.upstream->calls.size() == calls);
}
TEST_CASE("Jamendo chart pages preserve ranking and count missing track details", "[providers][jamendo]") {
    Fixture f; const auto rank = Json::array({rank_json(2,1), rank_json(1,2),rank_json(3,3)});
    f.upstream->answer(Json::array({tag_json()})); f.upstream->answer(rank);
    f.upstream->answer(Json::array({track_json(1),track_json(2)}));
    auto port = f.port<ports::ChartTracks>(); auto page = unwrap(port->list(context(), chart_ref(), {2,{}}));
    REQUIRE(page.page.items.size() == 2); CHECK(page.page.items[0].ref.id.value() == "2");
    CHECK(page.page.items[1].ref.id.value() == "1"); CHECK(page.page.page.listed_total == 3); CHECK_FALSE(page.page.page.total);
    CHECK(f.upstream->calls[1].url.find("limit=100&offset=0&tagId=23") != std::string::npos);
    f.upstream->answer(Json::array({tag_json()})); f.upstream->answer(rank); f.upstream->answer(Json::array());
    auto next = unwrap(port->list(context(), chart_ref(), {2,page.next_cursor}));
    CHECK(next.page.items.empty()); CHECK(next.page.page.unavailable_count == 1); CHECK_FALSE(next.next_cursor);
    CHECK(f.upstream->calls.back().url.find("id%5B%5D=3") != std::string::npos);
}
TEST_CASE("Jamendo ranking changes expire cursors and malformed ranks fail closed", "[providers][jamendo]") {
    Fixture f; auto rank = Json::array({rank_json(1,1),rank_json(2,2)});
    f.upstream->answer(Json::array({tag_json()})); f.upstream->answer(rank); f.upstream->answer(Json::array({track_json(1)}));
    auto port = f.port<ports::ChartTracks>(); auto first = unwrap(port->list(context(), chart_ref("trending"), {1,{}}));
    CHECK(f.upstream->calls[1].url.ends_with("order=trending"));
    auto expected = ErrorCode::upstream_failed;
    SECTION("revision") { rank[0]["id"] = 3; expected = ErrorCode::cursor_expired; }
    SECTION("duplicate id") { rank[1]["id"] = 1; }
    SECTION("invalid rank") { rank[1]["position"] = 1; }
    SECTION("wrong type") { rank[1]["type"] = "album"; }
    f.upstream->answer(Json::array({tag_json()})); f.upstream->answer(rank);
    auto next = port->list(context(), chart_ref("trending"), {1,first.next_cursor});
    REQUIRE_FALSE(next); CHECK(next.error().code == expected); CHECK(f.upstream->calls.size() == 5);
}
