// T28, the S2 extension gate, without network: the real music-service process with the
// real Kuwo and 5sing packages (and, for scale, 100 simulated sources next to them).
// Declared capabilities and the common routes agree for every collection and discovery
// operation; one failing source leaves the other's results; 100 sources are listed in
// pages while only the chosen ones are created, contacted or cost a thread; a disabled
// source fails its recovered tasks without switching sources while its published files
// stay readable; and an upgraded implementation re-resolves by the stable track ref,
// never continuing bytes or cursors from the old one.

#include "two_platforms.hpp"

#include "application/ports/providers/provider.hpp"

#include <cstdio>
#include <map>
#include <set>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#endif

using namespace music_service;
using namespace music_service::test;
using namespace music_service::test::two_platforms;

namespace {

using ErrorCode = domain::ErrorCode;

// Threads of this process, to show that sources do not bring threads of their own.
std::size_t process_threads() {
#ifdef _WIN32
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    REQUIRE(snapshot != INVALID_HANDLE_VALUE);
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    std::size_t count = 0;
    for (auto more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID == GetCurrentProcessId()) {
            ++count;
        }
    }
    CloseHandle(snapshot);
    return count;
#elif defined(__APPLE__)
    thread_act_array_t threads{};
    mach_msg_type_number_t count = 0;
    REQUIRE(task_threads(mach_task_self(), &threads, &count) == KERN_SUCCESS);
    for (mach_msg_type_number_t i = 0; i < count; ++i) {
        mach_port_deallocate(mach_task_self(), threads[i]);
    }
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads), sizeof(thread_t) * count);
    return count;
#else
    std::size_t count = 0;
    for ([[maybe_unused]] const auto& entry : fs::directory_iterator("/proc/self/task")) {
        ++count;
    }
    return count;
#endif
}

// The system may add or retire a few pool threads of its own (the Windows thread pool
// does), so counts are compared with this slack; a thread per source would add one for
// each of the 100 listed or 11 searched sources.
constexpr std::size_t thread_slack = 4;

// The thread count once it has stopped changing: started pools are complete.
std::size_t settled_threads() {
    auto last = process_threads();
    for (int stable = 0; stable < 5;) {
        std::this_thread::sleep_for(20ms);
        const auto now = process_threads();
        stable = now == last ? stable + 1 : 0;
        last = now;
    }
    return last;
}

// Answers every simulated source's search and records the hosts it was asked.
class SimulatedUpstream final : public ports::HttpTransport {
public:
    std::map<std::string, int> hosts() const {
        const std::lock_guard lock{mutex_};
        return hosts_;
    }

    domain::Result<ports::HttpResponse> request(const ports::HttpRequest& request, const ports::UpstreamLease&,
                                                const ports::OperationContext&) override {
        const auto start = request.url.find("://") + 3;
        const auto host = request.url.substr(start, request.url.find('/', start) - start);
        {
            const std::lock_guard lock{mutex_};
            ++hosts_[host];
        }
        return ports::HttpResponse{{200, {{"Content-Type", "text/plain"}, {"Content-Length", "2"}}, request.url}, "ok"};
    }

    domain::Result<ports::HttpResponseHead> receive(const ports::HttpRequest& request, const ports::UpstreamLease&,
                                                    const ports::OperationContext&, const ports::HeadConsumer&,
                                                    const ports::ChunkConsumer&) override {
        return ports::HttpResponseHead{404, {{"Content-Length", "0"}}, request.url};
    }

private:
    mutable std::mutex mutex_;
    std::map<std::string, int> hosts_;
};

// A simulated source's search that asks its own host through the shared upstream path.
class NetworkSearch final : public ports::ITrackSearch {
public:
    explicit NetworkSearch(const ports::ProviderSetup& setup) : source_(setup.source), upstream_(setup.upstream) {}

    domain::Result<ports::TrackSearchPage> search_tracks(const ports::ProviderContext& context,
                                                         const ports::TrackSearchQuery& query) override {
        ports::HttpRequest request;
        request.url = "https://" + source_.value() + ".invalid/search";
        request.operation = "search_tracks";
        ports::AdmissionRequest base{context.identity.binding().principal_id(),
                                     source_,
                                     request.operation,
                                     ports::TrafficClass::short_request,
                                     context.identity,
                                     {},
                                     {},
                                     context.operation.deadline};
        auto fetched = upstream_->fetch(std::move(request), base, context.operation);
        if (!fetched) {
            return std::move(fetched).error();
        }
        if (!std::holds_alternative<ports::HttpResponse>(fetched.value())) {
            return domain::Error{ErrorCode::service_busy, "upstream capacity is not available now", true, {}};
        }
        ports::TrackSearchPage result;
        result.page.items.push_back(
            domain::Track{{source_, unwrap(domain::PlatformId::parse("t1"))}, query.text, {}, {}, {}, {}});
        result.page.page = {query.limit, 1, 1, false};
        return result;
    }

private:
    domain::SourceId source_;
    std::shared_ptr<ports::UpstreamHttp> upstream_;
};

std::string simulated_name(int n) {
    char name[8];
    std::snprintf(name, sizeof name, "m%03d", n);
    return name;
}

// Catalog sources, every fourth with audio as well, searching over the network.
ports::ProviderBundle simulated(int n, const std::shared_ptr<FactoryCounters>& counters) {
    const auto name = simulated_name(n);
    auto bundle = n % 4 == 0 ? mock_combo(name, counters) : mock_catalog(name, counters);
    for (auto& binding : bundle.bindings) {
        if (binding.key() == "search_tracks") {
            binding = ports::CapabilityBinding::bind<ports::ITrackSearch>(counted<ports::ITrackSearch>(
                counters, [](const ports::ProviderSetup& setup) { return new NetworkSearch(setup); }));
        }
    }
    return bundle;
}

std::vector<std::string> listed_page(httplib::Client& api, const std::string& query, std::optional<std::string>& next) {
    const auto response = api.Get("/api/v1/providers" + query);
    REQUIRE(response->status == 200);
    const auto page = body(response)["data"];
    std::vector<std::string> sources;
    for (const auto& item : page["items"]) {
        sources.push_back(item["source"].get<std::string>());
    }
    const auto cursor = page["page"].value("next_cursor", json(nullptr));
    next = cursor.is_string() ? std::optional<std::string>{cursor.get<std::string>()} : std::nullopt;
    return sources;
}

// Every page of a listing; the size of each page in order.
std::vector<std::string> list_all(httplib::Client& api, const std::string& filter, std::vector<std::size_t>& sizes) {
    std::vector<std::string> all;
    std::optional<std::string> next;
    auto page = listed_page(api, "?limit=50" + filter, next);
    sizes.push_back(page.size());
    all.insert(all.end(), page.begin(), page.end());
    while (next) {
        page = listed_page(api, "?limit=50" + filter + "&cursor=" + *next, next);
        sizes.push_back(page.size());
        all.insert(all.end(), page.begin(), page.end());
    }
    return all;
}

int port_of(const std::string& url) { return std::stoi(url.substr(url.rfind(':') + 1)); }

std::string path_of(const std::string& url) {
    const auto scheme = url.find("://");
    return url.substr(url.find('/', scheme + 3));
}

// The published file read back through a media ticket.
std::string read_through_ticket(const RunningService& service, httplib::Client& api, const std::string& file_id) {
    const auto access = api.Post("/api/v1/media-access", R"({"target":{"kind":"file","id":")" + file_id + R"("}})",
                                 "application/json");
    REQUIRE(access->status == 200);
    httplib::Client media("127.0.0.1", port_of(*service.report.media_base_url));
    media.set_read_timeout(10s);
    const auto whole = media.Get(path_of(body(access)["data"]["url"].get<std::string>()));
    REQUIRE(whole);
    REQUIRE(whole->status == 200);
    return whole->body;
}

json provider_item(httplib::Client& api, const std::string& source) {
    const auto items = body(api.Get("/api/v1/providers"))["data"]["items"];
    const auto found = std::find_if(items.begin(), items.end(), [&](const json& item) { return item["source"] == source; });
    REQUIRE(found != items.end());
    return *found;
}

std::string accepted(const httplib::Result& created) {
    REQUIRE(created);
    INFO(created->body);
    REQUIRE(created->status == 202);
    return body(created)["data"]["task_id"].get<std::string>();
}

} // namespace

TEST_CASE("T28: 100 sources are listed in pages; only the chosen ones are created or contacted",
          "[gate][t28]") {
    Deployment d{"t28-scale"};
    std::vector<std::shared_ptr<FactoryCounters>> counters;
    const auto bundles = [&] {
        counters.clear();
        auto all = both();
        for (int n = 0; n < 100; ++n) {
            counters.push_back(std::make_shared<FactoryCounters>());
            all.push_back(simulated(n, counters.back()));
        }
        return all;
    };
    server::CommandHooks hooks;
    const auto created = [&] {
        int total = 0;
        for (const auto& c : counters) {
            total += c->created.load();
        }
        return total;
    };

    // The same deployment with only the two real packages, for the thread baseline.
    std::size_t baseline = 0;
    hooks.host.bundles = both();
    REQUIRE(d.run({"init"}, hooks).code == server::exit_ok);
    const auto token = d.read(d.bootstrap());
    {
        RunningService two{d, std::make_shared<infrastructure::observability::MemorySink>(), both(), false,
                           std::make_shared<SimulatedUpstream>()};
        auto api = two.client(token);
        REQUIRE(api.Get("/api/v1/providers")->status == 200);
        baseline = settled_threads();
    }

    // Then all 102, three of the simulated ones searched by default.
    d.config["catalog"] = {{"default_sources", {"m001", "m002", "m003"}}, {"max_sources_per_request", 8}};
    d.config["providers"] = json::object();
    for (int n = 1; n <= 3; ++n) {
        d.config["providers"][simulated_name(n)] = json::object();
    }
    d.write_config();

    auto upstream = std::make_shared<SimulatedUpstream>();
    RunningService service{d, std::make_shared<infrastructure::observability::MemorySink>(), bundles(), false,
                           upstream};
    auto api = service.client(token);
    CHECK(created() == 0); // starting creates no source objects
    CHECK(upstream->hosts().empty());

    // 102 sources in three pages, each once, in source order.
    std::vector<std::size_t> sizes;
    const auto all = list_all(api, "", sizes);
    CHECK(sizes == std::vector<std::size_t>{50, 50, 2});
    REQUIRE(all.size() == 102);
    CHECK(std::is_sorted(all.begin(), all.end()));
    CHECK(std::set<std::string>(all.begin(), all.end()).size() == 102);
    sizes.clear();
    const auto audio = list_all(api, "&capability=resolve_audio", sizes);
    CHECK(audio.size() == 27); // m000, m004, ... m096 and the two real ones
    CHECK(sizes == std::vector<std::size_t>{27});
    std::optional<std::string> next;
    CHECK(listed_page(api, "?limit=100", next).size() == 100);
    REQUIRE(next);
    CHECK(error_code(api.Get("/api/v1/providers?limit=101")) == "VALIDATION_FAILED");
    CHECK(error_code(api.Get("/api/v1/providers?limit=50&capability=resolve_audio&cursor=" + *next)) ==
          "CURSOR_MISMATCH");
    // Listing all of them instantiated nothing, contacted nobody and started no thread.
    CHECK(created() == 0);
    CHECK(upstream->hosts().empty());
    const auto listed_threads = settled_threads();
    INFO("threads: " << baseline << " with two sources, " << listed_threads << " with 102 listed");
    CHECK(listed_threads <= baseline + thread_slack);

    // A search without sources uses the configured default three, and only they are
    // created and contacted.
    const auto searched = api.Get("/api/v1/search/tracks?q=abc&limit=5");
    REQUIRE(searched->status == 200);
    const auto results = body(searched)["meta"]["source_results"];
    REQUIRE(results.size() == 3);
    for (const auto& result : results) {
        CHECK(result["status"] == "ok");
    }
    CHECK(upstream->hosts() == std::map<std::string, int>{{"m001.invalid", 1}, {"m002.invalid", 1}, {"m003.invalid", 1}});
    for (int n = 0; n < 100; ++n) {
        INFO(simulated_name(n));
        CHECK(counters[static_cast<std::size_t>(n)]->created == (n >= 1 && n <= 3 ? 1 : 0));
    }

    // More sources are searched in batches within the per-request limit; all of them at
    // once is refused before any source is touched.
    const auto all_at_once = api.Get("/api/v1/search/tracks?q=abc&sources=all");
    CHECK(all_at_once->status == 400);
    CHECK(error_code(all_at_once) == "VALIDATION_FAILED");
    std::string batch;
    for (int n = 10; n < 18; ++n) {
        batch += (batch.empty() ? "" : ",") + simulated_name(n);
    }
    const auto batched = api.Get("/api/v1/search/tracks?q=abc&limit=5&sources=" + batch);
    REQUIRE(batched->status == 200);
    CHECK(body(batched)["meta"]["source_results"].size() == 8);
    CHECK(upstream->hosts().size() == 11);
    CHECK(created() == 11);
    CHECK(api.Get("/api/v1/search/tracks?q=abc&sources=" + batch + ",m018")->status == 400);
    CHECK(upstream->hosts().size() == 11);

    // The searches ran on the shared pool: the thread count did not grow with sources.
    CHECK(settled_threads() <= listed_threads + thread_slack);
}

namespace {

// One capability per collection or discovery route, with ids valid at each source.
struct RouteCase {
    const char* key;
    std::string kuwo;
    std::string fivesing;
};

const std::vector<RouteCase>& route_cases() {
    static const std::vector<RouteCase> cases{
        {"search_tracks", "/api/v1/search/tracks?q=x&sources=kuwo", "/api/v1/search/tracks?q=x&sources=fivesing"},
        {"search_playlists", "/api/v1/search/playlists?q=x&sources=kuwo",
         "/api/v1/search/playlists?q=x&sources=fivesing"},
        {"search_albums", "/api/v1/search/albums?q=x&sources=kuwo", "/api/v1/search/albums?q=x&sources=fivesing"},
        {"search_artists", "/api/v1/search/artists?q=x&sources=kuwo", "/api/v1/search/artists?q=x&sources=fivesing"},
        {"artist_detail", "/api/v1/artists/detail?source=kuwo&id=336", "/api/v1/artists/detail?source=fivesing&id=336"},
        {"track_detail", "/api/v1/tracks/detail?source=kuwo&id=78932517",
         "/api/v1/tracks/detail?source=fivesing&id=" + work},
        {"lyrics", "/api/v1/lyrics?source=kuwo&id=78932517", "/api/v1/lyrics?source=fivesing&id=" + work},
        {"playlist_detail", "/api/v1/playlists/detail?source=kuwo&id=2234580349",
         "/api/v1/playlists/detail?source=fivesing&id=59b6bcf8da767f5ace4bb449"},
        {"playlist_tracks", "/api/v1/playlists/tracks?source=kuwo&id=2234580349",
         "/api/v1/playlists/tracks?source=fivesing&id=59b6bcf8da767f5ace4bb449"},
        {"album_detail", "/api/v1/albums/detail?source=kuwo&id=1293", "/api/v1/albums/detail?source=fivesing&id=1293"},
        {"album_tracks", "/api/v1/albums/tracks?source=kuwo&id=1293", "/api/v1/albums/tracks?source=fivesing&id=1293"},
        {"artist_tracks", "/api/v1/artists/tracks?source=kuwo&id=1", "/api/v1/artists/tracks?source=fivesing&id=1"},
        {"artist_albums", "/api/v1/artists/albums?source=kuwo&id=1", "/api/v1/artists/albums?source=fivesing&id=1"},
        {"chart_detail", "/api/v1/charts/detail?source=kuwo&id=16", "/api/v1/charts/detail?source=fivesing&id=16"},
        {"chart_tracks", "/api/v1/charts/tracks?source=kuwo&id=16", "/api/v1/charts/tracks?source=fivesing&id=16"},
        {"charts", "/api/v1/charts?source=kuwo", "/api/v1/charts?source=fivesing"},
        {"categories", "/api/v1/categories?source=kuwo", "/api/v1/categories?source=fivesing"},
        {"category_playlists", "/api/v1/categories/playlists?source=kuwo&category_id=1",
         "/api/v1/categories/playlists?source=fivesing&category_id=1"},
        {"recommend_playlists", "/api/v1/recommendations/playlists?source=kuwo",
         "/api/v1/recommendations/playlists?source=fivesing"},
    };
    return cases;
}

// Whether the route answered "this source does not have the capability": 422 for a
// single-source call, or an unsupported per-source result for a search.
bool unsupported(const httplib::Result& response) {
    REQUIRE(response);
    const auto answer = json::parse(response->body);
    if (response->status == 200 && answer.contains("meta") && answer["meta"].contains("source_results")) {
        const auto& results = answer["meta"]["source_results"];
        REQUIRE(results.size() == 1);
        return results[0]["status"] == "unsupported";
    }
    return response->status == 422 && answer["error"]["code"] == "CAPABILITY_UNSUPPORTED";
}

} // namespace

TEST_CASE("T28: declared capabilities match the routes; a failing source leaves the other's results",
          "[gate][t28]") {
    Deployment d{"t28-contract"};
    server::CommandHooks hooks;
    hooks.host.bundles = both();
    REQUIRE(d.run({"init"}, hooks).code == server::exit_ok);
    const auto token = d.read(d.bootstrap());
    auto upstream = std::make_shared<Upstream>(mp3(800), mp3(6738));
    RunningService service{d, std::make_shared<infrastructure::observability::MemorySink>(), both(), false, upstream};
    auto api = service.client(token);

    const auto providers = body(api.Get("/api/v1/providers"))["data"]["items"];
    REQUIRE(providers.size() == 2);
    std::set<std::string> covered{"parse_links", "resolve_audio"}; // not routed per source
    for (const auto& route : route_cases()) {
        covered.insert(route.key);
    }
    for (const auto& provider : providers) {
        const auto source = provider["source"].get<std::string>();
        std::set<std::string> declared;
        for (const auto& [key, spec] : provider["capabilities"].items()) {
            declared.insert(key);
            INFO(source << " declares " << key);
            CHECK(covered.count(key) == 1); // every declared capability has a route below
        }
        for (const auto& route : route_cases()) {
            INFO(source << " " << route.key);
            const auto before = upstream->requests().size();
            const auto response = api.Get(source == "kuwo" ? route.kuwo : route.fivesing);
            if (declared.count(route.key) == 1) {
                CHECK_FALSE(unsupported(response));
            } else {
                CHECK(unsupported(response));
                CHECK(upstream->requests().size() == before); // refused before any request
            }
        }
        const auto resolved = api.Post("/api/v1/assets/resolve",
                                       R"({"track":{"source":")" + source + R"(","id":")" +
                                           (source == "kuwo" ? std::string{"78932517"} : work) + R"("}})",
                                       "application/json");
        CHECK(resolved->status == (declared.count("resolve_audio") == 1 ? 200 : 422));
    }

    // A05 with the real packages: Kuwo's search fails, 5sing's results still arrive.
    const std::string search = "/api/v1/search/tracks?q=%E5%8D%83%E5%B9%B4&sources=kuwo,fivesing&limit=5";
    upstream->fail_kuwo_search(true);
    const auto partial = api.Get(search);
    REQUIRE(partial->status == 200);
    const auto envelope = body(partial);
    CHECK(envelope["meta"]["partial"] == true);
    std::map<std::string, json> by_source;
    for (const auto& result : envelope["meta"]["source_results"]) {
        by_source[result["source"].get<std::string>()] = result;
    }
    REQUIRE(by_source.size() == 2);
    CHECK(by_source["fivesing"]["status"] == "ok");
    CHECK(by_source["kuwo"]["status"] == "failed");
    INFO(by_source["kuwo"].dump());
    CHECK(by_source["kuwo"]["error"]["retryable"] == true);
    REQUIRE_FALSE(envelope["data"]["items"].empty());
    for (const auto& item : envelope["data"]["items"]) {
        CHECK(item["ref"]["source"] == "fivesing");
    }
    // The failure was not cached: the next search asks Kuwo again and gets both.
    upstream->fail_kuwo_search(false);
    const auto whole = body(api.Get(search));
    CHECK(whole["meta"]["partial"] == false);
    for (const auto& result : whole["meta"]["source_results"]) {
        CHECK(result["status"] == "ok");
    }
}

TEST_CASE("T28: a disabled source fails its recovered tasks without switching; its files stay readable",
          "[gate][t28]") {
    Deployment d{"t28-disabled"};
    server::CommandHooks hooks;
    hooks.host.bundles = both();
    REQUIRE(d.run({"init"}, hooks).code == server::exit_ok);
    const auto token = d.read(d.bootstrap());
    const auto five_audio = mp3(800);
    const auto kuwo_audio = mp3(6738);
    auto upstream = std::make_shared<Upstream>(five_audio, kuwo_audio);
    const auto sink = std::make_shared<infrastructure::observability::MemorySink>();
    const std::string five_track = R"({"track":{"source":"fivesing","id":"fc-16394286"}})";

    // A 5sing file is published while the source is enabled.
    std::string file_id;
    std::string stored;
    {
        RunningService service{d, sink, both(), false, upstream};
        auto api = service.client(token);
        const auto task_id = accepted(
            api.Post("/api/v1/downloads", {{"Idempotency-Key", "t28-disabled-0001"}}, five_track, "application/json"));
        const auto done = wait_status(api, task_id, "completed");
        REQUIRE(done["status"] == "completed");
        file_id = done["result"]["file_id"].get<std::string>();
        stored = read_through_ticket(service, api, file_id);
        CHECK(without_id3v2(stored) == five_audio);
    }

    // Two more tasks are accepted while downloads are paused, then the service stops.
    std::string queued_five;
    std::string queued_kuwo;
    {
        RunningService service{d, sink, both(), true, upstream};
        auto api = service.client(token);
        queued_five = accepted(api.Post("/api/v1/downloads", {{"Idempotency-Key", "t28-disabled-0002"}},
                                        R"({"track":{"source":"fivesing","id":"fc-16394286"},"duplicate_policy":"force"})",
                                        "application/json"));
        queued_kuwo = accepted(api.Post("/api/v1/downloads", {{"Idempotency-Key", "t28-disabled-0003"}},
                                        R"({"track":{"source":"kuwo","id":"78932517"}})", "application/json"));
        CHECK(body(api.Get("/api/v1/downloads/" + queued_five))["data"]["status"] == "queued");
    }

    // 5sing is disabled by configuration; the same build still has its package.
    d.config["providers"] = {{"fivesing", {{"enabled", false}}}};
    d.write_config();
    upstream->clear();
    RunningService service{d, sink, both(), false, upstream};
    auto api = service.client(token);

    // The recovered 5sing task ends with the source's state, on its own source.
    const auto failed = wait_status(api, queued_five, "failed");
    REQUIRE(failed["status"] == "failed");
    INFO(failed.dump());
    CHECK(failed["error"]["code"] == "PROVIDER_DISABLED");
    CHECK(failed["error"]["retryable"] == false);
    CHECK(failed["original_track"]["source"] == "fivesing");
    CHECK((failed["actual_track"].is_null() || failed["actual_track"]["source"] == "fivesing"));
    CHECK(failed["result"].is_null());
    // The Kuwo task is not held up and nothing went to 5sing or its CDN in its place.
    const auto kuwo_done = wait_status(api, queued_kuwo, "completed");
    CHECK(kuwo_done["status"] == "completed");
    for (const auto& request : upstream->requests()) {
        INFO(request.url);
        CHECK(request.url.find("5sing") == std::string::npos);
        CHECK(request.url.find("kugou.com") == std::string::npos);
    }
    CHECK(upstream->audio_gets(kuwo_cdn) == 1);

    // The file published before stays listed and readable, byte for byte.
    const auto file = api.Get("/api/v1/files/" + file_id);
    REQUIRE(file->status == 200);
    CHECK(body(file)["data"]["display_name"] == "琴酒蜀黍 - 千年食谱颂.mp3");
    CHECK(read_through_ticket(service, api, file_id) == stored);

    // The source is listed as disabled and every new call says so.
    CHECK(provider_item(api, "fivesing")["enabled"] == false);
    std::optional<std::string> next;
    CHECK(listed_page(api, "?enabled=true", next) == std::vector<std::string>{"kuwo"});
    CHECK(listed_page(api, "?enabled=false", next) == std::vector<std::string>{"fivesing"});
    const auto resolved =
        api.Post("/api/v1/assets/resolve", R"({"track":{"source":"fivesing","id":"fc-16394286"}})", "application/json");
    CHECK(resolved->status == 422);
    CHECK(error_code(resolved) == "PROVIDER_DISABLED");
    CHECK(error_code(api.Get("/api/v1/lyrics?source=fivesing&id=" + work)) == "PROVIDER_DISABLED");
    CHECK(error_code(api.Get("/api/v1/playlists/detail?source=fivesing&id=59b6bcf8da767f5ace4bb449")) ==
          "PROVIDER_DISABLED");
    const auto searched = api.Get("/api/v1/search/tracks?q=%E5%8D%83%E5%B9%B4&sources=kuwo,fivesing&limit=3");
    REQUIRE(searched->status == 200);
    const auto per_source = body(searched)["meta"]["source_results"];
    REQUIRE(per_source.size() == 2);
    for (const auto& result : per_source) {
        CHECK(result["status"] == (result["source"] == "kuwo" ? "ok" : "disabled"));
    }
    const auto forced = api.Post("/api/v1/downloads", {{"Idempotency-Key", "t28-disabled-0004"}},
                                 R"({"track":{"source":"fivesing","id":"fc-16394286"},"duplicate_policy":"force"})",
                                 "application/json");
    REQUIRE(forced);
    INFO(forced->body);
    CHECK(forced->status == 422);
    CHECK(error_code(forced) == "PROVIDER_DISABLED");
    for (const auto& request : upstream->requests()) {
        CHECK(request.url.find("5sing") == std::string::npos);
    }
}

TEST_CASE("T28: an upgraded implementation re-resolves by the track ref and restarts the bytes", "[gate][t28]") {
    Deployment d{"t28-upgrade"};
    server::CommandHooks hooks;
    hooks.host.bundles = both();
    REQUIRE(d.run({"init"}, hooks).code == server::exit_ok);
    const auto token = d.read(d.bootstrap());
    const auto kuwo_audio = mp3(6738);
    auto upstream = std::make_shared<Upstream>(mp3(800), kuwo_audio);
    const auto sink = std::make_shared<infrastructure::observability::MemorySink>();

    // A Kuwo transfer is cut by a shutdown after its first bytes; a playlist cursor is
    // handed out by the same implementation.
    std::string task_id;
    std::string cursor;
    std::string old_version;
    {
        RunningService service{d, sink, both(), false, upstream};
        auto api = service.client(token);
        old_version = provider_item(api, "kuwo")["implementation"]["version"].get<std::string>();
        const auto page = body(api.Get("/api/v1/playlists/tracks?source=kuwo&id=2234580349&limit=2"))["data"];
        cursor = page["page"]["next_cursor"].get<std::string>();
        upstream->hold(true);
        task_id = accepted(api.Post("/api/v1/downloads", {{"Idempotency-Key", "t28-upgrade-0001"}},
                                    R"({"track":{"source":"kuwo","id":"78932517"}})", "application/json"));
        REQUIRE(upstream->wait_holding());
        CHECK(body(api.Get("/api/v1/downloads/" + task_id))["data"]["status"] == "downloading");
    }

    // The next start has a newer Kuwo implementation.
    auto upgraded = both();
    upgraded[0].implementation_version += "-t28";
    REQUIRE(upgraded[0].source.value() == "kuwo");
    upstream->hold(false);
    upstream->clear();
    RunningService service{d, sink, upgraded, false, upstream};
    auto api = service.client(token);
    CHECK(provider_item(api, "kuwo")["implementation"]["version"] == old_version + "-t28");

    // The task is resolved again from its stable ref and downloaded from the first byte.
    const auto done = wait_status(api, task_id, "completed");
    REQUIRE(done["status"] == "completed");
    CHECK(done["actual_track"]["source"] == "kuwo");
    CHECK(done["actual_track"]["id"] == "78932517");
    CHECK(upstream->api_requests("https://mobi.kuwo.cn/mobi.s?") == 1); // a fresh address
    std::vector<ports::HttpRequest> transfers;
    for (const auto& request : upstream->requests()) {
        if (request.url.rfind(kuwo_cdn, 0) == 0) {
            transfers.push_back(request);
        }
    }
    REQUIRE(transfers.size() == 1);
    const auto range = request_header(transfers[0], "Range");
    CHECK((!range || *range == "bytes=0-"));
    CHECK(without_id3v2(read_through_ticket(service, api, done["result"]["file_id"].get<std::string>())) ==
          kuwo_audio);

    // Catalog context from the old implementation is not carried over either.
    CHECK(error_code(api.Get("/api/v1/playlists/tracks?source=kuwo&id=2234580349&limit=2&cursor=" + cursor)) ==
          "CURSOR_EXPIRED");
}
