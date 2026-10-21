// The SSE entry end to end over loopback with a minimal protocol client: frames split
// across network chunks, owner isolation, merge by version against HTTP responses,
// limits that leave the control API usable, revocation, lifetime and restart.

#include "service_harness.hpp"

#include "infrastructure/storage/sqlite/database.hpp"

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>

using namespace music_service;
using namespace music_service::test;
namespace fs = std::filesystem;
using infrastructure::filesystem::path_to_utf8;

namespace {

struct Frame {
    std::string event; // empty for a comment
    std::string data;
    std::string comment;
};

// Reads one SSE stream on its own thread and parses it as a client must: messages end
// at a blank line, wherever the network split the bytes.
class SseClient {
public:
    SseClient(int port, const std::string& token, httplib::Headers headers = {}) {
        thread_ = std::thread([this, port, token, headers = std::move(headers)] {
            httplib::Client client("127.0.0.1", port);
            client.set_connection_timeout(3s);
            client.set_read_timeout(20s);
            if (!token.empty()) {
                client.set_bearer_token_auth(token);
            }
            auto result = client.Get(
                "/events/v1/downloads", headers,
                [this](const httplib::Response& response) {
                    const std::lock_guard lock{mutex_};
                    status_ = response.status;
                    content_type_ = response.get_header_value("Content-Type");
                    retry_after_ = response.get_header_value("Retry-After");
                    return true;
                },
                [this](const char* data, std::size_t size) {
                    const std::lock_guard lock{mutex_};
                    if (status_ != 200) {
                        body_.append(data, size); // a JSON error before any stream
                        return true;
                    }
                    raw_.append(data, size);
                    parse();
                    changed_.notify_all();
                    return !stop_;
                });
            const std::lock_guard lock{mutex_};
            if (result) {
                status_ = result->status;
            }
            ended_ = true;
            changed_.notify_all();
        });
    }
    ~SseClient() { close(); }
    SseClient(const SseClient&) = delete;
    SseClient& operator=(const SseClient&) = delete;

    // The client side of a disconnect: takes effect at the next bytes (a heartbeat).
    void close() {
        {
            const std::lock_guard lock{mutex_};
            stop_ = true;
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    // The next event, skipping heartbeats; nullopt after the timeout or the end.
    std::optional<Frame> next_event(std::chrono::milliseconds timeout = 5s) {
        std::unique_lock lock{mutex_};
        const auto until = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            while (!frames_.empty()) {
                auto frame = std::move(frames_.front());
                frames_.pop_front();
                if (!frame.event.empty()) {
                    return frame;
                }
                ++heartbeats_;
            }
            if (ended_ || !changed_.wait_until(lock, until, [this] { return ended_ || !frames_.empty(); })) {
                if (frames_.empty()) {
                    return std::nullopt;
                }
            }
        }
    }

    bool wait_end(std::chrono::milliseconds timeout) {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, timeout, [this] { return ended_; });
    }
    int status() {
        std::unique_lock lock{mutex_};
        changed_.wait_for(lock, 5s, [this] { return status_ != 0 || ended_; });
        return status_;
    }
    std::string body() {
        wait_end(5s);
        const std::lock_guard lock{mutex_};
        return body_;
    }
    std::string content_type() {
        const std::lock_guard lock{mutex_};
        return content_type_;
    }
    std::string retry_after() {
        const std::lock_guard lock{mutex_};
        return retry_after_;
    }
    int heartbeats() {
        const std::lock_guard lock{mutex_};
        for (const auto& frame : frames_) {
            heartbeats_ += frame.event.empty() ? 1 : 0;
        }
        std::erase_if(frames_, [](const Frame& frame) { return frame.event.empty(); });
        return heartbeats_;
    }

private:
    void parse() {
        for (auto end = raw_.find("\n\n"); end != std::string::npos; end = raw_.find("\n\n")) {
            const auto block = raw_.substr(0, end);
            raw_.erase(0, end + 2);
            Frame frame;
            std::size_t start = 0;
            while (start <= block.size()) {
                auto stop = block.find('\n', start);
                if (stop == std::string::npos) {
                    stop = block.size();
                }
                const auto line = block.substr(start, stop - start);
                if (line.rfind("event: ", 0) == 0) {
                    frame.event = line.substr(7);
                } else if (line.rfind("data: ", 0) == 0) {
                    frame.data += line.substr(6);
                } else if (line.rfind(":", 0) == 0) {
                    frame.comment = line.substr(1);
                }
                start = stop + 1;
            }
            frames_.push_back(std::move(frame));
        }
    }

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::string raw_;
    std::deque<Frame> frames_;
    int status_{0};
    int heartbeats_{0};
    std::string content_type_;
    std::string retry_after_;
    std::string body_;
    bool ended_{false};
    bool stop_{false};
};

// The client merge rule (api-contract.md 9.4): per task, only a newer version replaces.
struct TaskBoard {
    std::map<std::string, std::pair<std::uint64_t, std::string>> tasks;
    bool apply(const json& task) {
        const auto id = task["task_id"].get<std::string>();
        const auto version = task["version"].get<std::uint64_t>();
        auto& entry = tasks[id];
        if (version <= entry.first) {
            return false;
        }
        entry = {version, task["status"].get<std::string>()};
        return true;
    }
};

void enable_events(Deployment& d, json limits = json::object(), json stream = json::object()) {
    d.config["events"] = {{"enabled", true}, {"port", 0}, {"workers", 8}, {"connection_queue", 8}};
    json all_limits{{"global_subscriptions", 5}, {"per_principal_subscriptions", 2}};
    all_limits.update(limits);
    d.config["event_limits"] = all_limits;
    json all_stream{{"heartbeat_seconds", 1}};
    all_stream.update(stream);
    d.config["event_stream"] = all_stream;
    d.write_config();
}

int events_port(const RunningService& service) {
    REQUIRE(service.report.events_base_url);
    const auto& url = *service.report.events_base_url;
    return std::stoi(url.substr(url.rfind(':') + 1));
}

std::string issue(Deployment& d, const std::string& principal, const std::string& scopes, const std::string& file) {
    const auto output = d.dir / "out" / file;
    REQUIRE(d.run({"token", "issue", "--principal", principal, "--scopes", scopes, "--output", path_to_utf8(output)})
                .code == server::exit_ok);
    return d.read(output);
}

std::string create_principal(Deployment& d, const std::string& name) {
    const auto created = d.run({"principal", "create", "--name", name});
    REQUIRE(created.code == server::exit_ok);
    return created.out.substr(0, created.out.find('\t'));
}

httplib::Headers key(std::string value) { return {{"Idempotency-Key", std::move(value)}}; }

const std::string download_body = R"({"track":{"source":"both","id":"1"},"metadata_policy":"skip"})";

} // namespace

TEST_CASE("SSE streams committed task versions to their owner only", "[events][server]") {
    Deployment d{"events-owner"};
    enable_events(d);
    REQUIRE(d.run({"init"}).code == server::exit_ok);
    const auto owner = d.read(d.bootstrap());
    const auto other_principal = create_principal(d, "其他客户端");
    d.config["destinations"][0]["principals"].push_back(other_principal);
    d.write_config();
    const auto other = issue(d, other_principal, "downloads:read,downloads:write", "other.txt");

    RunningService service{d, std::make_shared<infrastructure::observability::MemorySink>(),
                           std::vector<ports::ProviderBundle>{mock_combo("both")}};
    auto api = service.client(owner);
    const auto info = body(api.Get("/api/v1/info"))["data"];
    CHECK(info["features"]["progress_sse"] == true);
    CHECK(info["features"]["progress_polling"] == true);
    CHECK(info["events_base_url"] == *service.report.events_base_url);
    const auto port = events_port(service);

    SseClient mine{port, owner};
    SseClient theirs{port, other};
    REQUIRE(mine.status() == 200);
    CHECK(mine.content_type() == "text/event-stream");
    const auto ready = mine.next_event();
    REQUIRE(ready);
    CHECK(ready->event == "stream.ready");
    const auto ready_data = json::parse(ready->data);
    CHECK(ready_data["instance_id"] == service.report.instance_id);
    CHECK(ready_data["sync_required"] == true);
    REQUIRE(theirs.next_event());

    // Subscribe first, then query: the HTTP response and the event merge by version.
    TaskBoard board;
    const auto created = api.Post("/api/v1/downloads", key("events-download-key-1"), download_body, "application/json");
    REQUIRE(created);
    REQUIRE(created->status == 202);
    const auto task_id = body(created)["data"]["task_id"].get<std::string>();
    const auto task = body(api.Get("/api/v1/downloads/" + task_id))["data"]; // the snapshot
    const auto event = mine.next_event();
    REQUIRE(event);
    CHECK(event->event == "task.updated");
    const auto data = json::parse(event->data);
    CHECK(data["task_id"] == task_id);
    CHECK(data["version"] == 1);
    CHECK(data["status"] == "queued");
    CHECK(data["progress"].is_null());
    CHECK(data["updated_at"].get<std::string>().size() == 24);
    CHECK(data.size() == 5); // a projection, not the whole task
    CHECK(board.apply(data));
    CHECK_FALSE(board.apply(task)); // the same version from HTTP changes nothing

    // A replay or a reused intent commits nothing new and sends nothing.
    CHECK(api.Post("/api/v1/downloads", key("events-download-key-1"), download_body, "application/json")->status == 202);
    CHECK(api.Post("/api/v1/downloads", key("events-download-key-2"), download_body, "application/json")->status == 200);

    const auto canceled = api.Post("/api/v1/downloads/" + task_id + "/cancel", "", "application/json");
    REQUIRE(canceled);
    const auto terminal = mine.next_event();
    REQUIRE(terminal);
    const auto terminal_data = json::parse(terminal->data);
    CHECK(terminal_data["version"] == 2); // the canceled version, not the replays
    CHECK(terminal_data["status"] == "canceled");
    CHECK(board.apply(terminal_data));
    CHECK_FALSE(board.apply(task)); // a late older snapshot cannot overwrite it
    CHECK(board.tasks[task_id].second == "canceled");

    // Batches announce each created task once.
    const auto batch = api.Post("/api/v1/download-batches", key("events-batch-key-01"),
                                R"({"items":[{"track":{"source":"both","id":"7"}},{"track":{"source":"both","id":"7"}},
                                    {"track":{"source":"both","id":"8"}}]})",
                                "application/json");
    REQUIRE(batch);
    CHECK(batch->status == 202);
    std::set<std::string> announced;
    for (int i = 0; i < 2; ++i) {
        const auto item = mine.next_event();
        REQUIRE(item);
        announced.insert(json::parse(item->data)["task_id"].get<std::string>());
    }
    CHECK(announced.size() == 2);
    CHECK_FALSE(mine.next_event(1500ms));

    // The other principal saw heartbeats only; its own task reaches only itself.
    CHECK_FALSE(theirs.next_event(500ms));
    CHECK(theirs.heartbeats() >= 1);
    auto their_api = service.client(other);
    REQUIRE(their_api.Post("/api/v1/downloads", key("events-other-key-01"), download_body, "application/json")->status ==
            202);
    const auto theirs_event = theirs.next_event();
    REQUIRE(theirs_event);
    CHECK(json::parse(theirs_event->data)["task_id"] != task_id);
    CHECK_FALSE(mine.next_event(1500ms));
}

TEST_CASE("SSE requests are checked before the stream and limits keep control usable", "[events][server]") {
    Deployment d{"events-limits"};
    enable_events(d);
    REQUIRE(d.run({"init"}).code == server::exit_ok);
    const auto owner = d.read(d.bootstrap());
    const auto catalog_only = issue(d, "prn_local", "catalog:read", "catalog.txt");
    RunningService service{d, std::make_shared<infrastructure::observability::MemorySink>(),
                           std::vector<ports::ProviderBundle>{mock_combo("both")}};
    const auto port = events_port(service);
    httplib::Client raw("127.0.0.1", port);
    raw.set_read_timeout(5s);

    SseClient anonymous{port, ""};
    CHECK(anonymous.status() == 401);
    CHECK(json::parse(anonymous.body())["error"]["code"] == "UNAUTHENTICATED");
    SseClient unscoped{port, catalog_only};
    CHECK(unscoped.status() == 403);
    CHECK(json::parse(unscoped.body())["error"]["code"] == "FORBIDDEN");
    raw.set_bearer_token_auth(owner);
    CHECK(body(raw.Get("/events/v1/downloads?task_id=x"))["error"]["code"] == "VALIDATION_FAILED");
    CHECK(body(raw.Get("/events/v1/other"))["error"]["code"] == "NOT_FOUND");

    // Preflight for allowed origins only.
    const auto preflight = raw.Options("/events/v1/downloads", {{"Origin", "http://127.0.0.1:5173"},
                                                                {"Access-Control-Request-Method", "GET"}});
    REQUIRE(preflight);
    CHECK(preflight->status == 204);
    CHECK(preflight->get_header_value("Access-Control-Allow-Origin") == "http://127.0.0.1:5173");
    CHECK(raw.Options("/events/v1/downloads", {{"Origin", "http://evil.example"}})->status == 403);

    // Two streams per principal; a third is refused while both stay open, and the
    // control API keeps answering. Last-Event-ID resumes nothing: ready asks for a resync.
    SseClient first{port, owner, {{"Last-Event-ID", "42"}}};
    SseClient second{port, owner};
    REQUIRE(first.status() == 200);
    REQUIRE(second.status() == 200);
    const auto ready = first.next_event();
    REQUIRE(ready);
    CHECK(json::parse(ready->data)["sync_required"] == true);
    SseClient third{port, owner};
    CHECK(third.status() == 429);
    CHECK(third.retry_after() == "5");
    CHECK(json::parse(third.body())["error"]["code"] == "CALLER_LIMIT_EXCEEDED");
    auto api = service.client(owner);
    const auto started = std::chrono::steady_clock::now();
    CHECK(api.Get("/api/v1/info")->status == 200);
    CHECK(api.Post("/api/v1/downloads", key("limits-download-key"), download_body, "application/json")->status == 202);
    CHECK(std::chrono::steady_clock::now() - started < 2s);
    REQUIRE(first.next_event()); // the new task still reaches both streams
    REQUIRE(second.next_event());

    // A closed stream frees its slot.
    second.close();
    std::optional<SseClient> again;
    for (int i = 0; i < 20; ++i) {
        again.emplace(port, owner);
        if (again->status() == 200) {
            break;
        }
        again.reset();
        std::this_thread::sleep_for(200ms);
    }
    REQUIRE(again);
    CHECK(again->status() == 200);
}

TEST_CASE("SSE ends on revocation and stop, and clients resync over HTTP", "[events][server]") {
    Deployment d{"events-resync"};
    enable_events(d);
    REQUIRE(d.run({"init"}).code == server::exit_ok);
    const auto owner = d.read(d.bootstrap());
    const auto doomed_output = d.dir / "out" / "doomed.txt";
    const auto issued = d.run({"token", "issue", "--principal", "prn_local", "--scopes", "downloads:read", "--output",
                               path_to_utf8(doomed_output)});
    REQUIRE(issued.code == server::exit_ok);
    const auto doomed = d.read(doomed_output);
    const auto doomed_id = issued.out.substr(6, issued.out.find(' ', 6) - 6);

    std::string task_id;
    std::string first_instance;
    {
        RunningService service{d, std::make_shared<infrastructure::observability::MemorySink>(),
                               std::vector<ports::ProviderBundle>{mock_combo("both")}};
        first_instance = service.report.instance_id;
        const auto port = events_port(service);
        auto api = service.client(owner);

        // Revoking a token ends its stream at the next heartbeat.
        SseClient revoked{port, doomed};
        REQUIRE(revoked.status() == 200);
        REQUIRE(revoked.next_event());
        auto database = infrastructure::storage::sqlite::Database::open(d.dir / "data" / "service.sqlite3", {});
        REQUIRE(database);
        REQUIRE(database.value()->write([&](infrastructure::storage::sqlite::Connection& c) {
            return c.exec("UPDATE access_tokens SET revoked_at = 1 WHERE id = '" + doomed_id + "'");
        }));
        CHECK(revoked.wait_end(5s));

        // A client that is away misses events; on reconnect it resynchronizes over HTTP.
        SseClient away{port, owner};
        REQUIRE(away.next_event());
        const auto created = api.Post("/api/v1/downloads", key("resync-download-key"), download_body, "application/json");
        REQUIRE(created);
        task_id = body(created)["data"]["task_id"].get<std::string>();
        REQUIRE(away.next_event());
        away.close();
        CHECK(api.Post("/api/v1/downloads/" + task_id + "/cancel", "", "application/json")->status == 200);
        SseClient back{port, owner};
        const auto ready = back.next_event();
        REQUIRE(ready);
        CHECK(ready->event == "stream.ready");
        CHECK(body(api.Get("/api/v1/downloads/" + task_id))["data"]["status"] == "canceled");

        // Stopping the service ends open streams.
        auto stopped = service.stop();
        CHECK(stopped.code == server::exit_ok);
        CHECK(back.wait_end(5s));
    }

    // After a restart the instance id changes and the committed state is there to sync.
    RunningService restarted{d, std::make_shared<infrastructure::observability::MemorySink>(),
                             std::vector<ports::ProviderBundle>{mock_combo("both")}};
    SseClient fresh{events_port(restarted), owner};
    const auto ready = fresh.next_event();
    REQUIRE(ready);
    CHECK(json::parse(ready->data)["instance_id"] != first_instance);
    auto api = restarted.client(owner);
    const auto page = body(api.Get("/api/v1/downloads?limit=10"))["data"]["items"];
    REQUIRE(page.size() == 1);
    CHECK(page[0]["task_id"] == task_id);
    CHECK(page[0]["status"] == "canceled");
}

TEST_CASE("SSE connections end at their maximum lifetime", "[events][server]") {
    Deployment d{"events-lifetime"};
    enable_events(d, json::object(), {{"max_connection_seconds", 10}});
    REQUIRE(d.run({"init"}).code == server::exit_ok);
    const auto owner = d.read(d.bootstrap());
    RunningService service{d, std::make_shared<infrastructure::observability::MemorySink>(),
                           std::vector<ports::ProviderBundle>{mock_combo("both")}};
    SseClient stream{events_port(service), owner};
    REQUIRE(stream.status() == 200);
    REQUIRE(stream.next_event());
    const auto started = std::chrono::steady_clock::now();
    REQUIRE(stream.wait_end(15s));
    const auto lasted = std::chrono::steady_clock::now() - started;
    CHECK(lasted > 8s);
    CHECK(stream.heartbeats() >= 5);
    CHECK(stream.status() == 200); // a normal end; the client reconnects
}

TEST_CASE("without SSE every download operation works over HTTP", "[events][server]") {
    Deployment d{"events-off"};
    REQUIRE(d.run({"init"}).code == server::exit_ok);
    const auto owner = d.read(d.bootstrap());
    RunningService service{d, std::make_shared<infrastructure::observability::MemorySink>(),
                           std::vector<ports::ProviderBundle>{mock_combo("both")}};
    CHECK_FALSE(service.report.events_base_url);
    auto api = service.client(owner);
    const auto info = body(api.Get("/api/v1/info"))["data"];
    CHECK(info["features"]["progress_sse"] == false);
    CHECK(info["features"]["progress_polling"] == true);
    CHECK(info["events_base_url"].is_null());
    const auto created = api.Post("/api/v1/downloads", key("http-only-download"), download_body, "application/json");
    REQUIRE(created);
    const auto task_id = body(created)["data"]["task_id"].get<std::string>();
    CHECK(api.Get("/api/v1/downloads/" + task_id)->status == 200);
    CHECK(api.Post("/api/v1/downloads/" + task_id + "/cancel", "", "application/json")->status == 200);
    CHECK(body(api.Get("/api/v1/downloads/" + task_id))["data"]["status"] == "canceled");
}
