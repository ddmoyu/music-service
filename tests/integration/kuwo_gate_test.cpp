// The S1 gate (T20) without network: the real music-service process with the real
// Kuwo package, whose upstream answers come from the recorded fixtures and a CDN that
// serves a 176 s MP3 with ranges. Search → detail → resolve → durable task → SSE and
// HTTP → file and ticket, then the client-side save failing on its own (A29), a restart,
// replay and reuse, a cancel during the transfer, a shutdown checkpoint resumed after
// the restart (A27) and a client that only uses HTTP (A28). The online run of the same
// loop is packaging/e2e/kuwo_e2e.py.

#include "service_harness.hpp"
#include "sse_client.hpp"

#include "providers/kuwo/bundle.hpp"

#include <condition_variable>
#include <fstream>
#include <mutex>

using namespace music_service;
using namespace music_service::test;
namespace fs = std::filesystem;
using infrastructure::filesystem::path_to_utf8;

namespace {

const std::string rid = "78932517";
const std::string cdn = "https://kw-lv.kuwo.cn/";

std::string fixture(const char* name) {
    std::ifstream file(fs::path{MUSIC_SERVICE_TEST_FIXTURES} / "providers" / "kuwo" / name, std::ios::binary);
    REQUIRE(file);
    return {std::istreambuf_iterator<char>(file), {}};
}

// 26.122 ms per 128 kbit/s frame at 44.1 kHz: 6738 frames are the 176 s Kuwo declares.
std::string mp3(std::size_t frames = 6738) {
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

// The audio of a published file: without the ID3v2 tag the processor writes in front.
std::string without_id3v2(const std::string& bytes) {
    if (bytes.size() < 10 || bytes.compare(0, 3, "ID3") != 0) {
        return bytes;
    }
    const auto size = (std::size_t{static_cast<unsigned char>(bytes[6]) & 0x7FU} << 21) |
                      (std::size_t{static_cast<unsigned char>(bytes[7]) & 0x7FU} << 14) |
                      (std::size_t{static_cast<unsigned char>(bytes[8]) & 0x7FU} << 7) |
                      std::size_t{static_cast<unsigned char>(bytes[9]) & 0x7FU};
    const auto footer = (static_cast<unsigned char>(bytes[5]) & 0x10U) != 0 ? 10U : 0U;
    return bytes.substr(std::min(bytes.size(), 10 + size + footer));
}

std::optional<std::string> header(const ports::HttpRequest& request, std::string_view name) {
    for (const auto& item : request.headers) {
        if (item.name == name) {
            return item.value;
        }
    }
    return std::nullopt;
}

// Kuwo as the recorded fixtures show it: search, the song page and mobi.s for rid
// 78932517 (other rids are missing songs), and the CDN with a strong validator and
// single ranges. hold() makes an audio body stop after its first chunk until the
// operation is stopped or released.
class KuwoUpstream final : public ports::HttpTransport {
public:
    explicit KuwoUpstream(std::string audio) : audio_(std::move(audio)) {}

    void hold(bool on) {
        const std::lock_guard lock{mutex_};
        hold_ = on;
        changed_.notify_all();
    }
    bool wait_holding(std::chrono::milliseconds timeout = 10s) {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, timeout, [this] { return holding_; });
    }
    std::vector<ports::HttpRequest> requests() const {
        const std::lock_guard lock{mutex_};
        return requests_;
    }
    std::size_t audio_requests() const {
        const std::lock_guard lock{mutex_};
        return static_cast<std::size_t>(std::count_if(requests_.begin(), requests_.end(), [](const auto& r) {
            return r.url.rfind(cdn, 0) == 0;
        }));
    }

    domain::Result<ports::HttpResponse> request(const ports::HttpRequest& request, const ports::UpstreamLease&,
                                                const ports::OperationContext&) override {
        record(request);
        const auto& url = request.url;
        if (url.starts_with("https://newlyric.kuwo.cn/newlyric.lrc?")) {
            return answer(200, "application/octet-stream", fixture("lyric-synthetic.bin"), url);
        }
        if (url.rfind("https://www.kuwo.cn/search/searchMusicBykeyWord?", 0) == 0) {
            return answer(200, "application/json; charset=utf-8", fixture("search-qingtian-pn0-rn3.json"), url);
        }
        if (url == "https://www.kuwo.cn/play_detail/" + rid) {
            return answer(200, "text/html; charset=utf-8", fixture("play-detail-78932517.html"), url);
        }
        if (url.rfind("https://www.kuwo.cn/play_detail/", 0) == 0) {
            return answer(430, "text/html; charset=utf-8", fixture("play-detail-missing.html"), url);
        }
        if (url.rfind("https://mobi.kuwo.cn/mobi.s?", 0) == 0) {
            const auto found = url.find("&rid=" + rid + "&") != std::string::npos;
            return answer(200, "text/plain", fixture(found ? "audio-78932517-standard.json" : "audio-missing.json"), url);
        }
        return answer(404, "text/plain", "not found", url);
    }

    domain::Result<ports::HttpResponseHead> receive(const ports::HttpRequest& request, const ports::UpstreamLease&,
                                                    const ports::OperationContext& context,
                                                    const ports::HeadConsumer& on_head,
                                                    const ports::ChunkConsumer& on_chunk) override {
        record(request);
        if (request.url.rfind(cdn, 0) != 0) {
            ports::HttpResponseHead missing{404, {{"Content-Length", "0"}}, request.url};
            on_head(missing);
            return missing;
        }
        std::size_t from = 0;
        ports::HttpResponseHead head{200, {}, request.url};
        const auto range = header(request, "Range");
        const auto if_range = header(request, "If-Range");
        if (range && range->rfind("bytes=", 0) == 0 && (!if_range || *if_range == etag)) {
            from = std::stoull(range->substr(6));
            head.status = 206;
            head.headers.push_back({"Content-Range", "bytes " + std::to_string(from) + "-" +
                                                         std::to_string(audio_.size() - 1) + "/" +
                                                         std::to_string(audio_.size())});
        }
        head.headers.push_back({"Content-Type", "audio/mpeg"});
        head.headers.push_back({"Content-Length", std::to_string(audio_.size() - from)});
        head.headers.push_back({"ETag", etag});
        head.headers.push_back({"Accept-Ranges", "bytes"});
        if (!on_head(head)) {
            return head;
        }
        for (auto sent = from; sent < audio_.size();) {
            if (context.stop.stop_requested()) {
                return domain::Error{domain::ErrorCode::canceled, "the operation was canceled", false, {}};
            }
            if (sent > from && !wait_released(context)) {
                return domain::Error{domain::ErrorCode::canceled, "the operation was canceled", false, {}};
            }
            const auto size = std::min<std::size_t>(16 * 1024, audio_.size() - sent);
            if (!on_chunk(std::span<const char>{audio_.data() + sent, size})) {
                head.stopped_by_consumer = true;
                return head;
            }
            sent += size;
        }
        return head;
    }

    static constexpr const char* etag = "\"kw-78932517\"";

private:
    void record(const ports::HttpRequest& request) {
        const std::lock_guard lock{mutex_};
        requests_.push_back(request);
    }

    static ports::HttpResponse answer(int status, std::string type, std::string body, const std::string& url) {
        return {{status, {{"Content-Type", std::move(type)}, {"Content-Length", std::to_string(body.size())}}, url},
                std::move(body)};
    }

    // Blocks while held; false when the operation was stopped meanwhile.
    bool wait_released(const ports::OperationContext& context) {
        std::unique_lock lock{mutex_};
        while (hold_ && !context.stop.stop_requested()) {
            holding_ = true;
            changed_.notify_all();
            changed_.wait_for(lock, 5ms);
        }
        holding_ = false;
        return !context.stop.stop_requested();
    }

    std::string audio_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<ports::HttpRequest> requests_;
    bool hold_{false};
    bool holding_{false};
};

std::vector<ports::ProviderBundle> kuwo() { return {providers::kuwo::make_bundle()}; }

httplib::Headers key(std::string value) { return {{"Idempotency-Key", std::move(value)}}; }

const std::string download_body = R"({"track":{"source":"kuwo","id":"78932517"}})";

// A deployment of the gate: every outlet on loopback with free ports, a short shutdown.
struct Gate {
    explicit Gate(std::string_view name) : d{name} {
        d.config["shutdown"] = {{"soft_seconds", 1}};
        d.config["event_stream"] = {{"heartbeat_seconds", 1}, {"progress_interval_ms", 100}};
        d.write_config();
        REQUIRE(d.run({"init"}, hooks()).code == server::exit_ok);
        token = d.read(d.bootstrap());
        root = d.dir / "下载";
    }

    static server::CommandHooks hooks() {
        server::CommandHooks h;
        h.host.bundles = kuwo();
        return h;
    }

    std::unique_ptr<RunningService> start(const std::shared_ptr<KuwoUpstream>& upstream) {
        return std::make_unique<RunningService>(d, std::make_shared<infrastructure::observability::MemorySink>(), kuwo(),
                                                false, upstream);
    }

    Deployment d;
    std::string token;
    fs::path root;
};

int port_of(const std::string& url) { return std::stoi(url.substr(url.rfind(':') + 1)); }

httplib::Client media_client(const RunningService& service) {
    httplib::Client c("127.0.0.1", port_of(*service.report.media_base_url));
    c.set_connection_timeout(3s);
    c.set_read_timeout(10s);
    return c;
}

std::string path_of(const std::string& url) {
    const auto scheme = url.find("://");
    return url.substr(url.find('/', scheme + 3));
}

// Polls the task over HTTP until it reaches the status.
json wait_status(httplib::Client& api, const std::string& task_id, std::string_view status,
                 std::chrono::seconds timeout = 30s) {
    const auto until = std::chrono::steady_clock::now() + timeout;
    json task;
    while (std::chrono::steady_clock::now() < until) {
        task = body(api.Get("/api/v1/downloads/" + task_id))["data"];
        if (task["status"] == std::string{status}) {
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

} // namespace

TEST_CASE("S1 gate: Kuwo search to a published file over HTTP, SSE and media", "[gate][kuwo]") {
    Gate g{"gate-loop"};
    const auto audio = mp3();
    auto upstream = std::make_shared<KuwoUpstream>(audio);
    auto service = g.start(upstream);
    REQUIRE(service->report.media_base_url);
    REQUIRE(service->report.events_base_url);
    auto api = service->client(g.token);
    const auto first_instance = service->report.instance_id;

    const auto info = body(api.Get("/api/v1/info"))["data"];
    CHECK(info["features"]["downloads"] == true);
    CHECK(info["features"]["file_access"] == true);
    CHECK(info["features"]["progress_sse"] == true);
    CHECK(info["features"]["media_proxy"] == false);

    // The client subscribes first, then lists: the order the protocol asks for.
    SseClient stream{port_of(*service->report.events_base_url), g.token};
    const auto ready = stream.next_event();
    REQUIRE(ready);
    CHECK(ready->event == "stream.ready");

    // Search, detail and resolve on the real package.
    const auto search = api.Get("/api/v1/search/tracks?q=%E6%99%B4%E5%A4%A9&sources=kuwo&limit=3");
    REQUIRE(search->status == 200);
    const auto items = body(search)["data"]["items"];
    REQUIRE(items.size() == 3);
    const auto hit = std::find_if(items.begin(), items.end(), [](const json& t) { return t["ref"]["id"] == rid; });
    REQUIRE(hit != items.end());
    CHECK((*hit)["title"] == "晴天");
    CHECK((*hit)["ref"]["source"] == "kuwo");
    const auto detail = api.Get("/api/v1/tracks/detail?source=kuwo&id=" + rid);
    REQUIRE(detail->status == 200);
    const auto track = body(detail)["data"];
    CHECK(track["title"] == "晴天");
    const auto artist = track["artists"][0]["name"].get<std::string>();
    const auto resolved = api.Post("/api/v1/assets/resolve", R"({"track":{"source":"kuwo","id":"78932517"}})",
                                   "application/json");
    REQUIRE(resolved->status == 200);
    const auto asset = body(resolved)["data"];
    CHECK(asset["actual_quality"] == "standard");
    CHECK(asset["container"] == "mp3");
    CHECK(asset["bitrate_bps"] == 128000);
    CHECK(asset["completeness"] == "full");
    CHECK(api.Get("/api/v1/tracks/detail?source=kuwo&id=999999999999")->status == 404);

    // The durable task; the creating connection is gone before it runs (A04).
    std::string task_id;
    {
        auto creator = service->client(g.token);
        const auto created = creator.Post("/api/v1/downloads", key("gate-download-0001"), download_body,
                                          "application/json");
        REQUIRE(created->status == 202);
        task_id = body(created)["data"]["task_id"].get<std::string>();
    }

    // SSE carries committed versions only forward until completed (A10, A11).
    TaskBoard board;
    std::vector<std::string> seen;
    for (auto frame = stream.next_event(20s); frame; frame = stream.next_event(20s)) {
        if (frame->event != "task.updated") {
            continue;
        }
        const auto update = json::parse(frame->data);
        if (update["task_id"] != task_id) {
            continue;
        }
        if (board.apply(update)) {
            seen.push_back(update["status"].get<std::string>());
        }
        if (update["status"] == "completed") {
            break;
        }
    }
    REQUIRE_FALSE(seen.empty());
    CHECK(seen.back() == "completed");
    CHECK(std::find(seen.begin(), seen.end(), "downloading") != seen.end());

    // The full task over HTTP: the result, its file and the facts of the execution.
    const auto task = wait_status(api, task_id, "completed");
    CHECK(task["attempt"] == 1);
    CHECK(task["actual_track"]["source"] == "kuwo");
    CHECK(task["actual_track"]["id"] == rid);
    CHECK(task["actual_quality"] == "standard");
    CHECK(task["error"].is_null());
    const auto file_id = task["result"]["file_id"].get<std::string>();
    CHECK(task["result"]["links"]["file"] == "/api/v1/files/" + file_id);
    const auto published = g.root / infrastructure::filesystem::path_from_utf8(artist + " - 晴天.mp3");
    const auto stored = read_file(published);
    CHECK(without_id3v2(stored) == audio); // the audio as received, behind the written tags
    CHECK_FALSE(fs::exists(g.root / ".music-service-work" / task_id));

    const auto described = api.Get("/api/v1/files/" + file_id);
    REQUIRE(described->status == 200);
    const auto file = body(described)["data"];
    CHECK(file["size_bytes"] == stored.size());
    CHECK(file["metadata_status"] == "complete");
    CHECK(file["container"] == "mp3");
    CHECK(file["content_type"] == "audio/mpeg");
    CHECK(file["display_name"] == artist + " - 晴天.mp3");
    // Nothing a client sees names a server path.
    const auto root_text = path_to_utf8(g.root);
    for (const auto& text : {described->body, api.Get("/api/v1/downloads/" + task_id)->body}) {
        CHECK(text.find(root_text) == std::string::npos);
        CHECK(text.find(".music-service-work") == std::string::npos);
    }

    // A ticket for a player: a range, then the whole file.
    const auto access =
        api.Post("/api/v1/media-access", R"({"target":{"kind":"file","id":")" + file_id + R"("}})", "application/json");
    REQUIRE(access->status == 200);
    const auto ticket = path_of(body(access)["data"]["url"].get<std::string>());
    auto media = media_client(*service);
    const auto head_bytes = media.Get(ticket, httplib::Headers{{"Range", "bytes=0-15"}});
    REQUIRE(head_bytes->status == 206);
    CHECK(head_bytes->get_header_value("Content-Range") == "bytes 0-15/" + std::to_string(stored.size()));
    CHECK(head_bytes->body == stored.substr(0, 16));

    // A29: "save to this device" fails on the client half-way; the task stays completed.
    {
        auto saver = media_client(*service);
        std::size_t received = 0;
        const auto aborted = saver.Get(ticket, [&](const char*, std::size_t size) {
            received += size;
            return received < 64 * 1024; // the client's disk is full
        });
        CHECK_FALSE(aborted);
        CHECK(received < stored.size());
    }
    CHECK(body(api.Get("/api/v1/downloads/" + task_id))["data"]["status"] == "completed");
    const auto until = std::chrono::steady_clock::now() + 5s;
    while (body(api.Get("/api/v1/admin/status"))["data"]["media"]["responses"] != 0 &&
           std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(20ms);
    }
    CHECK(body(api.Get("/api/v1/admin/status"))["data"]["media"]["responses"] == 0);
    const auto again = media.Get(ticket);
    REQUIRE(again->status == 200);
    CHECK(again->body == stored);

    // Only HTTPS to the hosts the package names; no secrets attached.
    const auto requests = upstream->requests();
    for (const auto& request : requests) {
        CHECK(request.url.rfind("https://", 0) == 0);
        const auto host = request.url.substr(8, request.url.find('/', 8) - 8);
        CHECK((host == "www.kuwo.cn" || host == "mobi.kuwo.cn" || host == "kw-lv.kuwo.cn" || host == "newlyric.kuwo.cn"));
        CHECK(request.secret_headers.empty());
    }
    CHECK(upstream->audio_requests() == 1);

    // Restart: a new instance, the same durable task and file; another principal, created
    // while the service is stopped, sees none of it (A06, A23).
    stream.close();
    service->stop();
    service.reset();
    const auto created = g.d.run({"principal", "create", "--name", "other"}, Gate::hooks());
    REQUIRE(created.code == server::exit_ok);
    const auto other_id = created.out.substr(0, created.out.find('\t'));
    REQUIRE(g.d.run({"token", "issue", "--principal", other_id, "--scopes", "downloads:read,files:read", "--output",
                     path_to_utf8(g.d.dir / "out" / "other.txt")},
                    Gate::hooks())
                .code == server::exit_ok);
    const auto other_token = g.d.read(g.d.dir / "out" / "other.txt");

    service = g.start(upstream);
    CHECK(service->report.instance_id != first_instance);
    auto restarted = service->client(g.token);
    const auto after = body(restarted.Get("/api/v1/downloads/" + task_id))["data"];
    CHECK(after["status"] == "completed");
    CHECK(after["result"]["file_id"] == file_id);
    const auto fresh_access = restarted.Post("/api/v1/media-access",
                                             R"({"target":{"kind":"file","id":")" + file_id + R"("}})",
                                             "application/json");
    REQUIRE(fresh_access->status == 200);
    // Tickets live in memory: the old one is unknown after the restart.
    CHECK(media_client(*service).Get(ticket)->status == 404);
    const auto reread = media_client(*service).Get(path_of(body(fresh_access)["data"]["url"].get<std::string>()));
    REQUIRE(reread->status == 200);
    CHECK(reread->body == stored);

    auto theirs = service->client(other_token);
    CHECK(theirs.Get("/api/v1/downloads/" + task_id)->status == 404);
    CHECK(theirs.Get("/api/v1/files/" + file_id)->status == 404);
    CHECK(body(theirs.Get("/api/v1/downloads"))["data"]["items"].empty());

    // Replay of the first key is the first answer (A01); a new key reuses the file (A46).
    const auto replay = restarted.Post("/api/v1/downloads", key("gate-download-0001"), download_body, "application/json");
    CHECK(replay->status == 202);
    CHECK(replay->get_header_value("Idempotency-Replayed") == "true");
    CHECK(body(replay)["data"]["task_id"] == task_id);
    const auto reused = restarted.Post("/api/v1/downloads", key("gate-download-0002"), download_body, "application/json");
    CHECK(reused->status == 200);
    CHECK(body(reused)["data"]["reused"] == true);
    CHECK(body(reused)["data"]["task_id"] == task_id);
    CHECK(upstream->audio_requests() == 1); // nothing was downloaded again
}

TEST_CASE("S1 gate: a cancel stops the transfer, a shutdown checkpoint resumes after restart", "[gate][kuwo]") {
    Gate g{"gate-cancel"};
    const auto audio = mp3();
    auto upstream = std::make_shared<KuwoUpstream>(audio);
    auto service = g.start(upstream);
    auto api = service->client(g.token);
    SseClient stream{port_of(*service->report.events_base_url), g.token};
    REQUIRE(stream.next_event());

    // Cancel while the body is on the wire.
    upstream->hold(true);
    const auto created = api.Post("/api/v1/downloads", key("gate-cancel-0001"), download_body, "application/json");
    REQUIRE(created->status == 202);
    const auto canceled_id = body(created)["data"]["task_id"].get<std::string>();
    REQUIRE(upstream->wait_holding());
    const auto cancel = api.Post("/api/v1/downloads/" + canceled_id + "/cancel", "", "application/json");
    REQUIRE(cancel->status == 202);
    CHECK(body(cancel)["data"]["status"] == "canceling");
    const auto canceled = wait_status(api, canceled_id, "canceled");
    CHECK(canceled["result"].is_null());
    CHECK_FALSE(fs::exists(g.root / ".music-service-work" / canceled_id));
    std::vector<std::string> statuses;
    for (auto frame = stream.next_event(2s); frame; frame = stream.next_event(2s)) {
        if (frame->event == "task.updated") {
            const auto update = json::parse(frame->data);
            statuses.push_back(update["status"].get<std::string>());
            if (update["status"] == "canceled") {
                break;
            }
        }
    }
    CHECK(std::find(statuses.begin(), statuses.end(), "canceling") != statuses.end());
    CHECK((!statuses.empty() && statuses.back() == "canceled"));
    for (const auto& entry : fs::recursive_directory_iterator(g.root)) {
        CHECK_FALSE(entry.is_regular_file()); // nothing was published
    }

    // A shutdown during the next transfer keeps the flushed bytes.
    const auto second = api.Post("/api/v1/downloads", key("gate-cancel-0002"), download_body, "application/json");
    REQUIRE(second->status == 202);
    const auto task_id = body(second)["data"]["task_id"].get<std::string>();
    CHECK(task_id != canceled_id);
    REQUIRE(upstream->wait_holding());
    stream.close();
    const auto stopped = service->stop();
    CHECK(stopped.code == server::exit_ok);
    // The launcher's stdout line names every listener.
    const auto line_start = stopped.out.find("{\"");
    REQUIRE(line_start != std::string::npos);
    const auto line_end = stopped.out.find('\n', line_start);
    const auto ready_line = json::parse(stopped.out.substr(line_start, line_end - line_start));
    CHECK(ready_line["event"] == "ready");
    CHECK(ready_line["media_base_url"] == *service->report.media_base_url);
    CHECK(ready_line["events_base_url"] == *service->report.events_base_url);
    service.reset();
    const auto audio_before = upstream->audio_requests();

    // The next start resumes with a validated range and publishes the whole file.
    upstream->hold(false);
    service = g.start(upstream);
    auto restarted = service->client(g.token);
    const auto done = wait_status(restarted, task_id, "completed");
    CHECK(done["attempt"] == 1); // a shutdown is not a failed attempt
    const auto requests = upstream->requests();
    REQUIRE(upstream->audio_requests() == audio_before + 1);
    const auto& resumed = *std::find_if(requests.rbegin(), requests.rend(),
                                        [](const auto& r) { return r.url.rfind(cdn, 0) == 0; });
    const auto range = header(resumed, "Range");
    REQUIRE(range);
    CHECK(*range != "bytes=0-");
    CHECK(header(resumed, "If-Range") == std::optional<std::string>{KuwoUpstream::etag});
    const auto file_id = done["result"]["file_id"].get<std::string>();
    const auto access =
        restarted.Post("/api/v1/media-access", R"({"target":{"kind":"file","id":")" + file_id + R"("}})",
                       "application/json");
    REQUIRE(access->status == 200);
    const auto whole = media_client(*service).Get(path_of(body(access)["data"]["url"].get<std::string>()));
    REQUIRE(whole->status == 200);
    CHECK(without_id3v2(whole->body) == audio);
}

TEST_CASE("S1 gate: a client without SSE completes the loop over HTTP", "[gate][kuwo]") {
    Gate g{"gate-http"};
    g.d.config["events"] = {{"enabled", false}};
    g.d.write_config();
    const auto audio = mp3();
    auto upstream = std::make_shared<KuwoUpstream>(audio);
    auto service = g.start(upstream);
    CHECK_FALSE(service->report.events_base_url);
    auto api = service->client(g.token);
    const auto info = body(api.Get("/api/v1/info"))["data"];
    CHECK(info["features"]["progress_sse"] == false);
    CHECK(info["features"]["progress_polling"] == true);

    const auto created = api.Post("/api/v1/downloads", key("gate-http-only-0001"), download_body, "application/json");
    REQUIRE(created->status == 202);
    const auto task_id = body(created)["data"]["task_id"].get<std::string>();
    const auto done = wait_status(api, task_id, "completed");
    const auto listed = body(api.Get("/api/v1/downloads?status=completed"))["data"]["items"];
    REQUIRE(listed.size() == 1);
    CHECK(listed[0]["task_id"] == task_id);
    const auto file = api.Get("/api/v1/files/" + done["result"]["file_id"].get<std::string>());
    REQUIRE(file->status == 200);
    CHECK(body(file)["data"]["size_bytes"] > audio.size()); // the audio and its tags
}
