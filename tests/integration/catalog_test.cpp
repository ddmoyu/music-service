// Catalog and asset use cases on real SQLite credentials, a manual clock and scripted
// platforms whose results reveal which account served them.

#include "../unit/providers/mock_providers.hpp"

#include "application/accounts/credential_service.hpp"
#include "application/catalog/asset_service.hpp"
#include "application/catalog/catalog_service.hpp"
#include "infrastructure/clock.hpp"
#include "infrastructure/filesystem/paths.hpp"
#include "infrastructure/observability/correlation.hpp"
#include "infrastructure/secrets/file_secret_store.hpp"
#include "infrastructure/storage/sqlite/account_repositories.hpp"
#include "infrastructure/storage/sqlite/schema.hpp"
#include "providers/registry/provider_registry.hpp"
#include "runtime/executors/bounded_executor.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <future>
#include <thread>

using namespace music_service;
using namespace music_service::test;
using namespace music_service::application::catalog;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
using domain::ErrorCode;

namespace {

// Shared knobs of one scripted source.
struct Script {
    std::atomic<int> searches{0};
    std::atomic<int> details{0};
    std::atomic<int> resolves{0};
    std::atomic<int> delay_ms{0};
    std::atomic<bool> fail{false};
    std::atomic<bool> throws{false};
    std::atomic<bool> timeout{false};
    std::atomic<bool> pages{true};
    std::atomic<bool> preview{false};
    std::atomic<bool> low_quality{false};
    std::atomic<bool> stale_address{false};
    std::atomic<std::size_t> cursor_padding{0};
};

// Who served the call: "anonymous" or the credential id.
std::string account_of(const ports::ProviderContext& context) {
    const auto& id = context.identity.binding().credential_id();
    return id ? *id : "anonymous";
}

class ScriptedSource final : public ports::ITrackSearch,
                             public ports::IUserPlaylists,
                             public ports::ITrackDetail,
                             public ports::IAssetResolver,
                             public ports::ILinkParser {
public:
    ScriptedSource(domain::SourceId source, std::shared_ptr<Script> script)
        : source_(std::move(source)), script_(std::move(script)) {}

    domain::Result<ports::CatalogPage<domain::Playlist>> user_playlists(const ports::ProviderContext& c,
        const ports::ListQuery& q) override {
        ++script_->searches;
        if (auto e = behave(c)) return *e;
        ports::CatalogPage<domain::Playlist> p;
        domain::Playlist playlist{test::track(source_.value(), "1"), account_of(c)};
        playlist.relationship = "created";
        p.page.items.push_back(std::move(playlist));
        p.page.page = {q.limit,1,std::nullopt,!q.cursor};
        if (!q.cursor) p.next_cursor = "1";
        return p;
    }

    domain::Result<ports::TrackSearchPage> search_tracks(const ports::ProviderContext& context,
                                                         const ports::TrackSearchQuery& query) override {
        ++script_->searches;
        if (auto failed = behave(context)) {
            return *failed;
        }
        const int page = query.cursor ? std::stoi(*query.cursor) : 0;
        ports::TrackSearchPage result;
        domain::Track track{{source_, unwrap(domain::PlatformId::parse(std::to_string(page + 1)))},
                            query.text + "@" + account_of(context) + "#" + std::to_string(page),
                            {{"singer", std::nullopt}},
                            std::nullopt,
                            200000,
                            {}};
        result.page.items.push_back(std::move(track));
        result.page.page = {query.limit, 1, 10, script_->pages.load()};
        if (script_->pages) {
            result.next_cursor = std::to_string(page + 1) + std::string(script_->cursor_padding.load(), ' ');
        }
        return result;
    }

    domain::Result<domain::Track> track_detail(const ports::ProviderContext& context,
                                               const domain::TrackRef& track) override {
        ++script_->details;
        if (auto failed = behave(context)) {
            return *failed;
        }
        return domain::Track{track, "detail@" + account_of(context), {}, std::nullopt, 1000, {}};
    }

    domain::Result<ports::ResolvedAudio> resolve_audio(const ports::ProviderContext& context,
                                                       const domain::TrackRef& track,
                                                       const domain::ResourceRequirement&) override {
        ++script_->resolves;
        if (auto failed = behave(context)) {
            return *failed;
        }
        std::optional<std::chrono::system_clock::time_point> expires;
        if (script_->stale_address) {
            expires = std::chrono::system_clock::time_point{std::chrono::seconds{1'700'000'010}};
        }
        return ports::ResolvedAudio{
            track,
            script_->low_quality ? std::optional{domain::Quality::standard} : std::optional{domain::Quality::high},
            script_->preview ? domain::Completeness::preview : domain::Completeness::full,
            "mp3",
            script_->low_quality ? 128000 : 320000,
            "https://cdn.example/" + track.id.value() + ".mp3",
            {{"User-Agent", "x"}, {"Cookie", "account=" + account_of(context)}},
            expires,
            std::nullopt,
            ports::ProcessingPlan{}};
    }

    domain::Result<ports::ParsedLink> parse_link(std::string_view link) override {
        const std::string prefix = "https://" + source_.value() + ".example/song/";
        if (link.substr(0, prefix.size()) != prefix) {
            return domain::Error::validation("link", "not ours");
        }
        return ports::ParsedLink{"track", {source_, unwrap(domain::PlatformId::parse(link.substr(prefix.size())))}};
    }

private:
    std::optional<domain::Error> behave(const ports::ProviderContext& context) const {
        if (script_->throws) throw std::runtime_error("injected source failure");
        if (const auto delay = script_->delay_ms.load(); delay > 0) {
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds{delay};
            while (std::chrono::steady_clock::now() < until && !context.operation.stop.stop_requested()) {
                std::this_thread::sleep_for(5ms);
            }
        }
        if (script_->timeout) {
            return domain::Error{ErrorCode::upstream_timeout, "slow", true, {}};
        }
        if (script_->fail) {
            return domain::Error{ErrorCode::upstream_failed, "broken", true, {}};
        }
        return std::nullopt;
    }

    domain::SourceId source_;
    std::shared_ptr<Script> script_;
};

ports::ProviderBundle scripted(std::string_view name, std::shared_ptr<Script> script, bool audio = true) {
    auto bundle = base_bundle(name);
    bundle.auth_profiles = {{.auth_profile = "web_cookie", .kind = ports::CredentialKind::cookie}};
    const std::vector<std::string> profiles{"web_cookie"};
    bundle.capabilities = {
        {.key = "user_playlists", .credentials = ports::CredentialRequirement::required, .auth_profiles = profiles,
         .pagination = ports::Pagination::cursor},
        {.key = "search_tracks", .credentials = ports::CredentialRequirement::optional, .auth_profiles = profiles,
         .pagination = ports::Pagination::cursor},
        {.key = "track_detail", .credentials = ports::CredentialRequirement::optional, .auth_profiles = profiles},
        {.key = "parse_links"},
    };
    auto shared = std::make_shared<std::shared_ptr<ScriptedSource>>();
    const auto object = [shared, script, source = source(name)]() {
        if (!*shared) {
            *shared = std::make_shared<ScriptedSource>(source, script);
        }
        return *shared;
    };
    bundle.bindings = {
        ports::CapabilityBinding::bind<ports::IUserPlaylists>(
            [object](const ports::ProviderSetup&) -> domain::Result<std::shared_ptr<ports::IUserPlaylists>> {
                return std::shared_ptr<ports::IUserPlaylists>{object()};
            }),
        ports::CapabilityBinding::bind<ports::ITrackSearch>(
            [object](const ports::ProviderSetup&) -> domain::Result<std::shared_ptr<ports::ITrackSearch>> {
                return std::shared_ptr<ports::ITrackSearch>{object()};
            }),
        ports::CapabilityBinding::bind<ports::ITrackDetail>(
            [object](const ports::ProviderSetup&) -> domain::Result<std::shared_ptr<ports::ITrackDetail>> {
                return std::shared_ptr<ports::ITrackDetail>{object()};
            }),
        ports::CapabilityBinding::bind<ports::ILinkParser>(
            [object](const ports::ProviderSetup&) -> domain::Result<std::shared_ptr<ports::ILinkParser>> {
                return std::shared_ptr<ports::ILinkParser>{object()};
            }),
    };
    if (audio) {
        bundle.capabilities.push_back(
            {.key = "resolve_audio", .credentials = ports::CredentialRequirement::optional, .auth_profiles = profiles});
        bundle.bindings.push_back(ports::CapabilityBinding::bind<ports::IAssetResolver>(
            [object](const ports::ProviderSetup&) -> domain::Result<std::shared_ptr<ports::IAssetResolver>> {
                return std::shared_ptr<ports::IAssetResolver>{object()};
            }));
    }
    return bundle;
}

// Answers probes with a 206 and MP3 bytes, recording what was sent.
class ProbeUpstream final : public ports::UpstreamHttp {
public:
    domain::Result<ports::HopResult<ports::HttpResponse>> fetch(ports::HttpRequest, const ports::AdmissionRequest&,
                                                                const ports::OperationContext&) override {
        return domain::Error{ErrorCode::internal, "not used", false, {}};
    }
    domain::Result<ports::HopResult<ports::HttpResponseHead>>
    stream(ports::HttpRequest request, const ports::AdmissionRequest& base, const ports::OperationContext&,
           const ports::HeadConsumer& on_head, const ports::ChunkConsumer& on_chunk) override {
        last = request;
        last_base = base;
        ports::HttpResponseHead head{206, {{"Content-Type", "audio/mpeg"}, {"Content-Range", "bytes 0-15/8000000"}},
                                     request.url, nullptr, false};
        if (!on_head(head)) {
            head.stopped_by_consumer = true;
            return ports::HopResult<ports::HttpResponseHead>{head};
        }
        const std::string bytes = "ID3\x04\x00\x00\x00\x00\x00\x00 rest of the song...";
        std::size_t offered = 0;
        while (offered < bytes.size()) {
            const auto n = std::min<std::size_t>(5, bytes.size() - offered);
            ++chunks;
            if (!on_chunk(std::span<const char>{bytes.data() + offered, n})) {
                break;
            }
            offered += n;
        }
        return ports::HopResult<ports::HttpResponseHead>{head};
    }

    ports::HttpRequest last;
    ports::AdmissionRequest last_base{"", source("x"), "", ports::TrafficClass::short_request, std::nullopt, "", "", {}};
    int chunks{0};
};

struct Catalog {
    explicit Catalog(std::string_view name, CatalogLimits catalog_limits = defaults())
        : dir(fs::temp_directory_path() / infrastructure::filesystem::path_from_utf8("music-service-目录-" +
                                                                                     std::string{name})) {
        fs::remove_all(dir);
        fs::create_directories(dir / "secrets");
        database = infrastructure::storage::sqlite::Database::open(dir / "service.sqlite3", {}).value();
        REQUIRE(infrastructure::storage::sqlite::initialize_schema(*database, clock->now()).has_value());
        const infrastructure::storage::sqlite::RepositoryContext ctx{database.get(), clock, ids};
        identities = std::make_shared<infrastructure::storage::sqlite::SqliteIdentityRepository>(ctx);
        auto repo = std::make_shared<infrastructure::storage::sqlite::SqliteCredentialRepository>(ctx);
        std::map<std::string, application::config::ProviderConfig> config;
        config["off"].enabled = false;
        registry = unwrap(providers::ProviderRegistry::build(
            {scripted("alpha", alpha), scripted("beta", beta), scripted("off", std::make_shared<Script>()),
             scripted("meta", std::make_shared<Script>(), false), mock_audio("aud")},
            config));
        credentials = std::make_shared<application::accounts::CredentialService>(
            repo, std::make_shared<infrastructure::secrets::FileSecretStore>(dir / "secrets"), registry, clock);
        executor = std::make_shared<runtime::BoundedExecutor>("catalog", 4, 32);
        catalog = std::make_shared<CatalogService>(catalog_limits, registry, credentials, executor, clock,
                                                   [this] { return ids->next("cur_"); });
        assets = std::make_shared<AssetService>(catalog_limits.lifetimes, catalog_limits.handles, 4, registry,
                                                credentials, probe, clock, [this] { return ids->next("ast_"); }, 5s);
    }
    ~Catalog() {
        executor->request_stop();
        executor->join();
        assets.reset();
        catalog.reset();
        credentials.reset();
        database.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    static CatalogLimits defaults() {
        CatalogLimits limits;
        limits.catalog.default_sources = {"alpha"};
        return limits;
    }

    std::string principal(std::string name = "user") {
        return unwrap(identities->create_principal(std::move(name), ports::PrincipalRole::user, std::nullopt)).id;
    }
    std::string cookie(const std::string& owner, std::string_view source_name, std::string_view value) {
        return unwrap(credentials->create({owner, std::string{source_name}, "", ports::CredentialKind::cookie,
                                           "web_cookie", ports::SecretBytes{value}, std::nullopt}))
            .id;
    }
    SearchResult search(const std::string& owner, std::vector<std::string> sources, std::string q = "song",
                        std::optional<std::string> cursor = std::nullopt) {
        return unwrap(catalog->search_tracks({owner, std::move(q), std::move(sources), false, 5, std::move(cursor), std::nullopt}));
    }
    domain::Error search_error(const std::string& owner, std::vector<std::string> sources, std::string q,
                               std::optional<std::string> cursor = std::nullopt) {
        auto result = catalog->search_tracks({owner, std::move(q), std::move(sources), false, 5, std::move(cursor), std::nullopt});
        REQUIRE_FALSE(result.has_value());
        return std::move(result).error();
    }

    fs::path dir;
    std::shared_ptr<infrastructure::ManualClock> clock = std::make_shared<infrastructure::ManualClock>();
    std::shared_ptr<infrastructure::observability::IdGenerator> ids =
        std::make_shared<infrastructure::observability::IdGenerator>(5);
    std::shared_ptr<Script> alpha = std::make_shared<Script>();
    std::shared_ptr<Script> beta = std::make_shared<Script>();
    std::shared_ptr<ProbeUpstream> probe = std::make_shared<ProbeUpstream>();
    std::unique_ptr<infrastructure::storage::sqlite::Database> database;
    std::shared_ptr<infrastructure::storage::sqlite::SqliteIdentityRepository> identities;
    std::shared_ptr<const providers::ProviderRegistry> registry;
    std::shared_ptr<application::accounts::CredentialService> credentials;
    std::shared_ptr<runtime::BoundedExecutor> executor;
    std::shared_ptr<CatalogService> catalog;
    std::shared_ptr<AssetService> assets;
};

const SourceResult& source_result(const SearchResult& result, std::string_view source) {
    const auto it = std::find_if(result.sources.begin(), result.sources.end(),
                                 [&](const SourceResult& s) { return s.source == source; });
    REQUIRE(it != result.sources.end());
    return *it;
}

} // namespace

TEST_CASE("a source exception finishes promptly and the same query can retry", "[stability][catalog]") {
    Catalog c{"source-exception"};
    const auto owner = c.principal();
    c.alpha->throws = true;
    const auto start = std::chrono::steady_clock::now();
    const auto failed = c.search(owner, {"alpha"}, "retry");
    CHECK(std::chrono::steady_clock::now() - start < 1s);
    REQUIRE(failed.failure);
    CHECK(failed.sources[0].error->code == ErrorCode::internal);
    c.alpha->throws = false;
    CHECK_FALSE(c.search(owner, {"alpha"}, "retry").failure);
    CHECK(c.alpha->searches == 2);
}

TEST_CASE("large cursor payloads consume the configured handle byte budget", "[stability][catalog]") {
    auto limits = Catalog::defaults();
    limits.handles.total_bytes = 4096;
    Catalog c{"cursor-bytes", limits};
    const auto owner = c.principal();
    c.alpha->cursor_padding = 5000;
    const auto result = c.search(owner, {"alpha"}, "large");
    REQUIRE(result.failure);
    CHECK(result.failure->code == ErrorCode::service_busy);
}

TEST_CASE("search reports every source and combines the outcome", "[catalog]") {
    Catalog c{"sources"};
    const auto owner = c.principal();

    // Default sources when none are given.
    const auto defaulted = c.search(owner, {});
    REQUIRE(defaulted.sources.size() == 1);
    CHECK(defaulted.sources[0].source == "alpha");
    CHECK(defaulted.items.at(0).title == "song@anonymous#0");

    // One fails, one succeeds: 200 partial with both reported.
    c.beta->fail = true;
    const auto mixed = c.search(owner, {"alpha", "beta", "meta"}, "mix");
    CHECK(mixed.partial);
    CHECK_FALSE(mixed.failure);
    CHECK(mixed.items.size() == 2); // alpha and meta, in request order
    CHECK(source_result(mixed, "alpha").status == SourceStatus::ok);
    CHECK(source_result(mixed, "beta").status == SourceStatus::failed);
    CHECK(source_result(mixed, "beta").error->code == ErrorCode::upstream_failed);
    CHECK_FALSE(source_result(mixed, "beta").page);

    // Every executed source failed: timeouts alone -> UPSTREAM_TIMEOUT, else UPSTREAM_FAILED.
    c.beta->fail = false;
    c.beta->timeout = true;
    CHECK(c.search(owner, {"beta"}, "t1").failure->code == ErrorCode::upstream_timeout);
    c.alpha->fail = true;
    CHECK(c.search(owner, {"alpha", "beta"}, "t2").failure->code == ErrorCode::upstream_failed);
    c.alpha->fail = false;
    c.beta->timeout = false;

    // Nothing usable selected: disabled and unsupported sources are reported, not run.
    const auto unusable = c.search(owner, {"off", "aud"}, "u");
    CHECK(unusable.failure->code == ErrorCode::capability_unsupported);
    CHECK(source_result(unusable, "off").status == SourceStatus::disabled);
    CHECK(source_result(unusable, "aud").status == SourceStatus::unsupported);

    // Request validation.
    CHECK(c.search_error(owner, {"ghost"}, "x").code == ErrorCode::unknown_provider);
    CHECK(c.search_error(owner, {"alpha"}, "   ").code == ErrorCode::validation_failed);
    CHECK(c.search_error(owner, {"alpha"}, std::string(201, 'a')).code == ErrorCode::validation_failed);
    CHECK(c.catalog->search_tracks({owner, "x", {"alpha"}, false, 51, std::nullopt, std::nullopt}).error().code ==
          ErrorCode::validation_failed);
    CHECK(c.catalog->search_tracks({owner, "x", {"alpha"}, false, 5, std::nullopt, 500ms}).error().code ==
          ErrorCode::validation_failed);
    CHECK(c.search(owner, {"alpha", "alpha"}, "dup").sources.size() == 1);

    // "all" means the enabled sources that support search.
    const auto all = unwrap(c.catalog->search_tracks({owner, "all", {}, true, 5, std::nullopt, std::nullopt}));
    CHECK(all.sources.size() == 3); // alpha, beta, meta; not off, not aud
}

TEST_CASE("a slow source times out alone and does not hold the request", "[catalog]") {
    auto limits = Catalog::defaults();
    limits.catalog.per_source_budget = 300ms;
    Catalog c{"timeouts", limits};
    const auto owner = c.principal();
    c.beta->delay_ms = 3000;

    const auto started = std::chrono::steady_clock::now();
    const auto result = unwrap(c.catalog->search_tracks({owner, "slow", {"alpha", "beta"}, false, 5, std::nullopt, 1000ms}));
    CHECK(std::chrono::steady_clock::now() - started < 1500ms);
    CHECK(result.partial);
    CHECK(source_result(result, "alpha").status == SourceStatus::ok);
    const auto& slow = source_result(result, "beta");
    CHECK(slow.status == SourceStatus::failed);
    CHECK(slow.error->retryable);
}

TEST_CASE("late search results are dropped, not accepted or cached", "[catalog]") {
    auto limits = Catalog::defaults();
    limits.catalog.per_source_budget = 300ms;
    Catalog c{"late", limits};
    const auto owner = c.principal();

    // Past its own budget but inside the request's: still a timeout for that source.
    c.beta->delay_ms = 650;
    const auto slow = unwrap(c.catalog->search_tracks({owner, "late", {"beta"}, false, 5, std::nullopt, 1000ms}));
    REQUIRE(slow.failure);
    CHECK(slow.failure->code == ErrorCode::upstream_timeout);
    CHECK(slow.items.empty());
    CHECK_FALSE(source_result(slow, "beta").page);

    // Past the request's deadline: the work that finishes afterwards leaves no cache entry.
    limits.catalog.per_source_budget = 5000ms;
    Catalog d{"late-total", limits};
    const auto other = d.principal();
    d.beta->delay_ms = 3000;
    const auto timed_out = unwrap(d.catalog->search_tracks({other, "late", {"beta"}, false, 5, std::nullopt, 1000ms}));
    CHECK(timed_out.failure->code == ErrorCode::upstream_timeout);
    std::this_thread::sleep_for(300ms); // the stopped work returns in the background

    for (auto* catalog : {&c, &d}) {
        catalog->beta->delay_ms = 0;
        const auto before = catalog->beta->searches.load();
        const auto again =
            unwrap(catalog->catalog->search_tracks({catalog == &c ? owner : other, "late", {"beta"}, false, 5,
                                                    std::nullopt, 1000ms}));
        CHECK(source_result(again, "beta").status == SourceStatus::ok);
        CHECK_FALSE(source_result(again, "beta").cache->hit);
        CHECK(catalog->beta->searches.load() - before == 1);
    }
}

TEST_CASE("a page that cannot get a cursor is not reported as a success", "[catalog]") {
    auto limits = Catalog::defaults();
    limits.handles.cursors = 4;
    limits.per_owner_handle_share = 4; // one live cursor per principal
    Catalog c{"cursor-capacity", limits};
    const auto owner = c.principal();

    const auto first = c.search(owner, {"alpha"}, "one");
    REQUIRE(first.sources[0].page->next_cursor);

    const auto second = c.search(owner, {"alpha"}, "two");
    REQUIRE(second.failure);
    CHECK(second.failure->code == ErrorCode::service_busy);
    CHECK(second.failure->retryable);
    const auto& alpha = source_result(second, "alpha");
    CHECK(alpha.status == SourceStatus::failed);
    CHECK(alpha.error->code == ErrorCode::service_busy);
    CHECK_FALSE(alpha.page);
    CHECK(second.items.empty());

    // A page without more results needs no cursor; next to it the result is partial,
    // still naming the source that could not continue.
    c.beta->pages = false;
    const auto mixed = c.search(owner, {"alpha", "beta"}, "three");
    CHECK_FALSE(mixed.failure);
    CHECK(mixed.partial);
    CHECK(source_result(mixed, "alpha").error->code == ErrorCode::service_busy);
    CHECK(source_result(mixed, "beta").status == SourceStatus::ok);
    CHECK(mixed.items.size() == 1);
}

TEST_CASE("expired handles stay expired after they are purged", "[catalog]") {
    int next = 0;
    HandleTable<int> table{2, 2, [&] { return "h" + std::to_string(++next); }};
    const auto start = std::chrono::system_clock::time_point{std::chrono::seconds{1'700'000'000}};
    const auto later = start + 10s;

    const auto old = unwrap(table.put("alice", 1, start + 5s, start));
    CHECK(table.get("alice", old, later).error().code == ErrorCode::resource_expired);
    // Creating any handle purges the expired one; it still reads as expired to its owner only.
    (void)unwrap(table.put("bob", 2, later + 60s, later));
    CHECK(table.size() == 1);
    CHECK(table.get("alice", old, later).error().code == ErrorCode::resource_expired);
    CHECK(table.get("alice", old, later, ErrorCode::cursor_expired).error().code == ErrorCode::cursor_expired);
    CHECK(table.get("bob", old, later).error().code == ErrorCode::not_found);
    CHECK(table.get("alice", "h99", later).error().code == ErrorCode::not_found);

    // The record is bounded: only the most recent `capacity` expired ids are kept.
    const auto a = unwrap(table.put("alice", 3, later + 1s, later)); // fills the table (capacity 2)
    CHECK(table.put("alice", 4, later + 60s, later).error().code == ErrorCode::service_busy);
    const auto after = later + 70s;
    (void)unwrap(table.put("carol", 5, after + 60s, after)); // purges a and bob's; old falls out
    CHECK(table.get("alice", a, after).error().code == ErrorCode::resource_expired);
    CHECK(table.get("alice", old, after).error().code == ErrorCode::not_found);
}

TEST_CASE("search continuation payloads count toward the catalog cache byte budget", "[catalog]") {
    auto limits = Catalog::defaults();
    limits.cache.catalog_bytes = 4096;
    Catalog c{"cursor-bytes", limits};
    const auto owner = c.principal("owner");
    c.alpha->cursor_padding = 4096;
    const auto first = c.search(owner, {"alpha"}, "bounded");
    REQUIRE(first.sources[0].page->next_cursor);
    CHECK_FALSE(first.sources[0].cache->hit);
    const auto second = c.search(owner, {"alpha"}, "bounded");
    CHECK_FALSE(second.sources[0].cache->hit);
    CHECK(c.alpha->searches == 2);
}

TEST_CASE("results are cached per identity and identical calls merge", "[catalog]") {
    Catalog c{"cache"};
    const auto owner = c.principal();

    const auto first = c.search(owner, {"alpha"}, "cached");
    CHECK_FALSE(first.sources[0].cache->hit);
    const auto second = c.search(owner, {"alpha"}, "cached");
    CHECK(second.sources[0].cache->hit);
    CHECK(c.alpha->searches == 1);
    c.clock->advance(61s); // cache.catalog_ttl
    CHECK_FALSE(c.search(owner, {"alpha"}, "cached").sources[0].cache->hit);
    CHECK(c.alpha->searches == 2);

    // Failures are not cached.
    c.beta->fail = true;
    (void)c.search(owner, {"beta"}, "broken");
    c.beta->fail = false;
    CHECK(c.search(owner, {"beta"}, "broken").sources[0].status == SourceStatus::ok);
    CHECK(c.beta->searches == 2);

    // Concurrent identical calls: one upstream call.
    c.alpha->delay_ms = 300;
    const auto before = c.alpha->searches.load();
    auto one = std::async(std::launch::async, [&] { return c.search(owner, {"alpha"}, "together"); });
    std::this_thread::sleep_for(50ms);
    auto two = std::async(std::launch::async, [&] { return c.search(owner, {"alpha"}, "together"); });
    CHECK(one.get().items.at(0).title == two.get().items.at(0).title);
    CHECK(c.alpha->searches.load() - before == 1);
}

TEST_CASE("personal playlist caches and cursors follow account principal version and revocation", "[catalog][stage-d]") {
    Catalog c{"personal"};
    const auto owner = c.principal(), other = c.principal("other");
    ListRequest request{owner,"alpha","",2,{}};
    CHECK(c.catalog->user_playlists(request).error().code == ErrorCode::credentials_required);
    const auto a = c.cookie(owner,"alpha","a=1");
    REQUIRE(c.credentials->set_default(owner,"alpha",a));
    auto first = unwrap(c.catalog->user_playlists(request));
    CHECK(first.items.front().name == a);
    CHECK(unwrap(c.catalog->user_playlists(request)).cache.hit);
    auto next = request; next.cursor = first.page.next_cursor;
    REQUIRE(next.cursor);
    const auto b = c.cookie(owner,"alpha","b=1");
    REQUIRE(c.credentials->set_default(owner,"alpha",b));
    CHECK(c.catalog->user_playlists(next).error().code == ErrorCode::cursor_expired);
    auto second = unwrap(c.catalog->user_playlists(request));
    CHECK(second.items.front().name == b); CHECK_FALSE(second.cache.hit);
    REQUIRE(c.credentials->set_default(owner,"alpha",a,ports::SecretBytes{"a=1"}));
    CHECK(c.catalog->user_playlists(next).error().code == ErrorCode::cursor_expired);
    next.cursor = unwrap(c.catalog->user_playlists(request)).page.next_cursor;
    REQUIRE(c.credentials->update(owner,a,{ports::SecretBytes{"a=2"},{},{}}));
    CHECK(c.catalog->user_playlists(next).error().code == ErrorCode::cursor_expired);
    CHECK_FALSE(unwrap(c.catalog->user_playlists(request)).cache.hit);
    const auto foreign = c.cookie(other,"alpha","a=2");
    REQUIRE(c.credentials->set_default(other,"alpha",foreign));
    auto other_request = request; other_request.owner_id = other;
    CHECK(unwrap(c.catalog->user_playlists(other_request)).items.front().name == foreign);
    other_request.cursor = unwrap(c.catalog->user_playlists(request)).page.next_cursor;
    CHECK(c.catalog->user_playlists(other_request).error().code == ErrorCode::cursor_expired);
    REQUIRE(c.credentials->revoke(owner,a));
    CHECK(c.catalog->user_playlists(request).error().code == ErrorCode::credentials_required);
}

TEST_CASE("accounts partition caches and cursors", "[catalog]") {
    Catalog c{"identity"};
    const auto owner = c.principal();
    const auto a = c.cookie(owner, "alpha", "a=1");
    REQUIRE(c.credentials->set_default(owner, "alpha", a));

    // A38: two version-1 credentials never share results.
    const auto with_a = c.search(owner, {"alpha"});
    CHECK(with_a.items.at(0).title == "song@" + a + "#0");
    const auto cursor = *with_a.sources[0].page->next_cursor;
    CHECK(cursor.rfind("cur_", 0) == 0);
    const auto b = c.cookie(owner, "alpha", "b=1");
    REQUIRE(c.credentials->set_default(owner, "alpha", b));
    const auto with_b = c.search(owner, {"alpha"});
    CHECK(with_b.items.at(0).title == "song@" + b + "#0");
    CHECK_FALSE(with_b.sources[0].cache->hit);

    // A47: the cursor made under A is dead after the switch, even after switching back.
    CHECK(c.search_error(owner, {"alpha"}, "song", cursor).code == ErrorCode::cursor_expired);
    REQUIRE(c.credentials->set_default(owner, "alpha", a, ports::SecretBytes{"a=1"}));
    CHECK(c.search_error(owner, {"alpha"}, "song", cursor).code == ErrorCode::cursor_expired);
    // Reactivation advances A's credential version, so old cache entries cannot return.
    const auto back = c.search(owner, {"alpha"});
    CHECK(back.items.at(0).title == "song@" + a + "#0");
    CHECK_FALSE(back.sources[0].cache->hit);

    // A fresh cursor continues under the same selection; it pages the same account.
    const auto fresh = *back.sources[0].page->next_cursor;
    const auto page_two = c.search(owner, {"alpha"}, "song", fresh);
    CHECK(page_two.items.at(0).title == "song@" + a + "#1");

    // A39: a new credential version expires cursors and starts a new partition.
    REQUIRE(c.credentials->update(owner, a, {ports::SecretBytes{"a=2"}, {}, {}}));
    CHECK(c.search_error(owner, {"alpha"}, "song", fresh).code == ErrorCode::cursor_expired);
    CHECK_FALSE(c.search(owner, {"alpha"}).sources[0].cache->hit);

    // Cursors bind the query and source; misuse is a mismatch, not a silent new search.
    const auto again = *c.search(owner, {"alpha"}).sources[0].page->next_cursor;
    CHECK(c.search_error(owner, {"alpha"}, "other", again).code == ErrorCode::cursor_mismatch);
    CHECK(c.search_error(owner, {"beta"}, "song", again).code == ErrorCode::cursor_mismatch);
    CHECK(c.search_error(owner, {"alpha", "beta"}, "song", again).code == ErrorCode::validation_failed);
    CHECK(c.search_error(owner, {"alpha"}, "song", std::string{"cur_unknown"}).code == ErrorCode::cursor_expired);
    c.clock->advance(601s); // lifetimes.cursor
    CHECK(c.search_error(owner, {"alpha"}, "song", again).code == ErrorCode::cursor_expired);
}

TEST_CASE("principals never share results or cursors", "[catalog]") {
    Catalog c{"principals"};
    const auto first = c.principal("first");
    const auto second = c.principal("second");
    const auto first_cookie = c.cookie(first, "alpha", "f=1");
    REQUIRE(c.credentials->set_default(first, "alpha", first_cookie));

    // A06: the second principal is anonymous here and gets its own result.
    const auto mine = c.search(first, {"alpha"});
    const auto theirs = c.search(second, {"alpha"});
    CHECK(mine.items.at(0).title == "song@" + first_cookie + "#0");
    CHECK(theirs.items.at(0).title == "song@anonymous#0");
    CHECK_FALSE(theirs.sources[0].cache->hit);
    const auto cursor = *mine.sources[0].page->next_cursor;
    CHECK(c.search_error(second, {"alpha"}, "song", cursor).code == ErrorCode::cursor_expired);
}

TEST_CASE("track details and share links", "[catalog]") {
    Catalog c{"details"};
    const auto owner = c.principal();

    const auto detail = unwrap(c.catalog->track_detail(owner, "alpha", "42"));
    CHECK(detail.track.title == "detail@anonymous");
    CHECK_FALSE(detail.cache.hit);
    CHECK(unwrap(c.catalog->track_detail(owner, "alpha", "42")).cache.hit);
    CHECK(c.alpha->details == 1);
    CHECK(c.catalog->track_detail(owner, "ghost", "1").error().code == ErrorCode::unknown_provider);
    CHECK(c.catalog->track_detail(owner, "off", "1").error().code == ErrorCode::provider_disabled);
    CHECK(c.catalog->track_detail(owner, "aud", "1").error().code == ErrorCode::capability_unsupported);

    const auto matched = unwrap(c.catalog->parse_link(owner, "听这首：https://alpha.example/song/7，很好听"));
    CHECK(matched.matched);
    CHECK(matched.kind == "track");
    CHECK(*matched.ref == track("alpha", "7"));
    // The same entity twice is one match; two different ones are refused.
    CHECK(unwrap(c.catalog->parse_link(owner, "https://alpha.example/song/7 https://alpha.example/song/7")).matched);
    CHECK(c.catalog->parse_link(owner, "https://alpha.example/song/7 https://beta.example/song/8").error().code ==
          ErrorCode::validation_failed);
    CHECK_FALSE(unwrap(c.catalog->parse_link(owner, "no links here")).matched);
    // Disabled sources do not parse.
    CHECK_FALSE(unwrap(c.catalog->parse_link(owner, "https://off.example/song/1")).matched);
    CHECK(c.catalog->parse_link(owner, std::string(4097, 'x')).error().code == ErrorCode::validation_failed);
}

TEST_CASE("resolve applies the quality and preview rules and binds the asset", "[catalog]") {
    Catalog c{"resolve"};
    const auto owner = c.principal();
    const auto other = c.principal("other");
    const auto song = track("alpha", "9");

    const auto asset = unwrap(c.assets->resolve({owner, song, {domain::Quality::high, domain::QualityPolicy::strict, false}}));
    CHECK(asset.asset_id.rfind("ast_", 0) == 0);
    CHECK(asset.original_track == song);
    CHECK(asset.actual_track == song);
    CHECK(asset.actual_quality == domain::Quality::high);
    CHECK(asset.completeness == domain::Completeness::full);
    CHECK(asset.warnings.empty());
    CHECK(asset.expires_at == c.clock->now() + 300s);

    // Known lower quality: strict refuses, allow_lower accepts with a warning.
    c.alpha->low_quality = true;
    CHECK(c.assets->resolve({owner, song, {domain::Quality::high, domain::QualityPolicy::strict, false}}).error().code ==
          ErrorCode::quality_unavailable);
    const auto lowered =
        unwrap(c.assets->resolve({owner, song, {domain::Quality::high, domain::QualityPolicy::allow_lower, false}}));
    CHECK(lowered.warnings == std::vector{domain::ResourceWarning::quality_lowered});
    c.alpha->low_quality = false;

    // Known previews only with allow_preview, and then marked.
    c.alpha->preview = true;
    CHECK(c.assets->resolve({owner, song, {}}).error().code == ErrorCode::preview_rejected);
    const auto preview = unwrap(c.assets->resolve({owner, song, {domain::Quality::standard, domain::QualityPolicy::allow_lower, true}}));
    CHECK(preview.completeness == domain::Completeness::preview);
    c.alpha->preview = false;

    // An address that is already stale is not a result.
    c.alpha->stale_address = true;
    const auto stale = c.assets->resolve({owner, song, {}});
    CHECK(stale.error().code == ErrorCode::upstream_failed);
    c.alpha->stale_address = false;

    // No same-source audio and no automatic fallback: no execution path.
    CHECK(c.assets->resolve({owner, track("meta", "1"), {}}).error().code == ErrorCode::capability_unsupported);

    // Assets belong to their owner.
    CHECK(c.assets->probe(other, asset.asset_id).error().code == ErrorCode::not_found);
}

TEST_CASE("probe reads a few bytes and expires with the asset or its account", "[catalog]") {
    Catalog c{"probe"};
    const auto owner = c.principal();
    const auto cookie = c.cookie(owner, "alpha", "p=1");
    REQUIRE(c.credentials->set_default(owner, "alpha", cookie));
    const auto asset = unwrap(c.assets->resolve({owner, track("alpha", "9"), {domain::Quality::high, domain::QualityPolicy::strict, false}}));

    const auto report = unwrap(c.assets->probe(owner, asset.asset_id));
    CHECK(report.accessible);
    CHECK(report.http_status == 206);
    CHECK(report.range_supported);
    CHECK(report.size_bytes == 8000000);
    CHECK(report.format == "mp3");
    CHECK(report.estimated_duration_ms == 200000); // 8 MB at 320 kbit/s
    CHECK(report.content_type == "audio/mpeg");
    CHECK(c.probe->chunks <= 4); // stops after the leading bytes
    // Range request, cookie only as a secret header, admitted for the actual source.
    CHECK(c.probe->last.url == "https://cdn.example/9.mp3");
    CHECK(std::any_of(c.probe->last.headers.begin(), c.probe->last.headers.end(),
                      [](const auto& h) { return h.name == "Range" && h.value == "bytes=0-15"; }));
    CHECK(c.probe->last.secret_headers.size() == 1);
    CHECK(c.probe->last_base.source.value() == "alpha");
    CHECK(c.probe->last_base.operation == "probe");

    // The account changed: the asset no longer stands for the current account.
    REQUIRE(c.credentials->update(owner, cookie, {ports::SecretBytes{"p=2"}, {}, {}}));
    CHECK(c.assets->probe(owner, asset.asset_id).error().code == ErrorCode::resource_expired);

    const auto fresh = unwrap(c.assets->resolve({owner, track("alpha", "9"), {domain::Quality::high, domain::QualityPolicy::strict, false}}));
    c.clock->advance(301s);
    CHECK(c.assets->probe(owner, fresh.asset_id).error().code == ErrorCode::resource_expired);
}
