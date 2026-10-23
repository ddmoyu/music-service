// Kuwo collection capabilities (T23) against desensitized responses recorded on
// 2026-10-22 (tests/fixtures/providers/kuwo) and a fake upstream; no network. The opt-in
// [online-kuwo] case probes the live service.

#include "../domain/test_support.hpp"

#include "application/transport/upstream_client.hpp"
#include "infrastructure/clock.hpp"
#include "infrastructure/config/config_loader.hpp"
#include "infrastructure/transport/httplib_transport.hpp"
#include "providers/kuwo/bundle.hpp"
#include "providers/kuwo/parsing/collections.hpp"
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
    void respond(int status, std::string body) {
        answers_.push_back(ports::HttpResponse{{status, {}, {}}, std::move(body)});
    }

    domain::Result<ports::HopResult<ports::HttpResponse>> fetch(ports::HttpRequest request,
                                                                const ports::AdmissionRequest&,
                                                                const ports::OperationContext&) override {
        requests.push_back(request);
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

    std::vector<ports::HttpRequest> requests;

private:
    std::deque<ports::HttpResponse> answers_;
};

ports::ProviderContext context() {
    return {{std::chrono::steady_clock::now() + std::chrono::seconds(10), {}},
            unwrap(domain::AuthIdentityKey::make(anonymous("prn_a", "kuwo"), {})),
            std::nullopt};
}

// The bundle's capability objects behind one fake upstream.
struct Kuwo {
    Kuwo() : Kuwo(std::make_shared<FakeUpstream>()) {}
    explicit Kuwo(std::shared_ptr<ports::UpstreamHttp> transport) : upstream(std::move(transport)) {
        std::vector<ports::ProviderBundle> bundles;
        bundles.push_back(providers::kuwo::make_bundle());
        providers::ProviderServices services;
        services.upstream = upstream;
        registry = unwrap(providers::ProviderRegistry::build(std::move(bundles), {}, services));
    }
    FakeUpstream& fake() { return static_cast<FakeUpstream&>(*upstream); }
    template <class Port> std::shared_ptr<Port> get() { return unwrap(registry->require<Port>("kuwo")); }

    std::shared_ptr<ports::UpstreamHttp> upstream;
    std::shared_ptr<const providers::ProviderRegistry> registry;
};

domain::PlatformRef ref(const std::string& id) { return test::track("kuwo", id); }

} // namespace

TEST_CASE("kuwo legacy answers become JSON", "[providers][kuwo]") {
    CHECK(unwrap(parsing::legacy_json(R"({'a':'x','b':['1','2']})")) == R"({"a":"x","b":["1","2"]})");
    // Double quotes inside are escaped, \' unescaped, other escapes kept.
    CHECK(unwrap(parsing::legacy_json(R"({'a':'say "hi"','b':'it\'s','c':'&\n'})")) ==
          R"({"a":"say \"hi\"","b":"it's","c":"&\n"})");
    CHECK(unwrap(parsing::legacy_json(R"({"a":"plain"})")) == R"({"a":"plain"})");
    CHECK(detail_reason(parsing::legacy_json("{'a':'open").error(), "protocol") ==
          "legacy response has an unterminated string");
}

TEST_CASE("kuwo playlist and album search answers", "[providers][kuwo]") {
    const auto playlists = unwrap(parsing::parse_playlist_search(fixture("search-playlist-zhoujielun-pn0-rn2.txt"), 0));
    CHECK(playlists.total == 143);
    REQUIRE(playlists.items.size() == 2);
    const auto& first = playlists.items[0];
    CHECK(first.ref == ref("2867496601"));
    CHECK(first.name == "终于等到周杰伦，说好不哭你今天哭了吗？");
    CHECK(first.creator == "第一天");
    CHECK(first.track_count == 177);
    CHECK(first.play_count == 5662351);
    CHECK(first.artwork->rfind("https://img1.kuwo.cn/", 0) == 0);
    CHECK(first.description->find("&nbsp;") == std::string::npos); // entities decoded
    CHECK(detail_reason(parsing::parse_playlist_search(fixture("search-playlist-zhoujielun-pn0-rn2.txt"), 1).error(),
                        "protocol") == "search response is for another page");

    const auto albums = unwrap(parsing::parse_album_search(fixture("search-album-yehuimei-pn0-rn2.txt"), 0));
    CHECK(albums.total == 17);
    REQUIRE(albums.items.size() == 2);
    const auto& album = albums.items[0];
    CHECK(album.ref == ref("1293"));
    CHECK(album.title == "叶惠美");
    REQUIRE(album.artists.size() == 1);
    CHECK(album.artists[0].name == "周杰伦");
    CHECK(album.artists[0].ref->id.value() == "336");
    CHECK(album.release_date == "2003-07-31");
    CHECK(album.track_count == 11);
    CHECK(album.artwork->rfind("https://img4.kuwo.cn/", 0) == 0);
    CHECK(albums.items[1].title == "叶惠美"); // same title, another album and artist
    CHECK(albums.items[1].ref == ref("46381936"));
}

TEST_CASE("kuwo playlist, album and chart pages", "[providers][kuwo]") {
    const auto playlist = unwrap(parsing::parse_playlist_page(fixture("playlist-2234580349-pn0-rn2.json"), "2234580349"));
    CHECK(playlist.playlist.name == "华语，那些年l值得我们回忆的，");
    CHECK(playlist.playlist.creator == "1477727241566weixin");
    CHECK(playlist.playlist.track_count == 17);
    CHECK(playlist.playlist.play_count == 515305);
    REQUIRE(playlist.tracks.size() == 2);
    CHECK(playlist.tracks[0].ref == ref("6519471"));
    CHECK(playlist.tracks[0].title == "温柔乡");
    CHECK(playlist.tracks[0].artists[0].name == "陈雅森");
    CHECK(playlist.tracks[0].album->ref == ref("507999"));
    CHECK(playlist.tracks[0].duration_ms == 253000);
    // Kuwo answers an unknown pid with another, empty playlist.
    CHECK(parsing::parse_playlist_page(fixture("playlist-missing.json"), "999999999999").error().code ==
          ErrorCode::not_found);

    const auto album = unwrap(parsing::parse_album_page(fixture("album-1293-pn0-rn2.txt"), "1293"));
    CHECK(album.album.title == "叶惠美");
    CHECK(album.album.track_count == 11);
    CHECK(album.album.release_date == "2003-07-31");
    REQUIRE(album.tracks.size() == 2);
    CHECK(album.tracks[0].ref == ref("238210"));
    CHECK(album.tracks[0].title == "以父之名");
    CHECK(album.tracks[0].album->ref == ref("1293")); // albumId, a number here
    CHECK(album.tracks[0].duration_ms == 342000);
    CHECK(parsing::parse_album_page(fixture("album-missing.txt"), "999999999").error().code == ErrorCode::not_found);
    CHECK(detail_reason(parsing::parse_album_page(fixture("album-1293-pn0-rn2.txt"), "1294").error(), "protocol") ==
          "album response is about another album");

    const auto chart = unwrap(parsing::parse_chart_page(fixture("chart-16-pn0-rn2.json"), "16"));
    CHECK(chart.chart.ref == ref("16"));
    CHECK(chart.chart.name == "酷我热歌榜");
    CHECK(chart.chart.updated_at == "2026-10-21");
    CHECK(chart.chart.track_count == 300);
    CHECK(chart.chart.lists == domain::ListedKind::track);
    REQUIRE(chart.tracks.size() == 2);
    CHECK(chart.tracks[0].ref == ref("624683929"));
    CHECK(chart.tracks[0].duration_ms == 209000); // song_duration, not the clip's duration
    CHECK(parsing::parse_chart_page(fixture("chart-missing.json"), "999999").error().code == ErrorCode::not_found);
}

TEST_CASE("kuwo categories, category and recommended playlists, charts", "[providers][kuwo]") {
    const auto categories = unwrap(parsing::parse_categories(fixture("tags.json")));
    CHECK(categories.size() == 67); // playlist tags only, not the 18 zones
    CHECK(categories[0].id == "2189");
    CHECK(categories[0].name == "短视频");
    CHECK(categories[0].group == "主题");
    CHECK(categories[0].lists == domain::ListedKind::playlist);

    const auto listed = unwrap(parsing::parse_playlist_list(fixture("tag-playlists-1848-pn1-rn2.json")));
    CHECK(listed.total == 1688);
    REQUIRE(listed.items.size() == 2);
    CHECK(listed.items[0].ref == ref("3730082635"));
    CHECK(listed.items[0].creator == "临摹");
    CHECK(listed.items[0].track_count == 58);
    CHECK(listed.items[0].play_count == 69);
    CHECK(unwrap(parsing::parse_playlist_list(fixture("recommend-pn1-rn2.json"))).total == 1753);
    const auto refused = parsing::parse_playlist_list(R"({"code":-1,"msg":"param error"})");
    CHECK(detail_reason(refused.error(), "kuwo_code") == "-1");

    const auto charts = unwrap(parsing::parse_charts(fixture("bang-list.json")));
    CHECK(charts.size() == 36);
    CHECK(charts[0].ref == ref("16"));
    CHECK(charts[0].name == "酷我热歌榜");
    CHECK(charts[0].updated_at == "2026-10-21");
    CHECK(charts[0].description.has_value());
    CHECK(charts[1].name == "酷我飙升榜");
}

TEST_CASE("kuwo collection capabilities page one upstream page per call", "[providers][kuwo]") {
    Kuwo kuwo;
    auto& fake = kuwo.fake();

    auto search = kuwo.get<ports::PlaylistSearch>();
    fake.respond(200, fixture("search-playlist-zhoujielun-pn0-rn2.txt"));
    const auto found = unwrap(search->search(context(), {"周杰伦", 2, std::nullopt}));
    CHECK(fake.requests.back().url.rfind("https://search.kuwo.cn/r.s?all=%E5%91%A8%E6%9D%B0%E4%BC%A6&ft=playlist&", 0) ==
          0);
    CHECK(fake.requests.back().url.find("&pn=0&rn=2&") != std::string::npos);
    CHECK(fake.requests.back().operation == "search_playlists");
    CHECK(found.page.items.size() == 2);
    CHECK(found.page.page.total == 143);
    CHECK(found.next_cursor == "1");
    fake.respond(200, fixture("search-album-yehuimei-pn0-rn2.txt"));
    CHECK(unwrap(kuwo.get<ports::AlbumSearch>()->search(context(), {"叶惠美", 2, std::nullopt})).page.items.size() == 2);
    CHECK(fake.requests.back().url.find("&ft=album&") != std::string::npos);

    // Playlist detail asks for one song; tracks page by limit with a 0-based cursor.
    fake.respond(200, fixture("playlist-2234580349-pn0-rn2.json"));
    CHECK(unwrap(kuwo.get<ports::PlaylistDetail>()->detail(context(), ref("2234580349"))).track_count == 17);
    CHECK(fake.requests.back().url.find("pid=2234580349&pn=0&rn=1&") != std::string::npos);
    auto tracks = kuwo.get<ports::PlaylistTracks>();
    fake.respond(200, fixture("playlist-2234580349-pn0-rn2.json"));
    const auto first = unwrap(tracks->list(context(), ref("2234580349"), {2, std::nullopt}));
    CHECK(first.page.items.size() == 2);
    CHECK(first.page.page.total == 17);
    CHECK(first.next_cursor == "1");
    fake.respond(200, fixture("playlist-2234580349-pn0-rn2.json"));
    const auto last = unwrap(tracks->list(context(), ref("2234580349"), {2, std::string{"8"}}));
    CHECK(fake.requests.back().url.find("&pn=8&rn=2&") != std::string::npos);
    CHECK_FALSE(last.next_cursor); // 9 * 2 >= 17
    CHECK(last.page.page.has_more == false);
    fake.respond(200, fixture("playlist-missing.json"));
    CHECK(tracks->list(context(), ref("999999999999"), {2, std::nullopt}).error().code == ErrorCode::not_found);

    fake.respond(200, fixture("album-1293-pn0-rn2.txt"));
    CHECK(unwrap(kuwo.get<ports::AlbumTracks>()->list(context(), ref("1293"), {2, std::nullopt})).next_cursor == "1");
    CHECK(fake.requests.back().url.rfind("https://search.kuwo.cn/r.s?stype=albuminfo&albumid=1293&pn=0&rn=2&", 0) == 0);
    fake.respond(200, fixture("chart-16-pn0-rn2.json"));
    const auto chart = unwrap(kuwo.get<ports::ChartTracks>()->list(context(), ref("16"), {2, std::nullopt}));
    CHECK(chart.page.page.total == 300);
    CHECK(fake.requests.back().url.find("&type=bang&data=content&id=16&") != std::string::npos);
    CHECK(fake.requests.back().operation == "chart_tracks");

    // wapi pages are 1-based behind the same 0-based cursor.
    auto category = kuwo.get<ports::CategoryPlaylists>();
    fake.respond(200, fixture("tag-playlists-1848-pn1-rn2.json"));
    CHECK(unwrap(category->list(context(), ref("1848"), {2, std::string{"1"}})).next_cursor == "2");
    CHECK(fake.requests.back().url.find("&pn=2&rn=2&id=1848") != std::string::npos);
    fake.respond(200, fixture("recommend-pn1-rn2.json"));
    CHECK(unwrap(kuwo.get<ports::IRecommendedPlaylists>()->recommend_playlists(context(), {2, std::nullopt}))
              .page.items.size() == 2);
    CHECK(fake.requests.back().url.find("&pn=1&rn=2&order=hot") != std::string::npos);
    fake.respond(200, fixture("tags.json"));
    CHECK(unwrap(kuwo.get<ports::ICategories>()->categories(context())).size() == 67);
    fake.respond(200, fixture("bang-list.json"));
    CHECK(unwrap(kuwo.get<ports::ICharts>()->charts(context())).size() == 36);

    // Invalid input never reaches the network.
    const auto before = fake.requests.size();
    CHECK(category->list(context(), ref("abc"), {2, std::nullopt}).error().code == ErrorCode::validation_failed);
    CHECK(tracks->list(context(), test::track("fivesing", "1"), {2, std::nullopt}).error().code ==
          ErrorCode::validation_failed);
    CHECK(tracks->list(context(), ref("1"), {2, std::string{"x"}}).error().code == ErrorCode::cursor_mismatch);
    CHECK(tracks->list(context(), ref("1"), {101, std::nullopt}).error().code == ErrorCode::validation_failed);
    CHECK(search->search(context(), {"a", 51, std::nullopt}).error().code == ErrorCode::validation_failed);
    CHECK(fake.requests.size() == before);
    for (const auto& request : fake.requests) {
        CHECK(request.url.rfind("https://", 0) == 0);
        CHECK(request.secret_headers.empty());
    }

    fake.respond(503, "busy");
    const auto failed = kuwo.get<ports::ICharts>()->charts(context());
    CHECK(failed.error().retryable);
    CHECK(detail_reason(failed.error(), "http_status") == "503");
}

TEST_CASE("kuwo declares the collection capabilities it implements", "[providers][kuwo]") {
    Kuwo kuwo;
    const auto view = unwrap(kuwo.registry->describe("kuwo"));
    for (const auto* key : {"search_playlists", "search_albums", "playlist_detail", "playlist_tracks", "album_detail",
                            "album_tracks", "chart_detail", "chart_tracks", "categories", "category_playlists",
                            "charts", "recommend_playlists", "lyrics"}) {
        INFO(key);
        CHECK(view.supports(key));
    }
    for (const auto* key : {"search_artists", "artist_detail", "artist_tracks", "artist_albums"}) {
        INFO(key);
        CHECK(view.supports(key));
    }
}

// Live probe, opt-in: MUSIC_SERVICE_KUWO_ONLINE=1 (see kuwo_test.cpp).
TEST_CASE("kuwo live collection probe", "[providers][online-kuwo]") {
    if (infrastructure::config::process_environment("MUSIC_SERVICE_KUWO_ONLINE") != "1") {
        SKIP("set MUSIC_SERVICE_KUWO_ONLINE=1 to run");
    }
    auto clock = std::make_shared<infrastructure::SystemClock>();
    auto transport = unwrap(infrastructure::transport::HttplibTransport::create({}));
    auto ledger = std::make_shared<runtime::AdmissionLedger>(runtime::AdmissionLimits{}, clock);
    Kuwo kuwo{std::make_shared<application::transport::UpstreamClient>(ledger, transport, clock)};

    const auto playlists = unwrap(kuwo.get<ports::PlaylistSearch>()->search(context(), {"周杰伦", 5, std::nullopt}));
    REQUIRE_FALSE(playlists.page.items.empty());
    const auto& playlist = playlists.page.items[0];
    const auto playlist_tracks = unwrap(kuwo.get<ports::PlaylistTracks>()->list(context(), playlist.ref, {10, std::nullopt}));
    CHECK_FALSE(playlist_tracks.page.items.empty());
    WARN("playlist " << playlist.name << ": " << playlist_tracks.page.items.size() << " of "
                     << playlist_tracks.page.page.total.value_or(0));

    const auto albums = unwrap(kuwo.get<ports::AlbumSearch>()->search(context(), {"叶惠美", 5, std::nullopt}));
    REQUIRE_FALSE(albums.page.items.empty());
    const auto album = unwrap(kuwo.get<ports::AlbumDetail>()->detail(context(), albums.page.items[0].ref));
    const auto album_tracks = unwrap(kuwo.get<ports::AlbumTracks>()->list(context(), album.ref, {20, std::nullopt}));
    CHECK(album_tracks.page.items.size() == std::min<std::uint64_t>(20, album.track_count.value_or(0)));
    WARN("album " << album.title << ": " << album_tracks.page.items.size() << " tracks");

    const auto charts = unwrap(kuwo.get<ports::ICharts>()->charts(context()));
    REQUIRE_FALSE(charts.empty());
    const auto chart = unwrap(kuwo.get<ports::ChartTracks>()->list(context(), charts[0].ref, {10, std::string{"1"}}));
    CHECK(chart.page.items.size() == 10);
    const auto categories = unwrap(kuwo.get<ports::ICategories>()->categories(context()));
    REQUIRE_FALSE(categories.empty());
    const auto in_category = unwrap(kuwo.get<ports::CategoryPlaylists>()->list(
        context(), ref(categories[0].id), {5, std::nullopt}));
    CHECK_FALSE(in_category.page.items.empty());
    const auto recommended =
        unwrap(kuwo.get<ports::IRecommendedPlaylists>()->recommend_playlists(context(), {5, std::nullopt}));
    CHECK(recommended.page.items.size() == 5);
    WARN("charts " << charts.size() << ", categories " << categories.size() << ", " << charts[0].name << " page 2: "
                   << chart.page.items.size());
    CHECK(ledger->usage().total == 0);
}
