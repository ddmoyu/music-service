// T22 without network: the real music-service process with the real 5sing and Kuwo
// packages side by side, upstream answers from the recorded fixtures and a CDN serving
// an MP3 at the tier's primary and backup addresses. 5sing search, detail, lyrics and
// resolve; a durable download through the shared runner to a tagged file; the backup
// address used only when the primary does not answer; and the capability differences
// between the two sources answered by the common routes. The online run is
// packaging/e2e/kuwo_e2e.py --source fivesing. The [t41] case is the second-platform
// gate: two real packages, two principals, one runner and no platform branches.

#include "service_harness.hpp"
#include "sse_client.hpp"

#include "providers/fivesing/bundle.hpp"
#include "providers/kuwo/bundle.hpp"

#include <fstream>
#include <map>
#include <mutex>
#include <set>

using namespace music_service;
using namespace music_service::test;
namespace fs = std::filesystem;

namespace {

const std::string work = "fc-16394286";
const std::string primary_host = "https://wsaudiobssdlbig.kugou.com/";
const std::string backup_host = "https://wsaudiobssdlbig.cloud.kugou.com/";
const std::string lq_file = "23e71848fb13b3d1aa8ab7c19bace467.mp3";
const std::string kuwo_cdn = "https://kw-lv.kuwo.cn/";

std::string fixture(const char* platform, const char* name) {
    std::ifstream file(fs::path{MUSIC_SERVICE_TEST_FIXTURES} / "providers" / platform / name, std::ios::binary);
    REQUIRE(file);
    return {std::istreambuf_iterator<char>(file), {}};
}

// 128 kbit/s MPEG-1 Layer III frames at 44.1 kHz.
std::string mp3(std::size_t frames) {
    std::string bytes;
    for (std::size_t i = 0; i < frames; ++i) {
        const auto padding = i % 3 == 0;
        std::string frame(static_cast<std::size_t>(144 * 128 * 1000 / 44100 + (padding ? 1 : 0)), '\x55');
        frame[0] = '\xFF';
        frame[1] = '\xFB';
        frame[2] = static_cast<char>((9U << 4) | (padding ? 0x02U : 0U));
        frame[3] = '\x40';
        bytes += frame;
    }
    return bytes;
}

std::string without_id3v2(const std::string& bytes) {
    if (bytes.size() < 10 || bytes.compare(0, 3, "ID3") != 0) {
        return bytes;
    }
    const auto size = (std::size_t{static_cast<unsigned char>(bytes[6]) & 0x7FU} << 21) |
                      (std::size_t{static_cast<unsigned char>(bytes[7]) & 0x7FU} << 14) |
                      (std::size_t{static_cast<unsigned char>(bytes[8]) & 0x7FU} << 7) |
                      std::size_t{static_cast<unsigned char>(bytes[9]) & 0x7FU};
    return bytes.substr(std::min(bytes.size(), 10 + size));
}

// 5sing and Kuwo as the fixtures show them. The getSongUrl answer for fc-16394286
// declares the lq file with the size of the synthetic MP3; the CDN serves it under that
// file name at both hosts, Last-Modified as the only validator, as the real CDN does.
// fail_primary makes the primary host answer 503. Kuwo answers for rid 78932517 with
// its own MP3 (the 176 s it declares) behind a strong ETag.
class Upstream final : public ports::HttpTransport {
public:
    explicit Upstream(std::string audio, std::string kuwo_audio = {})
        : audio_(std::move(audio)), kuwo_audio_(std::move(kuwo_audio)) {}

    // Audio bodies requested so far under a CDN prefix.
    std::size_t audio_gets(const std::string& prefix) const {
        const std::lock_guard lock{mutex_};
        return static_cast<std::size_t>(std::count_if(requests_.begin(), requests_.end(), [&](const auto& r) {
            return r.method == ports::HttpMethod::get && r.url.rfind(prefix, 0) == 0;
        }));
    }
    std::size_t api_requests(const std::string& prefix) const {
        const std::lock_guard lock{mutex_};
        return static_cast<std::size_t>(std::count_if(requests_.begin(), requests_.end(),
                                                      [&](const auto& r) { return r.url.rfind(prefix, 0) == 0; }));
    }

    void fail_primary(bool on) {
        const std::lock_guard lock{mutex_};
        fail_primary_ = on;
    }
    std::vector<ports::HttpRequest> requests() const {
        const std::lock_guard lock{mutex_};
        return requests_;
    }
    void clear() {
        const std::lock_guard lock{mutex_};
        requests_.clear();
    }

    domain::Result<ports::HttpResponse> request(const ports::HttpRequest& request, const ports::UpstreamLease&,
                                                const ports::OperationContext&) override {
        record(request);
        const auto& url = request.url;
        if (url.rfind("http://search.5sing.kugou.com/home/json?", 0) == 0) {
            return answer(200, fixture("fivesing", "search-qiannianjiu-p1.json"), url);
        }
        if (url == "http://mobileapi.5sing.kugou.com/song/newget?songid=16394286&songtype=fc") {
            return answer(200, fixture("fivesing", "newget-fc-16394286.json"), url);
        }
        if (url.rfind("http://mobileapi.5sing.kugou.com/song/newget?", 0) == 0) {
            return answer(200, fixture("fivesing", "newget-missing.json"), url);
        }
        if (url == "http://mobileapi.5sing.kugou.com/song/getSongUrl?songid=16394286&songtype=fc") {
            auto body = fixture("fivesing", "url-fc-16394286.json");
            const std::string declared = R"("lqsize":"5324844")";
            body.replace(body.find(declared), declared.size(), R"("lqsize":")" + std::to_string(audio_.size()) + "\"");
            return answer(200, std::move(body), url);
        }
        if (url.rfind("http://mobileapi.5sing.kugou.com/song/getSongUrl?", 0) == 0) {
            return answer(200, fixture("fivesing", "url-missing.json"), url);
        }
        if (url.rfind("https://www.kuwo.cn/search/searchMusicBykeyWord?", 0) == 0) {
            return answer(200, fixture("kuwo", "search-qingtian-pn0-rn3.json"), url);
        }
        if (url == "https://www.kuwo.cn/play_detail/78932517") {
            return answer(200, fixture("kuwo", "play-detail-78932517.html"), url);
        }
        if (url.rfind("https://mobi.kuwo.cn/mobi.s?", 0) == 0) {
            const auto found = url.find("&rid=78932517&") != std::string::npos;
            return answer(200, fixture("kuwo", found ? "audio-78932517-standard.json" : "audio-missing.json"), url);
        }
        if (request.method == ports::HttpMethod::head) {
            auto head = cdn_head(url);
            return ports::HttpResponse{std::move(head), {}};
        }
        return answer(404, "not found", url);
    }

    domain::Result<ports::HttpResponseHead> receive(const ports::HttpRequest& request, const ports::UpstreamLease&,
                                                    const ports::OperationContext&, const ports::HeadConsumer& on_head,
                                                    const ports::ChunkConsumer& on_chunk) override {
        record(request);
        const bool kuwo = request.url.rfind(kuwo_cdn, 0) == 0;
        auto head = kuwo ? ports::HttpResponseHead{200,
                                                   {{"Content-Type", "audio/mpeg"},
                                                    {"Content-Length", std::to_string(kuwo_audio_.size())},
                                                    {"ETag", "\"kw-78932517\""}},
                                                   request.url}
                         : cdn_head(request.url);
        if (!on_head(head) || head.status != 200) {
            return head;
        }
        const auto& bytes = kuwo ? kuwo_audio_ : audio_;
        for (std::size_t sent = 0; sent < bytes.size();) {
            const auto size = std::min<std::size_t>(16 * 1024, bytes.size() - sent);
            if (!on_chunk(std::span<const char>{bytes.data() + sent, size})) {
                head.stopped_by_consumer = true;
                return head;
            }
            sent += size;
        }
        return head;
    }

private:
    ports::HttpResponseHead cdn_head(const std::string& url) {
        const std::lock_guard lock{mutex_};
        const bool primary = url.rfind(primary_host, 0) == 0;
        const bool known = (primary || url.rfind(backup_host, 0) == 0) && url.ends_with("/" + lq_file);
        if (!known) {
            return {404, {{"Content-Length", "0"}}, url};
        }
        if (primary && fail_primary_) {
            return {503, {{"Content-Length", "0"}}, url};
        }
        return {200,
                {{"Content-Type", "audio/mpeg"},
                 {"Content-Length", std::to_string(audio_.size())},
                 {"Last-Modified", "Sun, 18 Nov 2018 03:51:43 GMT"},
                 {"Accept-Ranges", "bytes"}},
                url};
    }

    void record(const ports::HttpRequest& request) {
        const std::lock_guard lock{mutex_};
        requests_.push_back(request);
    }

    static ports::HttpResponse answer(int status, std::string body, const std::string& url) {
        return {{status,
                 {{"Content-Type", "application/json; charset=utf-8"}, {"Content-Length", std::to_string(body.size())}},
                 url},
                std::move(body)};
    }

    std::string audio_;
    std::string kuwo_audio_;
    mutable std::mutex mutex_;
    std::vector<ports::HttpRequest> requests_;
    bool fail_primary_{false};
};

std::vector<ports::ProviderBundle> both() { return {providers::kuwo::make_bundle(), providers::fivesing::make_bundle()}; }

json wait_status(httplib::Client& api, const std::string& task_id, std::string_view status) {
    const auto until = std::chrono::steady_clock::now() + 30s;
    json task;
    while (std::chrono::steady_clock::now() < until) {
        task = body(api.Get("/api/v1/downloads/" + task_id))["data"];
        if (task["status"] == std::string{status} || task["status"] == "failed") {
            return task;
        }
        std::this_thread::sleep_for(20ms);
    }
    FAIL("task " << task_id << " stayed " << task.dump());
    return task;
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

std::string code_of(const httplib::Result& response) { return body(response)["error"]["code"].get<std::string>(); }

} // namespace

TEST_CASE("T22: 5sing next to Kuwo, from search to a published file", "[gate][fivesing]") {
    Deployment d{"fivesing-gate"};
    d.write_config();
    server::CommandHooks hooks;
    hooks.host.bundles = both();
    REQUIRE(d.run({"init"}, hooks).code == server::exit_ok);
    const auto token = d.read(d.bootstrap());
    const auto root = d.dir / "下载";

    const auto audio = mp3(800);
    auto upstream = std::make_shared<Upstream>(audio);
    RunningService service{d, std::make_shared<infrastructure::observability::MemorySink>(), both(), false, upstream};
    auto api = service.client(token);

    // Both sources are registered with their own capabilities.
    const auto listed = body(api.Get("/api/v1/providers"))["data"]["items"];
    REQUIRE(listed.size() == 2);
    const auto five = std::find_if(listed.begin(), listed.end(), [](const json& p) { return p["source"] == "fivesing"; });
    REQUIRE(five != listed.end());
    CHECK((*five)["name"] == "5sing");
    const auto& capabilities = (*five)["capabilities"];
    const auto has = [&](const char* key) { return capabilities.contains(key); };
    CHECK(has("search_tracks"));
    CHECK(has("lyrics"));
    CHECK(has("resolve_audio"));
    CHECK_FALSE(has("search_playlists"));

    // One search over both sources: each answers on its own.
    const auto search = api.Get("/api/v1/search/tracks?q=%E5%8D%83%E5%B9%B4%E9%85%92&sources=kuwo,fivesing&limit=5");
    REQUIRE(search->status == 200);
    const auto envelope = body(search);
    const auto& items = envelope["data"]["items"];
    std::set<std::string> sources;
    for (const auto& item : items) {
        sources.insert(item["ref"]["source"].get<std::string>());
    }
    CHECK(sources == std::set<std::string>{"fivesing", "kuwo"});
    CHECK(envelope["meta"]["partial"] == false);
    REQUIRE(envelope["meta"]["source_results"].size() == 2);
    for (const auto& result : envelope["meta"]["source_results"]) {
        INFO(result.dump());
        CHECK(result["status"] == "ok");
    }
    const auto cover = std::find_if(items.begin(), items.end(), [](const json& t) { return t["ref"]["id"] == work; });
    REQUIRE(cover != items.end());
    CHECK((*cover)["title"] == "千年食谱颂");
    CHECK((*cover)["duration_ms"].is_null()); // unknown stays unknown
    CHECK((*cover)["version"]["kind"] == "cover");

    // Detail, lyrics and capability differences through the same routes.
    const auto detail = body(api.Get("/api/v1/tracks/detail?source=fivesing&id=" + work))["data"];
    CHECK(detail["artists"][0]["name"] == "琴酒蜀黍");
    const auto lyrics = api.Get("/api/v1/lyrics?source=fivesing&id=" + work);
    REQUIRE(lyrics->status == 200);
    CHECK(body(lyrics)["data"]["lrc"].get<std::string>().find("[00:03.37]") != std::string::npos);
    const auto kuwo_lyrics = api.Get("/api/v1/lyrics?source=kuwo&id=78932517");
    CHECK(kuwo_lyrics->status == 422);
    CHECK(code_of(kuwo_lyrics) == "CAPABILITY_UNSUPPORTED");
    CHECK(api.Get("/api/v1/tracks/detail?source=fivesing&id=fc-999999999")->status == 404);
    CHECK(api.Get("/api/v1/tracks/detail?source=fivesing&id=16394286")->status == 400); // no type: no work

    const auto resolved =
        api.Post("/api/v1/assets/resolve", R"({"track":{"source":"fivesing","id":"fc-16394286"}})", "application/json");
    REQUIRE(resolved->status == 200);
    const auto asset = body(resolved)["data"];
    CHECK(asset["actual_quality"] == "standard");
    CHECK(asset["bitrate_bps"] == 128000);
    CHECK(asset["completeness"] == "full");
    CHECK(asset["warnings"].empty());
    const auto high = api.Post("/api/v1/assets/resolve",
                               R"({"track":{"source":"fivesing","id":"fc-16394286"},"quality":"high"})",
                               "application/json");
    CHECK(high->status == 422); // this work has no sq tier
    CHECK(code_of(high) == "QUALITY_UNAVAILABLE");

    // The shared runner downloads it into a tagged file named from the detail.
    upstream->clear();
    const auto created = api.Post("/api/v1/downloads", {{"Idempotency-Key", "fivesing-gate-0001"}},
                                  R"({"track":{"source":"fivesing","id":"fc-16394286"}})", "application/json");
    REQUIRE(created->status == 202);
    const auto task_id = body(created)["data"]["task_id"].get<std::string>();
    const auto task = wait_status(api, task_id, "completed");
    REQUIRE(task["status"] == "completed");
    CHECK(task["actual_track"]["id"] == work);
    CHECK(task["actual_quality"] == "standard");
    const auto file = body(api.Get("/api/v1/files/" + task["result"]["file_id"].get<std::string>()))["data"];
    CHECK(file["display_name"] == "琴酒蜀黍 - 千年食谱颂.mp3");
    CHECK(file["metadata_status"] == "complete");
    const auto stored = read_file(root / infrastructure::filesystem::path_from_utf8("琴酒蜀黍 - 千年食谱颂.mp3"));
    CHECK(without_id3v2(stored) == audio);

    // Plaintext only to the registered API endpoints, never with a secret; audio from
    // the primary over HTTPS.
    std::size_t audio_gets = 0;
    for (const auto& request : upstream->requests()) {
        INFO(request.url);
        CHECK(request.secret_headers.empty());
        if (request.url.rfind("http://", 0) == 0) {
            CHECK((request.url.rfind("http://mobileapi.5sing.kugou.com/song/", 0) == 0 ||
                   request.url.rfind("http://search.5sing.kugou.com/home/json?", 0) == 0));
        } else if (request.method == ports::HttpMethod::get) {
            CHECK(request.url.rfind(primary_host, 0) == 0);
            ++audio_gets;
        }
    }
    CHECK(audio_gets == 1);

    // The primary down: the same file comes from the backup.
    upstream->fail_primary(true);
    upstream->clear();
    const auto forced = api.Post("/api/v1/downloads", {{"Idempotency-Key", "fivesing-gate-0002"}},
                                 R"({"track":{"source":"fivesing","id":"fc-16394286"},"duplicate_policy":"force"})",
                                 "application/json");
    REQUIRE(forced->status == 202);
    const auto second = wait_status(api, body(forced)["data"]["task_id"].get<std::string>(), "completed");
    REQUIRE(second["status"] == "completed");
    const auto second_file = body(api.Get("/api/v1/files/" + second["result"]["file_id"].get<std::string>()))["data"];
    CHECK(second_file["size_bytes"] == file["size_bytes"]);
    std::vector<std::string> fetched;
    for (const auto& request : upstream->requests()) {
        if (request.url.rfind("https://", 0) == 0 && request.method == ports::HttpMethod::get) {
            fetched.push_back(request.url);
        }
    }
    REQUIRE(fetched.size() == 1);
    CHECK(fetched[0].rfind(backup_host, 0) == 0);
    CHECK(fetched[0].ends_with("/" + lq_file)); // the very file the primary names
}

namespace {

std::string create_principal(Deployment& d, const server::CommandHooks& hooks, const std::string& name) {
    const auto created = d.run({"principal", "create", "--name", name}, hooks);
    REQUIRE(created.code == server::exit_ok);
    return created.out.substr(0, created.out.find('\t'));
}

std::string issue(Deployment& d, const server::CommandHooks& hooks, const std::string& principal,
                  const std::string& file) {
    const auto output = d.dir / "out" / file;
    REQUIRE(d.run({"token", "issue", "--principal", principal, "--scopes",
                   "catalog:read,downloads:read,downloads:write,files:read", "--output",
                   infrastructure::filesystem::path_to_utf8(output)},
                  hooks)
                .code == server::exit_ok);
    return d.read(output);
}

std::set<std::string> listed_sources(httplib::Client& api, const std::string& query) {
    std::set<std::string> sources;
    const auto listed = body(api.Get("/api/v1/providers" + query));
    for (const auto& item : listed["data"]["items"]) {
        sources.insert(item["source"].get<std::string>());
    }
    return sources;
}

// The cache hit of each source in a search's per-source results.
std::map<std::string, bool> cache_hits(httplib::Client& api, const std::string& path) {
    const auto response = api.Get(path);
    REQUIRE(response->status == 200);
    std::map<std::string, bool> hits;
    const auto searched = body(response);
    for (const auto& result : searched["meta"]["source_results"]) {
        CHECK(result["status"] == "ok");
        hits[result["source"].get<std::string>()] = !result["cache"].is_null() && result["cache"]["hit"] == true;
    }
    return hits;
}

std::string start_download(httplib::Client& api, const std::string& key, const std::string& track_json) {
    const auto created = api.Post("/api/v1/downloads", {{"Idempotency-Key", key}}, R"({"track":)" + track_json + "}",
                                  "application/json");
    REQUIRE(created->status == 202);
    return body(created)["data"]["task_id"].get<std::string>();
}

// The statuses one SSE stream reported for each task, in order, until every listed task
// ended; only newer versions count (the client merge rule).
std::map<std::string, std::vector<std::string>> follow(SseClient& stream, const std::set<std::string>& tasks) {
    TaskBoard board;
    std::map<std::string, std::vector<std::string>> seen;
    std::set<std::string> done;
    for (auto frame = stream.next_event(20s); frame; frame = stream.next_event(20s)) {
        if (frame->event != "task.updated") {
            continue;
        }
        const auto update = json::parse(frame->data);
        const auto id = update["task_id"].get<std::string>();
        if (!board.apply(update)) {
            continue;
        }
        auto& statuses = seen[id];
        const auto status = update["status"].get<std::string>();
        if (statuses.empty() || statuses.back() != status) {
            statuses.push_back(status);
        }
        if (status == "completed" || status == "failed") {
            done.insert(id);
        }
        if (done == tasks) {
            break;
        }
    }
    return seen;
}

} // namespace

TEST_CASE("T41: two real platforms, two principals, one download path", "[gate][t41]") {
    Deployment d{"t41-gate"};
    server::CommandHooks hooks;
    hooks.host.bundles = both();
    REQUIRE(d.run({"init"}, hooks).code == server::exit_ok);
    const auto token_a = d.read(d.bootstrap());
    const auto principal_b = create_principal(d, hooks, "第二个客户端");
    d.config["destinations"].push_back({{"id", "dst_b"},
                                        {"name", "第二目录"},
                                        {"root", infrastructure::filesystem::path_to_utf8(d.dir / "second")},
                                        {"principals", {principal_b}}});
    d.config["event_stream"] = {{"heartbeat_seconds", 1}, {"progress_interval_ms", 100}};
    d.write_config();
    const auto token_b = issue(d, hooks, principal_b, "b.txt");

    const auto five_audio = mp3(800);
    const auto kuwo_audio = mp3(6738); // the 176 s Kuwo declares for 78932517
    auto upstream = std::make_shared<Upstream>(five_audio, kuwo_audio);
    RunningService service{d, std::make_shared<infrastructure::observability::MemorySink>(), both(), false, upstream};
    REQUIRE(service.report.events_base_url);
    auto a = service.client(token_a);
    auto b = service.client(token_b);
    const auto& events_url = *service.report.events_base_url;
    const auto events_port = std::stoi(events_url.substr(events_url.rfind(':') + 1));
    SseClient stream_a{events_port, token_a};
    SseClient stream_b{events_port, token_b};
    const auto ready_a = stream_a.next_event();
    const auto ready_b = stream_b.next_event();
    REQUIRE(ready_a);
    REQUIRE(ready_b);
    CHECK(ready_a->event == "stream.ready");
    CHECK(ready_b->event == "stream.ready");

    // Registration: both packages from the build list, filtered by what each declares.
    CHECK(listed_sources(a, "") == std::set<std::string>{"fivesing", "kuwo"});
    CHECK(listed_sources(a, "?capability=resolve_audio") == std::set<std::string>{"fivesing", "kuwo"});
    CHECK(listed_sources(a, "?capability=lyrics") == std::set<std::string>{"fivesing"});
    CHECK(listed_sources(a, "?capability=search_playlists").empty());
    const auto providers = body(a.Get("/api/v1/providers"));
    for (const auto& item : providers["data"]["items"]) {
        CHECK(item["implementation"]["execution_mode"] == "native");
        CHECK(item["auth_methods"].empty());
        const auto expected = item["source"] == "kuwo" ? json::array({"standard"}) : json::array({"standard", "high"});
        CHECK(item["quality_hints"] == expected);
    }

    // Capability differences through the same routes, decided by the declarations.
    CHECK(code_of(a.Get("/api/v1/lyrics?source=kuwo&id=78932517")) == "CAPABILITY_UNSUPPORTED");
    CHECK(a.Get("/api/v1/lyrics?source=fivesing&id=" + work)->status == 200);
    const auto kuwo_high = a.Post("/api/v1/assets/resolve",
                                  R"({"track":{"source":"kuwo","id":"78932517"},"quality":"high"})", "application/json");
    CHECK(code_of(kuwo_high) == "QUALITY_UNAVAILABLE");
    const auto five_lower = a.Post("/api/v1/assets/resolve",
                                   R"({"track":{"source":"fivesing","id":"fc-16394286"},"quality":"high",)"
                                   R"("quality_policy":"allow_lower"})",
                                   "application/json");
    REQUIRE(five_lower->status == 200);
    CHECK(body(five_lower)["data"]["actual_quality"] == "standard");
    // Each package owns its id format; one source's id is not valid at the other.
    CHECK(a.Get("/api/v1/tracks/detail?source=kuwo&id=" + work)->status == 400);
    CHECK(a.Get("/api/v1/tracks/detail?source=fivesing&id=78932517")->status == 400);

    // Catalog results are kept per identity: B never gets A's cached answers.
    const std::string search = "/api/v1/search/tracks?q=%E5%8D%83%E5%B9%B4&sources=kuwo,fivesing&limit=3";
    CHECK(cache_hits(a, search) == std::map<std::string, bool>{{"fivesing", false}, {"kuwo", false}});
    CHECK(cache_hits(a, search) == std::map<std::string, bool>{{"fivesing", true}, {"kuwo", true}});
    const auto searches = upstream->api_requests("http://search.5sing.kugou.com/");
    CHECK(cache_hits(b, search) == std::map<std::string, bool>{{"fivesing", false}, {"kuwo", false}});
    CHECK(upstream->api_requests("http://search.5sing.kugou.com/") == searches + 1);

    // One runner for both sources and both principals, at the same time.
    const auto a_kuwo = start_download(a, "t41-gate-a-kuwo-0001", R"({"source":"kuwo","id":"78932517"})");
    const auto a_five = start_download(a, "t41-gate-a-five-0001", R"({"source":"fivesing","id":"fc-16394286"})");
    const auto b_five = start_download(b, "t41-gate-b-five-0001", R"({"source":"fivesing","id":"fc-16394286"})");
    const auto seen_a = follow(stream_a, {a_kuwo, a_five});
    const auto seen_b = follow(stream_b, {b_five});

    // Events reach their owner only, and every task runs the same state machine.
    std::set<std::string> ids_a;
    for (const auto& [id, statuses] : seen_a) {
        ids_a.insert(id);
    }
    CHECK(ids_a == std::set<std::string>{a_kuwo, a_five});
    REQUIRE(seen_b.size() == 1);
    CHECK(seen_b.begin()->first == b_five);
    REQUIRE(seen_a.count(a_kuwo) == 1);
    REQUIRE(seen_a.count(a_five) == 1);
    CHECK(seen_a.at(a_kuwo).back() == "completed");
    CHECK(seen_a.at(a_kuwo) == seen_a.at(a_five));
    CHECK(seen_b.at(b_five) == seen_a.at(a_five));

    const auto kuwo_task = wait_status(a, a_kuwo, "completed");
    const auto five_task = wait_status(a, a_five, "completed");
    const auto theirs = wait_status(b, b_five, "completed");
    REQUIRE(kuwo_task["status"] == "completed");
    REQUIRE(five_task["status"] == "completed");
    REQUIRE(theirs["status"] == "completed");
    const auto five_file_id = five_task["result"]["file_id"].get<std::string>();
    const auto kuwo_file = body(a.Get("/api/v1/files/" + kuwo_task["result"]["file_id"].get<std::string>()))["data"];
    const auto five_file = body(a.Get("/api/v1/files/" + five_file_id))["data"];
    const auto b_file_id = theirs["result"]["file_id"].get<std::string>();
    CHECK(kuwo_file["metadata_status"] == "complete");
    CHECK(five_file["metadata_status"] == "complete");
    CHECK(five_file["display_name"] == "琴酒蜀黍 - 千年食谱颂.mp3");

    // Each principal's files land in its own destination and stay invisible to the other.
    const auto five_name = infrastructure::filesystem::path_from_utf8("琴酒蜀黍 - 千年食谱颂.mp3");
    CHECK(without_id3v2(read_file(d.dir / "下载" / five_name)) == five_audio);
    CHECK(without_id3v2(read_file(d.dir / "second" / five_name)) == five_audio);
    CHECK(without_id3v2(read_file(d.dir / "下载" / infrastructure::filesystem::path_from_utf8(
                                                       kuwo_file["display_name"].get<std::string>()))) == kuwo_audio);
    CHECK(b.Get("/api/v1/downloads/" + a_five)->status == 404);
    CHECK(b.Get("/api/v1/files/" + five_file_id)->status == 404);
    CHECK(a.Get("/api/v1/files/" + b_file_id)->status == 404);
    CHECK(body(b.Get("/api/v1/downloads"))["data"]["items"].size() == 1);
    CHECK(body(a.Get("/api/v1/downloads"))["data"]["items"].size() == 2);

    // Reuse is per principal and per track: A's new keys reuse A's files and nothing is
    // fetched again; B downloaded its own copy.
    for (const auto& [key, track] : {std::pair{"t41-gate-a-kuwo-0002", R"({"source":"kuwo","id":"78932517"})"},
                                     std::pair{"t41-gate-a-five-0002", R"({"source":"fivesing","id":"fc-16394286"})"}}) {
        INFO(key);
        const auto reused = a.Post("/api/v1/downloads", {{"Idempotency-Key", key}},
                                   std::string{R"({"track":)"} + track + "}", "application/json");
        CHECK(reused->status == 200);
        CHECK(body(reused)["data"]["reused"] == true);
    }
    CHECK(upstream->audio_gets(kuwo_cdn) == 1);
    CHECK(upstream->audio_gets(primary_host) == 2); // A's and B's
}
