// Real account selection, Kugou adapter, durable downloads and HTTP API; synthetic upstream only.
#include "service_harness.hpp"
#include "providers/kugou/bundle.hpp"
#include <mutex>

using namespace music_service;
using namespace music_service::test;
namespace {
constexpr auto song = "11111111111111111111111111111111";
constexpr auto cdn = "https://webfs.tx.kugou.com/audio.mp3";
constexpr auto cookie_a = "KugooID=12345; t=synthetic-a; kg_mid=mid-a; kg_dfid=dfid-a";
constexpr auto cookie_a2 = "KugooID=12345; t=synthetic-a2; kg_mid=mid-a; kg_dfid=dfid-a";
constexpr auto cookie_b = "KugooID=23456; t=synthetic-b; kg_mid=mid-b; kg_dfid=dfid-b";

class KugouTransport final : public ports::HttpTransport {
public:
    KugouTransport() {
        for (int i = 0; i < 115; ++i) {
            std::string frame(417, '\x55');
            frame[0] = '\xFF'; frame[1] = '\xFB'; frame[2] = '\x90'; frame[3] = '\x40';
            audio_ += frame;
        }
    }
    std::vector<ports::HttpRequest> requests() const {
        std::lock_guard lock{mutex_};
        return requests_;
    }
    domain::Result<ports::HttpResponse> request(const ports::HttpRequest& request,
            const ports::UpstreamLease&, const ports::OperationContext&) override {
        record(request);
        json data;
        if (request.url.starts_with("https://wwwapi.kugou.com/play/songinfo?")) {
            data = {{"status", 1}, {"err_code", 0}, {"data", {
                {"hash", song}, {"encode_album_audio_id", "encodedSong"}, {"is_free_part", 0},
                {"play_url", request.url.find("encode_album_audio_id=") != std::string::npos ? cdn : ""},
                {"bitrate", 128}, {"timelength", 3000}, {"filesize", audio_.size()}}}};
        } else if (request.url.starts_with("https://m.kugou.com/app/i/getSongInfo.php?")) {
            data = {{"hash", song}, {"req_hash", song}, {"status", 1}, {"errcode", 0},
                {"songName", "Synthetic song"}, {"author_name", "Synthetic artist"}, {"albumid", 0},
                {"timeLength", 3}, {"fileSize", audio_.size()}, {"bitRate", 128}, {"extName", "mp3"},
                {"fileHead", 0}, {"pay_type", 3}, {"url", cdn},
                {"extra", {{"128hash", song}, {"128timelength", 3000}}}};
        } else {
            return domain::Error{domain::ErrorCode::upstream_failed, "unexpected synthetic request", false, {}};
        }
        return ports::HttpResponse{{200, {{"Content-Type", "application/json"}}, {}}, data.dump()};
    }
    domain::Result<ports::HttpResponseHead> receive(const ports::HttpRequest& request,
            const ports::UpstreamLease&, const ports::OperationContext& context,
            const ports::HeadConsumer& on_head, const ports::ChunkConsumer& on_chunk) override {
        record(request);
        if (request.url != cdn)
            return domain::Error{domain::ErrorCode::upstream_failed, "unexpected synthetic CDN", false, {}};
        if (context.stop.stop_requested())
            return domain::Error{domain::ErrorCode::canceled, "synthetic cancellation", false, {}};
        ports::HttpResponseHead head{200, {{"Content-Type", "audio/mpeg"},
            {"Content-Length", std::to_string(audio_.size())}, {"ETag", "\"synthetic\""}}, {}};
        if (on_head(head) && !on_chunk(std::span<const char>{audio_.data(), audio_.size()}))
            head.stopped_by_consumer = true;
        return head;
    }
private:
    void record(const ports::HttpRequest& request) { std::lock_guard lock{mutex_}; requests_.push_back(request); }
    mutable std::mutex mutex_;
    std::vector<ports::HttpRequest> requests_;
    std::string audio_;
};
json wait_task(httplib::Client& api, const std::string& id) {
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    json task;
    do {
        task = body(api.Get("/api/v1/downloads/" + id))["data"];
        if (task["status"] == "completed" || task["status"] == "failed" || task["status"] == "canceled") return task;
        std::this_thread::sleep_for(20ms);
    } while (std::chrono::steady_clock::now() < deadline);
    FAIL("synthetic download did not finish");
}
}

TEST_CASE("Kugou Cookie playback and downloads retain accounts across defaults rotation and restart", "[integration][kugou][cookie-audio]") {
    Deployment d{"kugou-cookie-audio"};
    d.config["shutdown"] = {{"soft_seconds", 1}};
    d.write_config();
    server::CommandHooks hooks;
    hooks.host.bundles = std::vector<ports::ProviderBundle>{providers::kugou::make_bundle()};
    REQUIRE(d.run({"init"}, hooks).code == server::exit_ok);
    const auto token = d.read(d.bootstrap());
    const auto transport = std::make_shared<KugouTransport>();
    const auto sink = std::make_shared<infrastructure::observability::MemorySink>();
    auto start = [&](bool paused) {
        return std::make_unique<RunningService>(d, sink,
            std::vector<ports::ProviderBundle>{providers::kugou::make_bundle()}, paused, transport);
    };
    auto service = start(true);
    auto api = service->client(token);
    const json request{{"track", {{"source", "kugou"}, {"id", song}}}};
    auto download = request;
    download["metadata_policy"] = "skip";
    const auto latest_cookie = [&]() -> std::string {
        const auto calls = transport->requests();
        for (auto it = calls.rbegin(); it != calls.rend(); ++it) {
            if (it->operation != "resolve_audio") continue;
            return it->secret_headers.empty() ? "" : it->secret_headers.front().value;
        }
        FAIL("missing synthetic resolution");
    };
    const auto create = [&](const char* cookie) {
        const auto response = api.Post("/api/v1/credentials", json{{"source", "kugou"}, {"kind", "cookie"},
            {"auth_profile", "web_cookie"}, {"secret", {{"cookie", cookie}}}}.dump(), "application/json");
        REQUIRE(response->status == 201);
        return body(response)["data"]["credential_id"].get<std::string>();
    };
    REQUIRE(api.Post("/api/v1/assets/resolve", request.dump(), "application/json")->status == 200);
    CHECK(latest_cookie().empty());
    const auto a = create(cookie_a);
    REQUIRE(api.Put("/api/v1/credential-defaults/kugou", json{{"credential_id", a}}.dump(), "application/json")->status == 200);
    const auto asset_a = api.Post("/api/v1/assets/resolve", request.dump(), "application/json");
    REQUIRE(asset_a->status == 200);
    CHECK(latest_cookie() == cookie_a);
    CHECK(asset_a->body.find("synthetic-a") == std::string::npos);
    const auto accepted = api.Post("/api/v1/downloads", {{"Idempotency-Key", "kugou-cookie-account-a"}}, download.dump(), "application/json");
    INFO(accepted->body);
    REQUIRE(accepted->status == 202);
    const auto task_a = body(accepted)["data"]["task_id"].get<std::string>();
    CHECK(body(api.Get("/api/v1/downloads/" + task_a))["data"]["status"] == "queued");
    // Creating B selects B and disables A under the current single-active-account contract.
    const auto b = create(cookie_b);
    REQUIRE(api.Post("/api/v1/assets/resolve", request.dump(), "application/json")->status == 200);
    CHECK(latest_cookie() == cookie_b);
    REQUIRE(service->stop().code == server::exit_ok);
    service.reset();
    service = start(false);
    auto restarted = service->client(token);
    const auto disabled_a = wait_task(restarted, task_a);
    REQUIRE(disabled_a["status"] == "failed");
    CHECK(disabled_a["error"]["code"] == "CREDENTIALS_REQUIRED");
    CHECK(latest_cookie() == cookie_b); // The old task never switches to B or anonymous.
    REQUIRE(restarted.Put("/api/v1/credential-defaults/kugou",
        json{{"credential_id", a}, {"secret", {{"cookie", cookie_a}}}}.dump(), "application/json")->status == 200);
    REQUIRE(restarted.Put("/api/v1/credentials/" + a,
        json{{"secret", {{"cookie", cookie_a2}}}}.dump(), "application/json")->status == 200);
    download["credential_id"] = a;
    const auto new_a = restarted.Post("/api/v1/downloads", {{"Idempotency-Key", "kugou-cookie-account-a2"}}, download.dump(), "application/json");
    REQUIRE(new_a->status == 202);
    const auto done_a = wait_task(restarted, body(new_a)["data"]["task_id"].get<std::string>());
    REQUIRE(done_a["status"] == "completed");
    CHECK(latest_cookie() == cookie_a2);
    REQUIRE(restarted.Put("/api/v1/credential-defaults/kugou",
        json{{"credential_id", b}, {"secret", {{"cookie", cookie_b}}}}.dump(), "application/json")->status == 200);
    download["credential_id"] = b;
    const auto explicit_b = restarted.Post("/api/v1/downloads", {{"Idempotency-Key", "kugou-cookie-account-b"}}, download.dump(), "application/json");
    REQUIRE(explicit_b->status == 202);
    CHECK_FALSE(body(explicit_b)["data"]["reused"].get<bool>());
    const auto done_b = wait_task(restarted, body(explicit_b)["data"]["task_id"].get<std::string>());
    REQUIRE(done_b["status"] == "completed");
    CHECK(latest_cookie() == cookie_b);
    CHECK(done_a["result"]["file_id"] != done_b["result"]["file_id"]);
    REQUIRE(restarted.Delete("/api/v1/credential-defaults/kugou")->status == 200);
    REQUIRE(restarted.Post("/api/v1/assets/resolve", request.dump(), "application/json")->status == 200);
    CHECK(latest_cookie().empty());
    bool downloaded = false;
    for (const auto& call : transport->requests()) {
        if (call.url == cdn) {
            downloaded = true;
            CHECK(call.secret_headers.empty());
            for (const auto& header : call.headers) CHECK(header.name != "Cookie");
        } else if (!call.secret_headers.empty()) {
            CHECK(call.url.starts_with("https://wwwapi.kugou.com/play/songinfo?"));
            CHECK_FALSE(call.allow_redirects);
        }
    }
    CHECK(downloaded);
    service->stop();
    std::ostringstream logs;
    infrastructure::observability::JsonLineSink log_writer{logs};
    for (const auto& record : sink->records()) log_writer.write(record);
    CHECK(logs.str().find("synthetic-a") == std::string::npos);
    CHECK(logs.str().find("synthetic-b") == std::string::npos);
}
