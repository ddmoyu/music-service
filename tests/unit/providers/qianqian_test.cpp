#include "../domain/test_support.hpp"
#include "application/transport/http_rules.hpp"
#include "providers/qianqian/bundle.hpp"
#include "providers/qianqian/parsing.hpp"
#include "providers/registry/provider_registry.hpp"
#include <catch2/catch_test_macros.hpp>
#include <deque>
#include <filesystem>
#include <fstream>

using namespace music_service;
using namespace music_service::test;
namespace ports = application::ports;
namespace qqn = providers::qianqian;
using Json = nlohmann::json;
using domain::ErrorCode;
namespace {
class FakeUpstream final : public ports::UpstreamHttp {
public:
    std::deque<ports::HttpResponse> answers;
    std::vector<ports::HttpRequest> calls;
    std::optional<domain::Error> failure;
    bool deferred{false};
    void raw(std::string body, int status = 200) { answers.push_back({{status, {}, {}}, std::move(body)}); }
    void data(Json data) { raw(Json{{"state", true}, {"errno", 22000}, {"data", std::move(data)}}.dump()); }
    domain::Result<ports::HopResult<ports::HttpResponse>> fetch(ports::HttpRequest request,
        const ports::AdmissionRequest& base, const ports::OperationContext&) override {
        REQUIRE(application::transport::check_request(request));
        CHECK(base.source.value() == "qianqian");
        CHECK(base.operation == request.operation);
        CHECK(request.secret_headers.empty());
        calls.push_back(request);
        if (failure) return *failure;
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
ports::ProviderContext context() {
    return {{std::chrono::steady_clock::now() + std::chrono::seconds{30}, {}},
        unwrap(domain::AuthIdentityKey::make(anonymous("prn_a", "qianqian"), {})), {}, {}};
}
struct Fixture {
    std::shared_ptr<FakeUpstream> upstream = std::make_shared<FakeUpstream>();
    std::shared_ptr<const providers::ProviderRegistry> registry = unwrap(providers::ProviderRegistry::build({qqn::make_bundle()}, {}, {upstream}));
    template<class Port> std::shared_ptr<Port> port() { return unwrap(registry->require<Port>("qianqian")); }
};
std::string fixture_body(const char* name) {
    std::ifstream file(std::filesystem::path{MUSIC_SERVICE_TEST_FIXTURES} / "providers" / "qianqian" / name, std::ios::binary);
    REQUIRE(file);
    return {std::istreambuf_iterator<char>(file), {}};
}
Json recorded(const char* name) { return Json::parse(fixture_body(name)).at("data"); }
Json song() {
    return {{"TSID", "T123"}, {"assetId", "T123"}, {"title", "测试歌曲"}, {"duration", 180},
        {"artist", Json::array({{{"name", "词作者"}, {"artistType", 4}, {"artistCode", "A1"}},
                              {{"name", "演唱者"}, {"artistType", 38}, {"artistCode", "A2"}}})},
        {"albumTitle", "测试专辑"}, {"albumAssetCode", "P123"}, {"sort", 3}, {"isVip", 1}, {"isPaid", 0}};
}
Json audio() {
    auto data = song();
    data.update({{"path", "https://audio04.dmhmusic.com/test.mp3?sign=synthetic"}, {"rate", 128}, {"format", "mp3"},
                 {"expireTime", std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() + 3600}});
    return data;
}
Json playlist(int total = 4) {
    return {{"id", 123}, {"title", "歌单"}, {"trackCount", total}, {"addDate", "2026-10-23"},
        {"trackList", Json::array({song(), song()})}, {"haveMore", 1}};
}
domain::ResourceRequirement requirement(domain::Quality quality = domain::Quality::standard,
    domain::QualityPolicy policy = domain::QualityPolicy::strict, bool preview = false) {
    return {quality, policy, preview};
}
}

TEST_CASE("Qianqian signing sorts raw UTF-8 before URL encoding", "[providers][qianqian]") {
    qqn::Params params{{"word", "许嵩 & A+B"}, {"type", "1"}, {"pageNo", "1"}, {"pageSize", "3"}};
    auto query = qqn::signed_query(params, 1700000000);
    CHECK(query == "appid=16073360&pageNo=1&pageSize=3&sign=e09ca4ebce64bf7d891b4d9bfeabf0ef&timestamp=1700000000&type=1&word=%E8%AE%B8%E5%B5%A9%20%26%20A%2BB");
    params["sign"] = "stale"; params["timestamp"] = "old"; params["appid"] = "wrong";
    CHECK(qqn::signed_query(params, 1700000000) == query);
    CHECK(qqn::signed_query(params, 1700000001) != query);
    CHECK_FALSE(qqn::checked([&] { return qqn::signed_query(params, -1); }));
    CHECK_FALSE(qqn::checked([&] { return qqn::signed_query(params, 253402300800LL); }));
    CHECK_FALSE(qqn::signed_query(params, 0).empty());
}

TEST_CASE("Qianqian declares only implemented anonymous capabilities", "[providers][qianqian]") {
    Fixture f;
    CHECK(f.registry->instantiated() == 0);
    auto descriptor = unwrap(f.registry->describe("qianqian"));
    CHECK(descriptor.capabilities.size() == 13);
    CHECK(f.upstream->calls.empty());
    CHECK_FALSE(f.registry->require<ports::PlaylistSearch>("qianqian"));
    CHECK_FALSE(f.registry->require<ports::ICharts>("qianqian"));
    CHECK(unwrap(f.registry->describe_capability("qianqian", "search_tracks")).credentials == ports::CredentialRequirement::none);
}

TEST_CASE("Qianqian recorded search and detail preserve identity and performers", "[providers][qianqian]") {
    Fixture f;
    f.upstream->data(recorded("search-track.json"));
    const auto page = unwrap(f.port<ports::ITrackSearch>()->search_tracks(context(), {"许嵩", 3, {}}));
    REQUIRE(page.page.items.size() == 3);
    CHECK(page.page.items[0].ref.id.value() == "T10065435650");
    CHECK(page.page.items[0].title == "心安之地");
    CHECK(page.next_cursor == "3");
    CHECK(f.upstream->calls.back().url.find("pageNo=1&pageSize=3") != std::string::npos);
    f.upstream->data(Json::array({song()}));
    const auto detail = unwrap(f.port<ports::ITrackDetail>()->track_detail(context(), track("qianqian", "T123")));
    REQUIRE(detail.artists.size() == 1);
    CHECK(detail.artists[0].name == "演唱者");
    CHECK(detail.track_number == 3);
    CHECK_FALSE(detail.track_total);
}

TEST_CASE("Qianqian retains recorded playlist tracks with missing performer metadata", "[providers][qianqian]") {
    Fixture f;
    auto data = recorded("playlist-p2.json");
    REQUIRE_FALSE(data["trackList"].back().contains("artist"));
    bool valid = true;
    SECTION("missing performer field") {}
    SECTION("explicit null performer field") { data["trackList"].back()["artist"] = nullptr; }
    SECTION("wrong field type remains invalid") { data["trackList"].back()["artist"] = "invalid"; valid = false; }
    f.upstream->data(std::move(data));
    auto result = f.port<ports::PlaylistTracks>()->list(context(), track("qianqian", "309735"), {5, {}});
    REQUIRE(result.has_value() == valid);
    if (valid) {
        REQUIRE(result->page.items.size() == 5);
        CHECK(result->page.items.back().ref.id.value() == "T10064316227");
        CHECK(result->page.items.back().artists.empty());
        CHECK_FALSE(result->page.items.back().track_number);
        CHECK(result->next_cursor);
    } else CHECK(result.error().code == ErrorCode::upstream_failed);
}

TEST_CASE("Qianqian searches retain VIP entries and advance short windows", "[providers][qianqian]") {
    Fixture f;
    f.upstream->data({{"typeTrack", Json::array({song()})}, {"total", 10}, {"haveMore", 1}});
    auto result = unwrap(f.port<ports::ITrackSearch>()->search_tracks(context(), {"中文", 3, "3"}));
    REQUIRE(result.page.items.size() == 1);
    CHECK(result.next_cursor == "6");
    CHECK_FALSE(result.page.page.unavailable_count);
    CHECK(f.upstream->calls.back().url.find("pageNo=2") != std::string::npos);
    f.upstream->data(recorded("empty.json"));
    result = unwrap(f.port<ports::ITrackSearch>()->search_tracks(context(), {"no results", 3, {}}));
    CHECK(result.page.items.empty()); CHECK_FALSE(result.next_cursor); CHECK(result.page.page.total == 0);
}

TEST_CASE("Qianqian rejects malformed paging before network access", "[providers][qianqian]") {
    Fixture f;
    for (const auto& cursor : {"abc", "-1", "10001", "1", "3:bad", "999999999999999999999"}) {
        auto result = f.port<ports::ITrackSearch>()->search_tracks(context(), {"test", 3, cursor});
        REQUIRE_FALSE(result); CHECK(result.error().code == ErrorCode::cursor_mismatch);
    }
    for (auto limit : {0U, 51U}) CHECK_FALSE(f.port<ports::ITrackSearch>()->search_tracks(context(), {"test", limit, {}}));
    CHECK_FALSE(f.port<ports::ITrackSearch>()->search_tracks(context(), {"", 3, {}}));
    CHECK(f.upstream->calls.empty());
}

TEST_CASE("Qianqian collection pages preserve positions and detect changes", "[providers][qianqian]") {
    Fixture f;
    auto data = playlist(); data["trackList"][1] = nullptr;
    f.upstream->data(data);
    auto first = unwrap(f.port<ports::PlaylistTracks>()->list(context(), track("qianqian", "123"), {2, {}}));
    REQUIRE(first.next_cursor);
    CHECK(first.page.page.total == 4); CHECK(first.page.page.unavailable_count == 1);
    CHECK_FALSE(first.page.page.listed_total);
    f.upstream->data(playlist());
    auto second = unwrap(f.port<ports::PlaylistTracks>()->list(context(), track("qianqian", "123"), {2, first.next_cursor}));
    REQUIRE(second.page.items.size() == 2);
    CHECK(second.page.items[0].ref == second.page.items[1].ref);
    CHECK(f.upstream->calls.back().url.find("pageNo=2&pageSize=2") != std::string::npos);
    f.upstream->data(playlist(5));
    auto changed = f.port<ports::PlaylistTracks>()->list(context(), track("qianqian", "123"), {2, first.next_cursor});
    REQUIRE_FALSE(changed); CHECK(changed.error().code == ErrorCode::cursor_expired);
}

TEST_CASE("Qianqian recorded album traverses a complete ordered snapshot", "[providers][qianqian]") {
    Fixture f;
    auto album = recorded("album.json");
    f.upstream->data(album);
    const auto ref = track("qianqian", "P10004353346");
    auto first = unwrap(f.port<ports::AlbumTracks>()->list(context(), ref, {5, {}}));
    REQUIRE(first.page.items.size() == 5); REQUIRE(first.next_cursor);
    CHECK(first.page.page.total == 9); CHECK(first.page.page.listed_total == 9);
    CHECK(first.page.items[0].track_number == 1); CHECK(first.page.items[0].track_total == 9);
    CHECK(first.page.items[0].album->ref == ref); CHECK(first.page.items[0].artwork);
    f.upstream->data(album);
    auto last = unwrap(f.port<ports::AlbumTracks>()->list(context(), ref, {5, first.next_cursor}));
    CHECK(last.page.items.size() == 4); CHECK_FALSE(last.next_cursor);
    std::swap(album["trackList"][0], album["trackList"][1]);
    f.upstream->data(album);
    auto changed = f.port<ports::AlbumTracks>()->list(context(), ref, {5, first.next_cursor});
    REQUIRE_FALSE(changed); CHECK(changed.error().code == ErrorCode::cursor_expired);
}

TEST_CASE("Qianqian playlist positions do not become album track numbers", "[providers][qianqian][qianqian-review]") {
    Fixture f;
    auto data = recorded("playlist-p1.json");
    REQUIRE(data["trackList"][1]["sort"] == 2);
    f.upstream->data(data);
    const auto result = unwrap(f.port<ports::PlaylistTracks>()->list(context(), track("qianqian", "309735"), {3, {}}));
    REQUIRE(result.page.items.size() == 3);
    for (std::size_t i = 0; i < result.page.items.size(); ++i) {
        CHECK(result.page.items[i].ref.id.value() == data["trackList"][i]["TSID"].get<std::string>());
        CHECK_FALSE(result.page.items[i].track_number);
    }
    // 同一曲目的 song/info 返回专辑内的 sort=1，而非歌单位置 2。
    auto detail = data["trackList"][1]; detail["sort"] = 1;
    f.upstream->data(Json::array({detail}));
    CHECK(unwrap(f.port<ports::ITrackDetail>()->track_detail(context(), track("qianqian", "T10064900866"))).track_number == 1);
}

TEST_CASE("Qianqian discovery uses official live categories and homepage playlists", "[providers][qianqian]") {
    Fixture f;
    f.upstream->data(recorded("categories.json"));
    auto cats = unwrap(f.port<ports::ICategories>()->categories(context()));
    CHECK(cats.size() > 20);
    CHECK(cats.front().id == "2368"); CHECK(cats.front().group == "语种");
    f.upstream->data(recorded("playlists.json"));
    auto recommendations = unwrap(f.port<ports::IRecommendedPlaylists>()->recommend_playlists(context(), {3, {}}));
    CHECK(recommendations.page.items.size() == 3); CHECK(recommendations.next_cursor);
    f.upstream->data(recorded("categories.json")); f.upstream->data(recorded("category.json"));
    auto selected = unwrap(f.port<ports::CategoryPlaylists>()->list(context(), track("qianqian", "2376"), {3, {}}));
    CHECK(selected.page.items.size() == 3); CHECK(f.upstream->calls.back().url.find("subCateId=2376") != std::string::npos);
    f.upstream->data(recorded("categories.json"));
    auto missing = f.port<ports::CategoryPlaylists>()->list(context(), track("qianqian", "9999"), {3, {}});
    REQUIRE_FALSE(missing); CHECK(missing.error().code == ErrorCode::not_found);
}

TEST_CASE("Qianqian album search and collection detail preserve entity kinds", "[providers][qianqian]") {
    Fixture f;
    f.upstream->data(recorded("search-album.json"));
    CHECK(unwrap(f.port<ports::AlbumSearch>()->search(context(), {"许嵩", 3, {}})).page.items.size() == 3);
    CHECK(f.upstream->calls.back().url.find("type=3") != std::string::npos);
    f.upstream->data(recorded("album.json"));
    CHECK(unwrap(f.port<ports::AlbumDetail>()->detail(context(), track("qianqian", "P10004353346"))).track_count == 9);
    f.upstream->data(recorded("playlist-p1.json"));
    CHECK(unwrap(f.port<ports::PlaylistDetail>()->detail(context(), track("qianqian", "309735"))).track_count == 24);
    CHECK_FALSE(f.port<ports::AlbumDetail>()->detail(context(), track("qianqian", "123")));
}

TEST_CASE("Qianqian audio derives qualities from returned format and rate", "[providers][qianqian]") {
    Fixture f;
    auto data = audio();
    SECTION("standard") {
        f.upstream->data(data);
        auto out = unwrap(f.port<ports::IAssetResolver>()->resolve_audio(context(), track("qianqian", "T123"), requirement()));
        CHECK(out.actual_quality == domain::Quality::standard); CHECK(out.completeness == domain::Completeness::full);
        CHECK(out.duration_ms == 180000); CHECK(out.expires_at); CHECK_FALSE(out.shareable);
    }
    SECTION("high") {
        data["rate"] = 320; f.upstream->data(data);
        CHECK(unwrap(f.port<ports::IAssetResolver>()->resolve_audio(context(), track("qianqian", "T123"), requirement(domain::Quality::high))).actual_quality == domain::Quality::high);
    }
    SECTION("lossless bitrate is not the request code 3000") {
        data["rate"] = 1788; data["format"] = "flac"; f.upstream->data(data);
        CHECK(unwrap(f.port<ports::IAssetResolver>()->resolve_audio(context(), track("qianqian", "T123"), requirement(domain::Quality::lossless))).actual_quality == domain::Quality::lossless);
    }
    SECTION("strict rejects a lower resource") {
        f.upstream->data(data);
        auto out = f.port<ports::IAssetResolver>()->resolve_audio(context(), track("qianqian", "T123"), requirement(domain::Quality::high));
        REQUIRE_FALSE(out); CHECK(out.error().code == ErrorCode::quality_unavailable);
    }
    SECTION("explicit fallback reports actual lower quality") {
        f.upstream->data(data); f.upstream->data(data);
        CHECK(unwrap(f.port<ports::IAssetResolver>()->resolve_audio(context(), track("qianqian", "T123"), requirement(domain::Quality::high, domain::QualityPolicy::allow_lower))).actual_quality == domain::Quality::standard);
    }
    SECTION("fallback does not skip an available intermediate tier") {
        f.upstream->data(data); data["rate"] = 320; f.upstream->data(data);
        auto result = unwrap(f.port<ports::IAssetResolver>()->resolve_audio(context(), track("qianqian", "T123"), requirement(domain::Quality::lossless, domain::QualityPolicy::allow_lower)));
        CHECK(result.actual_quality == domain::Quality::high); CHECK(f.upstream->calls.size() == 2);
    }
    SECTION("Hi-Res is not declared") {
        CHECK_FALSE(f.port<ports::IAssetResolver>()->resolve_audio(context(), track("qianqian", "T123"), requirement(domain::Quality::hires)));
        CHECK(f.upstream->calls.empty());
    }
}

TEST_CASE("Qianqian VIP and preview results cannot become full downloads", "[providers][qianqian]") {
    Fixture f;
    auto data = audio(); data.erase("path");
    SECTION("VIP without a file reports access required") {
        f.upstream->data(data);
        auto out = f.port<ports::IAssetResolver>()->resolve_audio(context(), track("qianqian", "T123"), requirement());
        REQUIRE_FALSE(out); CHECK(out.error().code == ErrorCode::credentials_required);
    }
    SECTION("preview disabled") {
        data["trail_audio_info"] = audio(); f.upstream->data(data);
        auto out = f.port<ports::IAssetResolver>()->resolve_audio(context(), track("qianqian", "T123"), requirement());
        REQUIRE_FALSE(out); CHECK(out.error().code == ErrorCode::preview_rejected);
    }
    SECTION("preview enabled keeps its own duration") {
        auto preview = audio(); preview["duration"] = 30; data["trail_audio_info"] = preview; f.upstream->data(data);
        auto out = unwrap(f.port<ports::IAssetResolver>()->resolve_audio(context(), track("qianqian", "T123"), requirement(domain::Quality::standard, domain::QualityPolicy::strict, true)));
        CHECK(out.completeness == domain::Completeness::preview); CHECK(out.duration_ms == 30000);
    }
}

TEST_CASE("Qianqian rejects expired foreign and mismatched resources", "[providers][qianqian]") {
    Fixture f;
    auto data = audio();
    ErrorCode expected = ErrorCode::upstream_failed;
    SECTION("expiry") { data["expireTime"] = 1; expected = ErrorCode::resource_expired; }
    SECTION("foreign origin") { data["path"] = "https://audio.dmhmusic.com.evil.test/file.mp3"; }
    SECTION("plaintext") { data["path"] = "http://audio04.dmhmusic.com/file.mp3"; }
    SECTION("wrong identity") { data["TSID"] = "T456"; data["assetId"] = "T456"; }
    SECTION("conflicting identities") { data["assetId"] = "T456"; }
    SECTION("wrong format") { data["format"] = "mflac"; }
    f.upstream->data(data);
    auto out = f.port<ports::IAssetResolver>()->resolve_audio(context(), track("qianqian", "T123"), requirement());
    REQUIRE_FALSE(out); CHECK(out.error().code == expected);
}

TEST_CASE("Qianqian preview expiry belongs to the selected preview resource", "[providers][qianqian][qianqian-review]") {
    Fixture f;
    auto data = audio(); data.erase("path");
    auto preview = audio(); preview["duration"] = 30;
    bool expired = false;
    SECTION("expired preview must not inherit a future full-file expiry") {
        preview["expireTime"] = 1; expired = true;
    }
    SECTION("valid preview must not inherit an expired full-file timestamp") {
        data["expireTime"] = 1;
    }
    SECTION("preview with no separate expiry retains the envelope limit") {
        preview.erase("expireTime"); data["expireTime"] = 1; expired = true;
    }
    data["trail_audio_info"] = preview;
    f.upstream->data(data);
    const auto result = f.port<ports::IAssetResolver>()->resolve_audio(context(), track("qianqian", "T123"),
        requirement(domain::Quality::standard, domain::QualityPolicy::strict, true));
    REQUIRE(result.has_value() != expired);
    if (expired) CHECK(result.error().code == ErrorCode::resource_expired);
    else {
        CHECK(result->completeness == domain::Completeness::preview);
        REQUIRE(result->expires_at);
        CHECK(std::chrono::duration_cast<std::chrono::seconds>(result->expires_at->time_since_epoch()).count() == preview["expireTime"].get<std::int64_t>());
    }
}

TEST_CASE("Qianqian accepts plain lyrics and rejects document error bodies", "[providers][qianqian][qianqian-review]") {
    Fixture f;
    auto data = song();
    data["lyric"] = "https://static-qianqian.taihe.com/song.txt";
    std::string body;
    bool valid = false, unavailable = false;
    SECTION("official TXT lyrics need no timestamp") { body = "示例曲\n作词：示例作者\n这是一行没有时间戳的歌词\n"; valid = true; }
    SECTION("empty whitespace has no lyrics") { body = " \r\n\t"; valid = true; unavailable = true; }
    SECTION("uppercase HTML error is not lyrics") { body = "<HTML><BODY>[upstream error]</BODY></HTML>"; }
    SECTION("XML error is not lyrics") { body = "<?xml version=\"1.0\"?><Error>[AccessDenied]</Error>"; }
    SECTION("JSON error is not lyrics") { body = "{\"message\":\"error\",\"details\":[1]}"; }
    f.upstream->data(Json::array({data})); f.upstream->raw(body);
    auto result = f.port<ports::ILyrics>()->lyrics(context(), track("qianqian", "T123"));
    REQUIRE(result.has_value() == valid);
    if (valid) {
        CHECK(result->unavailable == unavailable);
        if (!unavailable) CHECK(result->lrc == body);
    } else CHECK(result.error().code == ErrorCode::upstream_failed);
}

TEST_CASE("Qianqian lyrics are fetched through admission and missing stays unavailable", "[providers][qianqian]") {
    Fixture f;
    auto data = song();
    SECTION("real LRC response") {
        data["lyric"] = "https://static-qianqian.taihe.com/song.lrc";
        f.upstream->data(Json::array({data})); f.upstream->raw("[00:01.00]中文歌词");
        auto result = unwrap(f.port<ports::ILyrics>()->lyrics(context(), track("qianqian", "T123")));
        CHECK(result.lrc == "[00:01.00]中文歌词"); CHECK_FALSE(result.unavailable); CHECK(f.upstream->calls.size() == 2);
    }
    SECTION("missing URL") {
        f.upstream->data(Json::array({data}));
        CHECK(unwrap(f.port<ports::ILyrics>()->lyrics(context(), track("qianqian", "T123"))).unavailable);
    }
    SECTION("HTML error response") {
        data["lyric"] = "https://static-qianqian.taihe.com/song.lrc";
        f.upstream->data(Json::array({data})); f.upstream->raw("<html>[error]</html>");
        CHECK_FALSE(f.port<ports::ILyrics>()->lyrics(context(), track("qianqian", "T123")));
    }
    SECTION("foreign URL") {
        data["lyric"] = "https://evil.test/song.lrc";
        f.upstream->data(Json::array({data}));
        CHECK_FALSE(f.port<ports::ILyrics>()->lyrics(context(), track("qianqian", "T123")));
        CHECK(f.upstream->calls.size() == 1);
    }
}

TEST_CASE("Qianqian links enforce exact hosts and stable entity prefixes", "[providers][qianqian]") {
    Fixture f; auto parser = f.port<ports::ILinkParser>();
    CHECK(unwrap(parser->parse_link("https://music.91q.com/song/T123")).kind == "track");
    CHECK(unwrap(parser->parse_link("https://music.91q.com/album/P123/")).kind == "album");
    CHECK(unwrap(parser->parse_link("https://music.91q.com/songlist/123")).kind == "playlist");
    for (const auto& link : {"https://music.91q.com.evil.test/song/T123", "https://evil.test@music.91q.com/song/T123",
        "https://music.91q.com:444/song/T123", "https://music.91q.com/song/P123", "https://music.91q.com/song/T123?TSID=T456",
        "https://music.91q.com/song/T123#T456", "https://music.91q.com/song/T0", "https://music.91q.com/song/T123/extra"})
        CHECK_FALSE(parser->parse_link(link));
    CHECK(f.upstream->calls.empty());
}

TEST_CASE("Qianqian upstream failures never masquerade as empty success", "[providers][qianqian]") {
    Fixture f;
    ErrorCode expected = ErrorCode::upstream_failed;
    SECTION("invalid signature") { f.upstream->raw(R"({"errno":22001,"state":false,"errmsg":"secret response","data":[]})"); }
    SECTION("unsupported operation is not invalid credentials") { f.upstream->raw(R"({"errno":23001,"state":false,"data":[]})"); }
    SECTION("missing track") { f.upstream->raw(R"({"errno":23010,"state":false,"data":[]})"); expected = ErrorCode::not_found; }
    SECTION("missing envelope fields") { f.upstream->raw("{}"); }
    SECTION("invalid JSON") { f.upstream->raw("<html>"); }
    SECTION("oversized JSON") { f.upstream->raw(std::string(qqn::max_body + 1, ' ')); }
    SECTION("excess nesting") { f.upstream->raw(std::string(40, '[') + "0" + std::string(40, ']')); }
    SECTION("rate limit") { f.upstream->raw("secret response", 429); expected = ErrorCode::upstream_rate_limited; }
    SECTION("upstream timeout") { f.upstream->failure = domain::Error{ErrorCode::upstream_timeout, "timeout", true, {}}; expected = ErrorCode::upstream_timeout; }
    SECTION("admission deferred") { f.upstream->deferred = true; expected = ErrorCode::service_busy; }
    SECTION("empty page with more") { f.upstream->data({{"typeTrack", Json::array()}, {"total", 2}, {"haveMore", 1}}); }
    auto result = f.port<ports::ITrackSearch>()->search_tracks(context(), {"test", 3, {}});
    REQUIRE_FALSE(result); CHECK(result.error().code == expected);
    CHECK(result.error().message.find("secret response") == std::string::npos);
}

TEST_CASE("Qianqian canceled and expired contexts perform no network call", "[providers][qianqian]") {
    Fixture f; auto c = context();
    SECTION("canceled") {
        std::stop_source stop; stop.request_stop(); c.operation.stop = stop.get_token();
        auto out = f.port<ports::ITrackSearch>()->search_tracks(c, {"test", 3, {}});
        REQUIRE_FALSE(out); CHECK(out.error().code == ErrorCode::canceled);
    }
    SECTION("deadline") {
        c.operation.deadline = std::chrono::steady_clock::now() - std::chrono::seconds{1};
        auto out = f.port<ports::ITrackSearch>()->search_tracks(c, {"test", 3, {}});
        REQUIRE_FALSE(out); CHECK(out.error().code == ErrorCode::deadline_exceeded);
    }
    CHECK(f.upstream->calls.empty());
}

TEST_CASE("Qianqian gzip is decoded with bounded output and integrity checks", "[providers][qianqian]") {
    Fixture f;
    auto bytes = fixture_body("search-track.json.gz");
    bool success = false;
    std::string encoding = "gzip";
    SECTION("valid official-shaped gzip") { success = true; encoding = " GZip "; }
    SECTION("invalid CRC") { bytes[bytes.size() - 5] ^= 1; }
    SECTION("truncated stream") { bytes.resize(bytes.size() - 4); }
    SECTION("trailing data") { bytes += "extra"; }
    SECTION("decoded size exceeds limit") { bytes = fixture_body("oversized.json.gz"); }
    SECTION("unsupported content encoding") { encoding = "br"; }
    f.upstream->raw(std::move(bytes));
    f.upstream->answers.back().head.headers.push_back({"Content-Encoding", encoding});
    auto result = f.port<ports::ITrackSearch>()->search_tracks(context(), {"许嵩", 3, {}});
    REQUIRE(result.has_value() == success);
    if (success) CHECK(result->page.items.front().title == "心安之地");
    else CHECK(result.error().code == ErrorCode::upstream_failed);
    CHECK(f.upstream->calls.front().preserve_content_encoding);
}

TEST_CASE("Qianqian gzip processing obeys cancellation and deadlines", "[providers][qianqian]") {
    auto c = context();
    std::stop_source stop;
    ErrorCode expected = ErrorCode::canceled;
    SECTION("canceled") { stop.request_stop(); c.operation.stop = stop.get_token(); }
    SECTION("expired") { c.operation.deadline = std::chrono::steady_clock::now() - std::chrono::seconds{1}; expected = ErrorCode::deadline_exceeded; }
    auto result = qqn::checked([&] { return qqn::decode_body(fixture_body("search-track.json.gz"), "gzip", qqn::max_body, c.operation); });
    REQUIRE_FALSE(result); CHECK(result.error().code == expected);
}
