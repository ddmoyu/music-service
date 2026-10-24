// Collections, catalogs and lyrics (T21) on real SQLite credentials and the registry:
// playlist/album/artist search fanned out like track search with partial results
// (A05), timeouts, unsupported and disabled sources told apart, list pages fetched only
// when asked and continued only by their own cursor, details, catalogs and lyrics routed
// by capability from a catalog-only source, and the HTTP routes.

#include "../unit/providers/mock_collections.hpp"
#include "service_harness.hpp"

#include "application/accounts/credential_service.hpp"
#include "application/catalog/catalog_service.hpp"
#include "infrastructure/clock.hpp"
#include "infrastructure/observability/correlation.hpp"
#include "infrastructure/secrets/file_secret_store.hpp"
#include "infrastructure/storage/sqlite/account_repositories.hpp"
#include "infrastructure/storage/sqlite/schema.hpp"
#include "providers/registry/provider_registry.hpp"
#include "runtime/executors/bounded_executor.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace music_service;
using namespace music_service::test;
using namespace music_service::application::catalog;
namespace fs = std::filesystem;
namespace sqlite = infrastructure::storage::sqlite;
using domain::ErrorCode;

namespace {

struct Collections {
    explicit Collections(std::string_view name)
        : dir(fs::temp_directory_path() /
              infrastructure::filesystem::path_from_utf8("music-service-集合-" + std::string{name})) {
        fs::remove_all(dir);
        fs::create_directories(dir / "secrets");
        database = sqlite::Database::open(dir / "service.sqlite3", {}).value();
        REQUIRE(sqlite::initialize_schema(*database, clock->now()).has_value());
        const sqlite::RepositoryContext ctx{database.get(), clock, ids};
        identities = std::make_shared<sqlite::SqliteIdentityRepository>(ctx);
        std::map<std::string, application::config::ProviderConfig> config;
        config["off"].enabled = false;
        slow->delay_ms = 3000;
        registry = unwrap(providers::ProviderRegistry::build(
            {mock_collections("alpha", alpha), mock_collections("slow", slow), mock_collections("off", alpha),
             mock_catalog("plain")},
            config));
        credentials = std::make_shared<application::accounts::CredentialService>(
            std::make_shared<sqlite::SqliteCredentialRepository>(ctx),
            std::make_shared<infrastructure::secrets::FileSecretStore>(dir / "secrets"), registry, clock);
        executor = std::make_shared<runtime::BoundedExecutor>("catalog", 4, 32);
        CatalogLimits limits;
        limits.catalog.default_sources = {"alpha"};
        limits.catalog.per_source_budget = std::chrono::milliseconds{300};
        catalog = std::make_shared<CatalogService>(limits, registry, credentials, executor, clock,
                                                   [this] { return ids->next("cur_"); });
        owner = unwrap(identities->create_principal("owner", ports::PrincipalRole::user, std::nullopt)).id;
        other = unwrap(identities->create_principal("other", ports::PrincipalRole::user, std::nullopt)).id;
    }
    ~Collections() {
        executor->request_stop();
        executor->join();
        catalog.reset();
        credentials.reset();
        database.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    ListRequest list(std::string source, std::string id, std::optional<std::uint32_t> limit = std::nullopt,
                     std::optional<std::string> cursor = std::nullopt, std::string who = {}) const {
        return ListRequest{who.empty() ? owner : who, std::move(source), std::move(id), limit, std::move(cursor)};
    }

    fs::path dir;
    std::shared_ptr<infrastructure::SystemClock> clock = std::make_shared<infrastructure::SystemClock>();
    std::shared_ptr<infrastructure::observability::IdGenerator> ids =
        std::make_shared<infrastructure::observability::IdGenerator>(16);
    std::unique_ptr<sqlite::Database> database;
    std::shared_ptr<sqlite::SqliteIdentityRepository> identities;
    std::shared_ptr<CollectionScript> alpha = std::make_shared<CollectionScript>();
    std::shared_ptr<CollectionScript> slow = std::make_shared<CollectionScript>();
    std::shared_ptr<const providers::ProviderRegistry> registry;
    std::shared_ptr<application::accounts::CredentialService> credentials;
    std::shared_ptr<runtime::BoundedExecutor> executor;
    std::shared_ptr<CatalogService> catalog;
    std::string owner;
    std::string other;
};

template <class Result> const BasicSourceResult<typename decltype(Result::items)::value_type>&
source_of(const Result& result, std::string_view name) {
    const auto found = std::find_if(result.sources.begin(), result.sources.end(),
                                    [name](const auto& s) { return s.source == name; });
    REQUIRE(found != result.sources.end());
    return *found;
}

} // namespace

TEST_CASE("playlist, album and artist search fan out with partial results (A05)", "[catalog][collections]") {
    Collections c{"search"};
    SearchRequest request{c.owner, "night", {"alpha", "slow", "plain", "off"}, false, 10, std::nullopt, std::nullopt};
    const auto started = std::chrono::steady_clock::now();
    auto playlists = unwrap(c.catalog->search_playlists(request));
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::milliseconds{2000}); // not the slow source's 3 s
    CHECK(playlists.partial);
    CHECK_FALSE(playlists.failure);
    REQUIRE(playlists.items.size() == 2);
    CHECK(playlists.items[0].name == "night#0");
    CHECK(playlists.items[0].ref.source.value() == "alpha");
    CHECK(source_of(playlists, "alpha").status == SourceStatus::ok);
    const auto& timed_out = source_of(playlists, "slow");
    CHECK(timed_out.status == SourceStatus::failed);
    CHECK(timed_out.error->code == ErrorCode::upstream_timeout);
    CHECK(source_of(playlists, "plain").status == SourceStatus::unsupported); // no playlist search
    CHECK(source_of(playlists, "off").status == SourceStatus::disabled);

    // The cursor continues this source's playlists only.
    const auto next = source_of(playlists, "alpha").page->next_cursor;
    REQUIRE(next);
    SearchRequest more{c.owner, "night", {"alpha"}, false, 10, next, std::nullopt};
    auto second = unwrap(c.catalog->search_playlists(more));
    CHECK(second.items.front().name == "night#1");
    CHECK(c.catalog->search_tracks(more).error().code == ErrorCode::cursor_mismatch);
    CHECK(c.catalog->search_albums(more).error().code == ErrorCode::cursor_mismatch);

    auto albums = unwrap(c.catalog->search_albums({c.owner, "x", {"alpha"}, false, 5, std::nullopt, std::nullopt}));
    REQUIRE(albums.items.size() == 2);
    CHECK(albums.items[0].artists.front().name == "singer");
    auto artists = unwrap(c.catalog->search_artists({c.owner, "x", {}, false, 5, std::nullopt, std::nullopt}));
    CHECK(artists.items.size() == 2); // the default sources

    // Only unusable sources selected: a failure, no items.
    auto none = unwrap(c.catalog->search_playlists({c.owner, "x", {"plain", "off"}, false, 5, std::nullopt, std::nullopt}));
    REQUIRE(none.failure);
    CHECK(none.failure->code == ErrorCode::capability_unsupported);
    CHECK(c.catalog->search_playlists({c.owner, "x", {"nope"}, false, 5, std::nullopt, std::nullopt}).error().code ==
          ErrorCode::unknown_provider);
}

TEST_CASE("list pages are fetched when asked and continued by their own cursor", "[catalog][collections]") {
    Collections c{"lists"};
    auto first = unwrap(c.catalog->playlist_tracks(c.list("alpha", "pl1", 2)));
    CHECK(first.items.size() == 2);
    CHECK(first.items.front().title == "pl1#0");
    CHECK(first.page.limit == 2);
    CHECK(first.page.returned == 2);
    CHECK(first.page.has_more == true);
    REQUIRE(first.page.next_cursor);
    CHECK(c.alpha->calls == 1); // one page, nothing walked in the background

    auto again = unwrap(c.catalog->playlist_tracks(c.list("alpha", "pl1", 2)));
    CHECK(again.cache.hit);
    CHECK(c.alpha->calls == 1);

    auto second = unwrap(c.catalog->playlist_tracks(c.list("alpha", "pl1", 2, first.page.next_cursor)));
    CHECK(second.items.front().title == "pl1#1");
    auto third = unwrap(c.catalog->playlist_tracks(c.list("alpha", "pl1", 2, second.page.next_cursor)));
    CHECK(third.page.has_more == false);
    CHECK_FALSE(third.page.next_cursor);
    CHECK(c.alpha->calls == 3);

    // A cursor belongs to its list, entity, limit and owner.
    const auto cursor = first.page.next_cursor;
    CHECK(c.catalog->playlist_tracks(c.list("alpha", "pl2", 2, cursor)).error().code == ErrorCode::cursor_mismatch);
    CHECK(c.catalog->album_tracks(c.list("alpha", "pl1", 2, cursor)).error().code == ErrorCode::cursor_mismatch);
    CHECK(c.catalog->playlist_tracks(c.list("alpha", "pl1", 3, cursor)).error().code == ErrorCode::cursor_mismatch);
    CHECK(c.catalog->playlist_tracks(c.list("alpha", "pl1", 2, cursor, c.other)).error().code ==
          ErrorCode::cursor_expired);
    CHECK(c.catalog->playlist_tracks(c.list("alpha", "pl1", 2, std::string{"cur_unknown"})).error().code ==
          ErrorCode::cursor_expired);

    // Route limits and defaults.
    CHECK(unwrap(c.catalog->album_tracks(c.list("alpha", "al1"))).page.limit == 50);
    CHECK(c.catalog->chart_tracks(c.list("alpha", "top", 101)).error().code == ErrorCode::validation_failed);
    CHECK(c.catalog->artist_tracks(c.list("alpha", "ar1", 0)).error().code == ErrorCode::validation_failed);
    CHECK(unwrap(c.catalog->artist_albums(c.list("alpha", "ar1"))).page.limit == 20);
    CHECK(c.catalog->artist_albums(c.list("alpha", "ar1", 51)).error().code == ErrorCode::validation_failed);
    auto albums = unwrap(c.catalog->artist_albums(c.list("alpha", "ar1", 5)));
    CHECK(albums.items.front().title == "ar1#0");
    auto category = unwrap(c.catalog->category_playlists(c.list("alpha", "pop")));
    CHECK(category.items.front().name == "pop#0");
    auto recommended = unwrap(c.catalog->recommend_playlists(c.list("alpha", "", 4)));
    CHECK(recommended.items.front().name == "recommended#0");
    REQUIRE(recommended.page.next_cursor);
    CHECK(unwrap(c.catalog->recommend_playlists(c.list("alpha", "", 4, recommended.page.next_cursor)))
              .items.front()
              .name == "recommended#1");

    // A failed page is not cached.
    CHECK(c.catalog->playlist_tracks(c.list("alpha", "gone", 2)).error().code == ErrorCode::not_found);
    const auto before = c.alpha->calls.load();
    CHECK(c.catalog->playlist_tracks(c.list("alpha", "gone", 2)).error().code == ErrorCode::not_found);
    CHECK(c.alpha->calls == before + 1);
}

TEST_CASE("details, catalogs and lyrics route by capability", "[catalog][collections]") {
    Collections c{"details"};
    auto playlist = unwrap(c.catalog->playlist_detail(c.owner, "alpha", "pl1"));
    CHECK(playlist.value.name == "detail:pl1");
    CHECK(playlist.value.track_count == 120); // the platform's claim, not a page size
    CHECK(unwrap(c.catalog->album_detail(c.owner, "alpha", "al1")).value.release_date == "2003-07");
    CHECK(unwrap(c.catalog->artist_detail(c.owner, "alpha", "ar1")).value.name == "detail:ar1");
    CHECK(unwrap(c.catalog->artist_detail(c.owner, "alpha", "ar1")).cache.hit);
    CHECK(c.catalog->artist_detail(c.owner, "plain", "ar1").error().code == ErrorCode::capability_unsupported);
    CHECK(c.catalog->artist_detail(c.owner, "alpha", "gone").error().code == ErrorCode::not_found);
    CHECK(unwrap(c.catalog->chart_detail(c.owner, "alpha", "top")).value.update_hint == "daily");
    CHECK(c.catalog->album_detail(c.owner, "alpha", "gone").error().code == ErrorCode::not_found);

    // A source without the capability, a disabled one and an unknown one differ.
    CHECK(c.catalog->playlist_detail(c.owner, "plain", "pl1").error().code == ErrorCode::capability_unsupported);
    CHECK(c.catalog->playlist_detail(c.owner, "off", "pl1").error().code == ErrorCode::provider_disabled);
    CHECK(c.catalog->playlist_detail(c.owner, "nope", "pl1").error().code == ErrorCode::unknown_provider);
    CHECK(c.catalog->lyrics(c.owner, "plain", "1").error().code == ErrorCode::capability_unsupported);
    CHECK(c.catalog->playlist_tracks(c.list("plain", "pl1")).error().code == ErrorCode::capability_unsupported);

    // Catalogs carry the capability's catalog_kind.
    auto categories = unwrap(c.catalog->categories(c.owner, "alpha"));
    REQUIRE(categories.items.size() == 2);
    CHECK(categories.items[0].group == "style");
    CHECK(categories.catalog_kind == ports::CatalogKind::built_in);
    auto charts = unwrap(c.catalog->charts(c.owner, "alpha"));
    REQUIRE(charts.items.size() == 1);
    CHECK(charts.items[0].lists == domain::ListedKind::track);
    CHECK(charts.catalog_kind == ports::CatalogKind::live);
    CHECK(unwrap(c.catalog->charts(c.owner, "alpha")).cache.hit);

    // No lyrics for a track is an answer, not an error.
    auto lyrics = unwrap(c.catalog->lyrics(c.owner, "alpha", "1"));
    CHECK(lyrics.value.lrc);
    CHECK_FALSE(lyrics.value.unavailable);
    REQUIRE(lyrics.value.karaoke.size() == 1);
    CHECK(lyrics.value.karaoke[0].words[1].start_ms == 3000);
    auto silent = unwrap(c.catalog->lyrics(c.owner, "alpha", "silent"));
    CHECK(silent.value.unavailable);
    CHECK_FALSE(silent.value.lrc);

    // The catalog-only source declares no audio; its catalog works anyway.
    const auto view = unwrap(c.registry->describe("alpha"));
    CHECK_FALSE(view.supports("resolve_audio"));
    CHECK(view.supports("playlist_tracks"));
}

TEST_CASE("collection routes over HTTP", "[catalog][collections][server]") {
    Deployment d{"collections-http"};
    REQUIRE(d.run({"init"}).code == server::exit_ok);
    const auto token = d.read(d.bootstrap());
    auto script = std::make_shared<CollectionScript>();
    RunningService service{d, std::make_shared<infrastructure::observability::MemorySink>(),
                           std::vector<ports::ProviderBundle>{mock_collections("cat", script), mock_catalog("plain")},
                           true};
    auto api = service.client(token);

    const auto search = api.Get("/api/v1/search/playlists?q=night&sources=cat,plain&limit=5");
    REQUIRE(search->status == 200);
    const auto searched = body(search);
    CHECK(searched["data"]["items"][0]["name"] == "night#0");
    CHECK(searched["data"]["items"][0]["track_count"] == 120);
    CHECK(searched["meta"]["partial"] == true);
    CHECK(searched["meta"]["source_results"][1]["status"] == "unsupported");
    CHECK(searched["meta"]["source_results"][0]["page"]["listed_total"] == 6);
    CHECK(searched["meta"]["source_results"][0]["page"]["unavailable_count"] == 0);
    const auto cached_search = body(api.Get("/api/v1/search/playlists?q=night&sources=cat,plain&limit=5"));
    CHECK(cached_search["meta"]["source_results"][0]["cache"]["hit"] == true);
    CHECK(cached_search["meta"]["source_results"][0]["page"]["listed_total"] == 6);
    CHECK(api.Get("/api/v1/search/albums?q=x&sources=cat")->status == 200);
    CHECK(body(api.Get("/api/v1/search/artists?q=x&sources=cat"))["data"]["items"][0]["name"] == "x#0");
    CHECK(api.Get("/api/v1/search/playlists?q=x&sources=plain")->status == 422);

    const auto first = body(api.Get("/api/v1/playlists/tracks?source=cat&id=pl1&limit=2"))["data"];
    CHECK(first["items"].size() == 2);
    CHECK(first["items"][0]["title"] == "pl1#0");
    CHECK(first["page"]["has_more"] == true);
    const auto cursor = first["page"]["next_cursor"].get<std::string>();
    const auto second =
        body(api.Get("/api/v1/playlists/tracks?source=cat&id=pl1&limit=2&cursor=" + cursor))["data"];
    CHECK(second["items"][0]["title"] == "pl1#1");
    CHECK(api.Get("/api/v1/playlists/tracks?source=cat&id=pl2&limit=2&cursor=" + cursor)->status == 400);
    CHECK(error_code(api.Get("/api/v1/playlists/tracks?source=cat&id=pl2&limit=2&cursor=" + cursor)) ==
          "CURSOR_MISMATCH");
    CHECK(api.Get("/api/v1/albums/tracks?source=cat&id=al1&limit=101")->status == 400);
    CHECK(api.Get("/api/v1/playlists/tracks?source=cat")->status == 400);
    CHECK(api.Get("/api/v1/playlists/tracks?source=cat&id=pl1&page=2")->status == 400); // unknown parameter

    CHECK(body(api.Get("/api/v1/playlists/detail?source=cat&id=pl1"))["data"]["name"] == "detail:pl1");
    CHECK(body(api.Get("/api/v1/albums/detail?source=cat&id=al1"))["data"]["artists"][0]["name"] == "singer");
    CHECK(body(api.Get("/api/v1/artists/detail?source=cat&id=ar1"))["data"]["name"] == "detail:ar1");
    CHECK(body(api.Get("/api/v1/artists/detail?source=cat&id=ar1"))["data"]["description"].is_null());
    CHECK(api.Get("/api/v1/artists/detail?source=cat&id=gone")->status == 404);
    CHECK(error_code(api.Get("/api/v1/artists/detail?source=plain&id=ar1")) == "CAPABILITY_UNSUPPORTED");
    CHECK(api.Get("/api/v1/artists/detail?source=cat")->status == 400);
    CHECK(body(api.Get("/api/v1/charts/detail?source=cat&id=top"))["data"]["lists"] == "track");
    CHECK(api.Get("/api/v1/albums/detail?source=cat&id=gone")->status == 404);
    const auto unsupported = api.Get("/api/v1/playlists/detail?source=plain&id=pl1");
    CHECK(unsupported->status == 422);
    CHECK(error_code(unsupported) == "CAPABILITY_UNSUPPORTED");
    CHECK(body(api.Get("/api/v1/artists/albums?source=cat&id=ar1"))["data"]["page"]["limit"] == 20);
    CHECK(body(api.Get("/api/v1/artists/tracks?source=cat&id=ar1"))["data"]["page"]["limit"] == 50);
    CHECK(body(api.Get("/api/v1/charts/tracks?source=cat&id=top&limit=3"))["data"]["items"].size() == 2);
    CHECK(body(api.Get("/api/v1/categories/playlists?source=cat&category_id=pop"))["data"]["items"][0]["name"] ==
          "pop#0");
    const auto category_tracks = body(api.Get("/api/v1/categories/tracks?source=cat&category_id=rock&limit=2"))["data"];
    CHECK(category_tracks["items"][0]["title"] == "rock#0");
    const auto category_cursor = category_tracks["page"]["next_cursor"].get<std::string>();
    CHECK(body(api.Get("/api/v1/categories/tracks?source=cat&category_id=rock&limit=2&cursor=" + category_cursor))
          ["data"]["items"][0]["title"] == "rock#1");
    CHECK(error_code(api.Get("/api/v1/categories/tracks?source=cat&category_id=pop&limit=2&cursor=" + category_cursor)) == "CURSOR_MISMATCH");
    CHECK(error_code(api.Get("/api/v1/charts/tracks?source=cat&id=rock&limit=2&cursor=" + category_cursor)) == "CURSOR_MISMATCH");
    CHECK(api.Get("/api/v1/categories/tracks?source=cat&category_id=rock&limit=51")->status == 400);
    CHECK(api.Get("/api/v1/categories/tracks?source=cat")->status == 400);
    CHECK(error_code(api.Get("/api/v1/categories/tracks?source=plain&category_id=rock")) == "CAPABILITY_UNSUPPORTED");
    CHECK(service.client().Get("/api/v1/categories/tracks?source=cat&category_id=rock")->status == 401);
    CHECK(body(api.Get("/api/v1/recommendations/playlists?source=cat&limit=4"))["data"]["items"][0]["name"] ==
          "recommended#0");

    const auto categories = body(api.Get("/api/v1/categories?source=cat"))["data"];
    CHECK(categories["catalog_kind"] == "built_in");
    CHECK(categories["items"][0]["source"] == "cat");
    CHECK(categories["items"][0]["group"] == "style");
    CHECK(categories["items"][1]["group"].is_null());
    CHECK(body(api.Get("/api/v1/charts?source=cat"))["data"]["catalog_kind"] == "live");

    const auto lyrics = body(api.Get("/api/v1/lyrics?source=cat&id=1"))["data"];
    CHECK(lyrics["lrc"].get<std::string>().rfind("[00:01.00]", 0) == 0);
    CHECK(lyrics["karaoke"][0]["words"][0]["duration_ms"] == 2000);
    const auto silent = api.Get("/api/v1/lyrics?source=cat&id=silent");
    REQUIRE(silent->status == 200);
    CHECK(body(silent)["data"]["lrc"].is_null());
    CHECK(body(silent)["data"]["unavailable"] == true);
    CHECK(api.Get("/api/v1/lyrics?source=plain&id=1")->status == 422);

    // catalog:read is required.
    CHECK(service.client().Get("/api/v1/charts?source=cat")->status == 401);
}
