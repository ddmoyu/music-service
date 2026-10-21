// Download execution on real SQLite, the real admission ledger and upstream client, the
// baseline audio processor and the Finalizer, with a scripted HTTP transport (T17): the
// complete run, unknown lengths (A25), admission pauses and attempts (A43, A44), late
// writes of old executions (A22), budgets that never restart (A26), cancel, shutdown
// checkpoints and resume (A27), fair dispatch and the pause policy.

#include "../unit/providers/mock_providers.hpp"

#include "application/accounts/credential_service.hpp"
#include "application/transport/upstream_client.hpp"
#include "audio_processing/verifying_processor.hpp"
#include "downloads/runner.hpp"
#include "downloads/scheduler.hpp"
#include "infrastructure/clock.hpp"
#include "infrastructure/filesystem/destination_files.hpp"
#include "infrastructure/filesystem/paths.hpp"
#include "infrastructure/observability/correlation.hpp"
#include "infrastructure/secrets/file_secret_store.hpp"
#include "infrastructure/storage/sqlite/account_repositories.hpp"
#include "infrastructure/storage/sqlite/migrations.hpp"
#include "infrastructure/storage/sqlite/task_repositories.hpp"
#include "providers/registry/provider_registry.hpp"
#include "runtime/admission/admission_ledger.hpp"
#include "runtime/executors/bounded_executor.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>

using namespace music_service;
using namespace music_service::test;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace sqlite = infrastructure::storage::sqlite;
using domain::ErrorCode;
using domain::TaskStatus;
using downloads::RunOutcome;
using nlohmann::json;

namespace {

// 26.122 ms per 128 kbit/s frame at 44.1 kHz: 1149 frames are 30.01 s.
std::string mp3(std::size_t frames = 1149) {
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

std::string read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

// One scripted answer of the audio server.
struct Reply {
    int status{200};
    std::vector<ports::HttpHeader> headers{};
    std::string body{};
    std::optional<std::size_t> reset_after{}; // bytes, then a retryable transport failure
    bool hold{false};                         // after the first chunk, wait for the stop request
};

// The audio server: answers per request, streams 16 KiB chunks and honors the stop token
// as the real transport does.
class ScriptedTransport final : public ports::HttpTransport {
public:
    std::function<Reply(const ports::HttpRequest&)> answer;
    std::atomic<bool> holding{false};
    // Called after each delivered chunk with the bytes sent so far (the runner has handled it).
    std::function<void(std::size_t)> after_chunk;

    domain::Result<ports::HttpResponse> request(const ports::HttpRequest&, const ports::UpstreamLease&,
                                                const ports::OperationContext&) override {
        return domain::Error{ErrorCode::upstream_failed, "not scripted", false, {}};
    }

    domain::Result<ports::HttpResponseHead> receive(const ports::HttpRequest& request, const ports::UpstreamLease&,
                                                    const ports::OperationContext& context,
                                                    const ports::HeadConsumer& on_head,
                                                    const ports::ChunkConsumer& on_chunk) override {
        {
            const std::lock_guard lock{mutex_};
            requests_.push_back(request);
        }
        const auto reply = answer(request);
        ports::HttpResponseHead head{reply.status, reply.headers, request.url};
        if (!on_head(head)) {
            return head;
        }
        std::size_t sent = 0;
        while (sent < reply.body.size()) {
            if (context.stop.stop_requested()) {
                return domain::Error{ErrorCode::canceled, "the operation was canceled", false, {}};
            }
            if (reply.reset_after && sent >= *reply.reset_after) {
                return domain::Error{ErrorCode::upstream_failed, "connection reset", true, {{"transport", "reset"}}};
            }
            if (reply.hold && sent > 0) {
                holding = true;
                while (!context.stop.stop_requested()) {
                    std::this_thread::sleep_for(5ms);
                }
                holding = false;
                return domain::Error{ErrorCode::canceled, "the operation was canceled", false, {}};
            }
            auto size = std::min<std::size_t>(16 * 1024, reply.body.size() - sent);
            if (reply.reset_after) {
                size = std::min(size, *reply.reset_after - sent);
            }
            if (!on_chunk({reply.body.data() + sent, size})) {
                head.stopped_by_consumer = true;
                return head;
            }
            sent += size;
            if (after_chunk) {
                after_chunk(sent);
            }
        }
        return head;
    }

    std::vector<ports::HttpRequest> requests() const {
        const std::lock_guard lock{mutex_};
        return requests_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<ports::HttpRequest> requests_;
};

std::optional<std::string> header(const ports::HttpRequest& request, std::string_view name) {
    for (const auto& h : request.headers) {
        if (h.name == name) {
            return h.value;
        }
    }
    return std::nullopt;
}

// The whole file, or the asked-for range of it, with a strong validator.
Reply serve(const std::string& bytes, std::string etag = "\"v1\"") {
    return Reply{
        200, {{"Content-Length", std::to_string(bytes.size())}, {"ETag", etag}, {"Content-Type", "audio/mpeg"}}, bytes};
}

Reply serve_range(const ports::HttpRequest& request, const std::string& bytes) {
    const auto range = header(request, "Range");
    if (!range) {
        return serve(bytes);
    }
    const auto from = std::stoull(range->substr(6));
    return Reply{206,
                 {{"Content-Range", "bytes " + std::to_string(from) + "-" + std::to_string(bytes.size() - 1) + "/" +
                                        std::to_string(bytes.size())},
                  {"ETag", "\"v1\""}},
                 bytes.substr(from)};
}

// Resolves every track to https://cdn.test/<source>/<id>.mp3, 30 s long, unless told otherwise.
struct ResolverScript {
    std::atomic<int> calls{0};
    std::function<domain::Result<ports::ResolvedAudio>(const domain::TrackRef&)> answer;
};

class ScriptedResolver final : public ports::IAssetResolver {
public:
    explicit ScriptedResolver(std::shared_ptr<ResolverScript> script) : script_(std::move(script)) {}

    domain::Result<ports::ResolvedAudio> resolve_audio(const ports::ProviderContext&, const domain::TrackRef& track,
                                                       const domain::ResourceRequirement&) override {
        ++script_->calls;
        if (script_->answer) {
            return script_->answer(track);
        }
        return ports::ResolvedAudio{track,
                                    domain::Quality::standard,
                                    domain::Completeness::unknown,
                                    "mp3",
                                    128000,
                                    "https://cdn.test/" + track.source.value() + "/" + track.id.value() + ".mp3",
                                    {},
                                    {},
                                    30010,
                                    ports::ProcessingPlan{}};
    }

private:
    std::shared_ptr<ResolverScript> script_;
};

// Catalog (search, detail "detail") plus anonymous audio from the script.
ports::ProviderBundle scripted(std::string_view name, std::shared_ptr<ResolverScript> script) {
    auto bundle = mock_catalog(name);
    bundle.capabilities.push_back({.key = "resolve_audio"});
    bundle.bindings.push_back(ports::CapabilityBinding::bind<ports::IAssetResolver>(
        [script](const ports::ProviderSetup&) -> domain::Result<std::shared_ptr<ports::IAssetResolver>> {
            return std::shared_ptr<ports::IAssetResolver>{std::make_shared<ScriptedResolver>(script)};
        }));
    return bundle;
}

downloads::RunnerOptions quick_options() {
    downloads::RunnerOptions options;
    options.initial_backoff = 1ms;
    options.progress_interval = 0ms; // a checkpoint per chunk
    options.admission_retry = 0ms;
    return options;
}

struct Runs {
    explicit Runs(std::string_view name, downloads::RunnerOptions options = quick_options(),
                  runtime::AdmissionLimits limits = {})
        : dir(fs::temp_directory_path() /
              infrastructure::filesystem::path_from_utf8("music-service-执行-" + std::string{name})) {
        fs::remove_all(dir);
        fs::create_directories(dir / "secrets");
        fs::create_directories(dir / "下载");
        root = unwrap(infrastructure::filesystem::canonical_directory(dir / "下载"));
        database = sqlite::Database::open(dir / "service.sqlite3", {}).value();
        REQUIRE(sqlite::migrate(*database, clock->now()));
        const sqlite::RepositoryContext ctx{database.get(), clock, ids};
        identities = std::make_shared<sqlite::SqliteIdentityRepository>(ctx);
        destinations = std::make_shared<sqlite::SqliteDestinationRepository>(ctx);
        tasks = std::make_shared<sqlite::SqliteTaskRepository>(ctx, sqlite::TaskRepositoryOptions{});
        publications = std::make_shared<sqlite::SqlitePublicationRepository>(ctx);
        REQUIRE(destinations->bind("dst_main", infrastructure::filesystem::path_to_utf8(root)));
        owner = unwrap(identities->create_principal("owner", ports::PrincipalRole::user, std::nullopt)).id;
        other = unwrap(identities->create_principal("other", ports::PrincipalRole::user, std::nullopt)).id;

        registry = unwrap(providers::ProviderRegistry::build({scripted("src", script), scripted("busy", script)}, {}));
        credentials = std::make_shared<application::accounts::CredentialService>(
            std::make_shared<sqlite::SqliteCredentialRepository>(ctx),
            std::make_shared<infrastructure::secrets::FileSecretStore>(dir / "secrets"), registry, clock);
        ledger = std::make_shared<runtime::AdmissionLedger>(limits, clock);
        transport->answer = [this](const ports::HttpRequest& request) { return serve_range(request, audio); };
        files = std::make_shared<infrastructure::filesystem::LocalDestinationFiles>(
            std::map<std::string, fs::path>{{"dst_main", root}});
        processing = std::make_shared<runtime::BoundedExecutor>("processing", 1, 4);
        auto processors =
            std::make_shared<audio_processing::ProcessorCatalog>(audio_processing::ProcessorCatalog::builtin());

        downloads::RunnerServices services;
        services.tasks = tasks;
        services.providers = registry;
        services.credentials = credentials;
        services.upstream = std::make_shared<application::transport::UpstreamClient>(ledger, transport, clock);
        services.work_files = files;
        services.files = files;
        services.finalizer = std::make_shared<downloads::Finalizer>(publications, files);
        services.processors = [processors](const ports::ProcessingPlan& plan) { return processors->find(plan); };
        services.processing = processing;
        services.clock = clock;
        services.file_name = [](std::string_view text) { return infrastructure::filesystem::sanitize_file_name(text); };
        runner = std::make_shared<downloads::DownloadRunner>(options, std::move(services));
    }
    ~Runs() {
        processing->stop_accepting();
        processing->request_stop();
        processing->join();
        runner.reset();
        database.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    std::string create(std::string_view source = "src", std::string_view id = "1", std::string who = {}) {
        if (who.empty()) {
            who = owner;
        }
        domain::DownloadIntentDraft draft{who,
                                          track(source, id),
                                          domain::SameSource{},
                                          {anonymous(who, source)},
                                          {domain::Quality::standard, domain::QualityPolicy::strict, false},
                                          domain::MetadataPolicy::best_effort,
                                          {"dst_main", "d1", "n1", "m1"}};
        auto intent = unwrap(domain::NormalizedDownloadIntent::normalize(std::move(draft)));
        const auto key = "key-" + std::string{source} + "-" + std::string{id} + "-" + who;
        return unwrap(tasks->create({std::move(intent), key, "h", false})).task.task_id;
    }

    // Claims the oldest due task (now may lie in the future to skip a retry wait) and runs it.
    downloads::RunReport run_next(std::chrono::system_clock::time_point now = {}) {
        auto claimed = unwrap(tasks->claim_next(now == decltype(now){} ? clock->now() : now));
        REQUIRE(claimed);
        downloads::ExecutionControl control;
        return runner->run(*claimed, control);
    }

    ports::TaskRecord get(const std::string& task_id, const std::string& who = {}) {
        return unwrap(tasks->get(who.empty() ? owner : who, task_id));
    }
    json progress(const ports::TaskRecord& task) const { return json::parse(task.progress_json.value_or("null")); }

    fs::path dir;
    fs::path root;
    std::shared_ptr<infrastructure::SystemClock> clock = std::make_shared<infrastructure::SystemClock>();
    std::shared_ptr<infrastructure::observability::IdGenerator> ids =
        std::make_shared<infrastructure::observability::IdGenerator>(16);
    std::unique_ptr<sqlite::Database> database;
    std::shared_ptr<sqlite::SqliteIdentityRepository> identities;
    std::shared_ptr<sqlite::SqliteDestinationRepository> destinations;
    std::shared_ptr<sqlite::SqliteTaskRepository> tasks;
    std::shared_ptr<sqlite::SqlitePublicationRepository> publications;
    std::shared_ptr<const providers::ProviderRegistry> registry;
    std::shared_ptr<application::accounts::CredentialService> credentials;
    std::shared_ptr<runtime::AdmissionLedger> ledger;
    std::shared_ptr<ScriptedTransport> transport = std::make_shared<ScriptedTransport>();
    std::shared_ptr<ResolverScript> script = std::make_shared<ResolverScript>();
    std::shared_ptr<infrastructure::filesystem::LocalDestinationFiles> files;
    std::shared_ptr<runtime::BoundedExecutor> processing;
    std::shared_ptr<downloads::DownloadRunner> runner;
    std::string owner;
    std::string other;
    std::string audio = mp3();
};

template <class Predicate> bool eventually(Predicate&& done, std::chrono::milliseconds budget = 10s) {
    const auto until = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < until) {
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return done();
}

ports::AdmissionRequest download_slot(const std::string& owner, std::string_view source) {
    return ports::AdmissionRequest{owner,        unwrap(domain::SourceId::parse(source)),
                                   "test",       ports::TrafficClass::download,
                                   std::nullopt, "https://cdn.test",
                                   {},           std::chrono::steady_clock::now() + 1min};
}

} // namespace

TEST_CASE("a claimed task is downloaded, verified and published", "[downloads][runner]") {
    Runs r{"complete"};
    const auto id = r.create();
    const auto report = r.run_next();
    CHECK(report.outcome == RunOutcome::completed);

    const auto task = r.get(id);
    CHECK(task.status == TaskStatus::completed);
    CHECK(task.attempt == 1);
    CHECK_FALSE(task.attempt_open);
    REQUIRE(task.deadline_at);
    CHECK(*task.deadline_at > task.created_at);
    const auto result = json::parse(*task.result_json);
    CHECK(result["file_id"].get<std::string>().rfind("fil_", 0) == 0);
    CHECK(result["processing"]["completeness"] == "full"); // measured, the provider did not know
    const auto execution = json::parse(*task.execution_json);
    CHECK(execution["actual_track"] == json{{"source", "src"}, {"id", "1"}});
    CHECK(execution["actual_quality"] == "standard");
    const auto progress = r.progress(task);
    CHECK(progress["received_bytes"] == r.audio.size());
    CHECK(progress["total_bytes"] == r.audio.size());

    // The published file holds exactly the bytes; the work directory is gone.
    CHECK(read_bytes(r.root / "detail.mp3") == r.audio);
    CHECK_FALSE(fs::exists(r.root / ".music-service-work" / infrastructure::filesystem::path_from_utf8(id)));
    const auto asset = unwrap(r.publications->get_asset(r.owner, result["file_id"].get<std::string>()));
    CHECK(asset.relative_path == "detail.mp3");
    CHECK(json::parse(asset.properties_json)["actual_track"]["id"] == "1");

    // The same name again gets the next candidate, never the existing file.
    r.create("src", "2");
    CHECK(r.run_next().outcome == RunOutcome::completed);
    CHECK(read_bytes(r.root / "detail (2).mp3") == r.audio);
}

TEST_CASE("an unknown length stays null while transferring (A25)", "[downloads][runner]") {
    Runs r{"unknown-length"};
    r.transport->answer = [&](const ports::HttpRequest&) {
        return Reply{200, {{"Content-Type", "audio/mpeg"}}, r.audio}; // no Content-Length, no validator
    };
    const auto id = r.create();
    std::vector<json> seen; // the committed progress while bytes are still arriving
    r.transport->after_chunk = [&](std::size_t sent) {
        if (sent < r.audio.size()) {
            seen.push_back(r.progress(r.get(id)));
        }
    };
    CHECK(r.run_next().outcome == RunOutcome::completed);
    REQUIRE(seen.size() > 2);
    for (const auto& progress : seen) {
        CHECK(progress["received_bytes"].get<std::uint64_t>() > 0);
        CHECK(progress["total_bytes"].is_null());
        CHECK(progress["eta_ms"].is_null());
        CHECK(progress["stage_progress"].is_null()); // no invented percentage
    }
    // Once the body ended the length is known.
    CHECK(r.progress(r.get(id))["total_bytes"] == r.audio.size());
}

TEST_CASE("progress reports bytes per second and an ETA in milliseconds (A50)", "[downloads][runner]") {
    // The public projection the runner writes, independent of timing.
    Runs r{"progress"};
    r.transport->answer = [&](const ports::HttpRequest& request) { return serve_range(request, r.audio); };
    const auto id = r.create();
    REQUIRE(r.run_next().outcome == RunOutcome::completed);
    const auto progress = r.progress(r.get(id));
    for (const auto* key : {"received_bytes", "total_bytes", "speed_bytes_per_second", "eta_ms", "stage_progress"}) {
        CHECK(progress.contains(key));
    }
}

TEST_CASE("admission pauses keep the attempt, new executions refuse old writes (A22, A43, A44, A26)",
          "[downloads][runner]") {
    runtime::AdmissionLimits limits;
    limits.per_source[ports::TrafficClass::download] = 1;
    Runs r{"admission", quick_options(), limits};
    const auto id = r.create();

    // Another transfer of this source holds the only download slot.
    auto held = unwrap(r.ledger->try_acquire(download_slot(r.other, "src")));
    REQUIRE(std::holds_alternative<ports::UpstreamLease>(held));

    auto claimed = *unwrap(r.tasks->claim_next(r.clock->now()));
    const auto first_execution = *claimed.execution_id;
    downloads::ExecutionControl control;
    CHECK(r.runner->run(claimed, control).outcome == RunOutcome::waiting_admission);
    auto task = r.get(id);
    CHECK(task.status == TaskStatus::waiting_retry);
    CHECK(task.wait_reason == domain::WaitReason::admission);
    CHECK(task.resume_stage == TaskStatus::downloading);
    CHECK(task.attempt == 1);
    CHECK(task.attempt_open);
    REQUIRE(task.deadline_at);
    const auto deadline = *task.deadline_at;
    CHECK(r.ledger->usage().total == 1); // only the held lease: nothing leaked

    // Claimed again while still blocked: a new execution, the same attempt and deadline.
    claimed = *unwrap(r.tasks->claim_next(r.clock->now()));
    CHECK(*claimed.execution_id != first_execution);
    CHECK(r.runner->run(claimed, control).outcome == RunOutcome::waiting_admission);
    task = r.get(id);
    CHECK(task.attempt == 1);
    CHECK(task.deadline_at == deadline);

    // A late write of the first execution is refused.
    const auto late = r.tasks->checkpoint({id, first_execution, task.version, R"({"received_bytes":1})"});
    REQUIRE_FALSE(late);
    CHECK(late.error().code == ErrorCode::version_conflict);

    // The slot is free; the server fails for real: that attempt ends and is counted.
    std::get<ports::UpstreamLease>(held).release();
    r.transport->answer = [](const ports::HttpRequest&) { return Reply{503, {}, {}}; };
    CHECK(r.run_next().outcome == RunOutcome::waiting_retry);
    task = r.get(id);
    CHECK(task.wait_reason == domain::WaitReason::upstream_retry);
    CHECK(task.attempt == 1);
    CHECK_FALSE(task.attempt_open);

    // The next attempt counts, and the deadline still is the first one.
    r.transport->answer = [&](const ports::HttpRequest& request) { return serve_range(request, r.audio); };
    CHECK(r.run_next(r.clock->now() + 1h).outcome == RunOutcome::completed);
    task = r.get(id);
    CHECK(task.attempt == 2);
    CHECK(task.deadline_at == deadline);
}

TEST_CASE("attempts and the absolute deadline are budgets, not restarts (A26)", "[downloads][runner]") {
    SECTION("attempts run out") {
        auto options = quick_options();
        options.max_attempts = 2;
        Runs r{"attempts", options};
        r.transport->answer = [](const ports::HttpRequest&) { return Reply{429, {}, {}}; };
        const auto id = r.create();
        CHECK(r.run_next().outcome == RunOutcome::waiting_retry);
        CHECK(r.run_next(r.clock->now() + 1h).outcome == RunOutcome::failed);
        const auto task = r.get(id);
        CHECK(task.status == TaskStatus::failed);
        CHECK(task.attempt == 2);
        CHECK(json::parse(*task.error_json)["code"] == "UPSTREAM_RATE_LIMITED");
    }
    SECTION("the deadline passes while the task waits") {
        auto options = quick_options();
        options.absolute_budget = 1s;
        runtime::AdmissionLimits limits;
        limits.per_source[ports::TrafficClass::download] = 1;
        Runs r{"deadline", options, limits};
        const auto id = r.create();
        auto held = unwrap(r.ledger->try_acquire(download_slot(r.other, "src")));
        CHECK(r.run_next().outcome == RunOutcome::waiting_admission);
        std::this_thread::sleep_for(1100ms); // the pause counts against the absolute budget
        std::get<ports::UpstreamLease>(held).release();
        CHECK(r.run_next().outcome == RunOutcome::failed);
        CHECK(json::parse(*r.get(id).error_json)["code"] == "REQUEST_DEADLINE_EXCEEDED");
    }
}

TEST_CASE("an interrupted transfer resumes from flushed bytes with its validator", "[downloads][runner]") {
    Runs r{"resume"};
    int calls = 0;
    r.transport->answer = [&](const ports::HttpRequest& request) {
        ++calls;
        auto reply = serve_range(request, r.audio);
        if (calls == 1) {
            reply.reset_after = 100000; // the connection drops mid-way
        }
        return reply;
    };
    const auto id = r.create();
    CHECK(r.run_next().outcome == RunOutcome::completed);
    const auto requests = r.transport->requests();
    REQUIRE(requests.size() == 2);
    CHECK(header(requests[1], "Range") == "bytes=100000-");
    CHECK(header(requests[1], "If-Range") == "\"v1\"");
    CHECK(read_bytes(r.root / "detail.mp3") == r.audio);
    CHECK(r.get(id).attempt == 1); // transport retries stay within the attempt
}

TEST_CASE("a 200 answer to a range request replaces the partial file", "[downloads][runner]") {
    Runs r{"restart"};
    int calls = 0;
    r.transport->answer = [&](const ports::HttpRequest&) {
        auto reply = serve(r.audio); // ignores Range
        if (++calls == 1) {
            reply.reset_after = 50000;
        }
        return reply;
    };
    r.create();
    CHECK(r.run_next().outcome == RunOutcome::completed);
    CHECK(read_bytes(r.root / "detail.mp3") == r.audio); // never appended to the old prefix
}

TEST_CASE("a 206 of another range or resource is not mixed into the file (A14)", "[downloads][runner]") {
    SECTION("the range starts elsewhere") {
        Runs r{"range-start"};
        int calls = 0;
        r.transport->answer = [&](const ports::HttpRequest& request) {
            ++calls;
            if (calls == 1) {
                auto reply = serve(r.audio);
                reply.reset_after = 80000;
                return reply;
            }
            if (header(request, "Range")) {
                auto reply = serve_range(request, r.audio);
                reply.headers[0].value = "bytes 0-" + std::to_string(r.audio.size() - 1) + "/" +
                                         std::to_string(r.audio.size());
                reply.body = r.audio; // the whole file, labeled as a range from 0
                return reply;
            }
            return serve(r.audio);
        };
        r.create();
        CHECK(r.run_next().outcome == RunOutcome::completed);
        const auto requests = r.transport->requests();
        REQUIRE(requests.size() == 3);
        CHECK_FALSE(header(requests[2], "Range")); // started over without a range
        CHECK(read_bytes(r.root / "detail.mp3") == r.audio);
    }
    SECTION("the validator changed") {
        Runs r{"range-etag"};
        const auto other = mp3(1150); // another encoding of the track
        int calls = 0;
        r.transport->answer = [&](const ports::HttpRequest& request) {
            if (++calls == 1) {
                auto reply = serve(r.audio);
                reply.reset_after = 80000;
                return reply;
            }
            if (const auto range = header(request, "Range")) {
                const auto from = std::stoull(range->substr(6));
                return Reply{206,
                             {{"Content-Range", "bytes " + std::to_string(from) + "-" +
                                                    std::to_string(r.audio.size() - 1) + "/" +
                                                    std::to_string(r.audio.size())},
                              {"ETag", "\"v2\""}},
                             other.substr(from, r.audio.size() - from)};
            }
            return serve(other, "\"v2\"");
        };
        r.create();
        CHECK(r.run_next().outcome == RunOutcome::completed);
        CHECK(read_bytes(r.root / "detail.mp3") == other); // only bytes of one resource
    }
}

TEST_CASE("a 416 completes only with matching evidence, otherwise starts over (A15)", "[downloads][runner]") {
    Runs r{"unsatisfiable"};
    int calls = 0;
    r.transport->answer = [&](const ports::HttpRequest& request) {
        if (++calls == 1) {
            auto reply = serve_range(request, r.audio);
            reply.reset_after = 64000;
            return reply;
        }
        if (header(request, "Range")) {
            // The server claims the resource is no longer than what is here.
            return Reply{416, {{"Content-Range", "bytes */64000"}}, {}};
        }
        return serve(r.audio);
    };
    const auto id = r.create();
    CHECK(r.run_next().outcome == RunOutcome::completed);
    const auto requests = r.transport->requests();
    REQUIRE(requests.size() == 3);
    CHECK(header(requests[1], "Range") == "bytes=64000-");
    CHECK_FALSE(header(requests[2], "Range"));
    CHECK(read_bytes(r.root / "detail.mp3") == r.audio); // never published as 64000 bytes
    CHECK(r.get(id).attempt == 1);
}

TEST_CASE("an expired address is resolved again and the same resource resumes (A16)", "[downloads][runner]") {
    Runs r{"expired"};
    int resolved = 0;
    r.script->answer = [&](const domain::TrackRef& t) -> domain::Result<ports::ResolvedAudio> {
        ++resolved;
        return ports::ResolvedAudio{t,
                                    domain::Quality::standard,
                                    domain::Completeness::unknown,
                                    "mp3",
                                    128000,
                                    "https://cdn.test/signed-" + std::to_string(resolved) + ".mp3",
                                    {},
                                    {},
                                    30010,
                                    ports::ProcessingPlan{}};
    };
    int first_url = 0;
    r.transport->answer = [&](const ports::HttpRequest& request) {
        if (request.url == "https://cdn.test/signed-1.mp3") {
            if (++first_url == 1) {
                auto reply = serve_range(request, r.audio);
                reply.reset_after = 90000;
                return reply;
            }
            return Reply{403, {{"Content-Type", "text/html"}}, "expired"}; // the signature ran out
        }
        return serve_range(request, r.audio);
    };
    const auto id = r.create();
    const auto first = r.run_next();
    CHECK(first.outcome == RunOutcome::waiting_retry);
    auto task = r.get(id);
    CHECK(task.wait_reason == domain::WaitReason::upstream_retry);
    CHECK(task.resume_stage == TaskStatus::resolving);
    REQUIRE(task.next_retry_at);
    CHECK(r.run_next(*task.next_retry_at).outcome == RunOutcome::completed);
    CHECK(resolved == 2);
    const auto requests = r.transport->requests();
    const auto& last = requests.back();
    CHECK(last.url == "https://cdn.test/signed-2.mp3");
    CHECK(header(last, "Range") == "bytes=90000-"); // same track, quality and validator
    CHECK(header(last, "If-Range") == "\"v1\"");
    CHECK(read_bytes(r.root / "detail.mp3") == r.audio);
    CHECK(r.get(id).attempt == 2);
}

TEST_CASE("Retry-After moves the next attempt later, within its cap", "[downloads][runner]") {
    auto options = quick_options();
    options.max_retry_after = 120s;
    runtime::AdmissionLimits limits;
    limits.max_cooldown = 0s; // only the task's own wait is under test here
    Runs r{"retry-after", options, limits};
    std::string wait = "30";
    r.transport->answer = [&](const ports::HttpRequest&) {
        return Reply{503, {{"Retry-After", wait}}, {}};
    };
    const auto id = r.create();
    auto before = r.clock->now();
    CHECK(r.run_next().outcome == RunOutcome::waiting_retry);
    auto task = r.get(id);
    REQUIRE(task.next_retry_at);
    CHECK(*task.next_retry_at >= before + 30s);
    CHECK(*task.next_retry_at < before + 40s);

    wait = "86400"; // a day: capped
    before = r.clock->now();
    CHECK(r.run_next(*task.next_retry_at).outcome == RunOutcome::waiting_retry);
    task = r.get(id);
    REQUIRE(task.next_retry_at);
    CHECK(*task.next_retry_at >= before + 120s);
    CHECK(*task.next_retry_at < before + 130s);
    CHECK(task.attempt == 2);
}

TEST_CASE("a cut-off or non-audio body is never published", "[downloads][runner]") {
    Runs r{"invalid"};
    r.transport->answer = [](const ports::HttpRequest&) {
        return Reply{200, {{"Content-Type", "text/html"}}, "<html>login required</html>"};
    };
    const auto id = r.create();
    const auto report = r.run_next();
    CHECK((report.outcome == RunOutcome::waiting_retry || report.outcome == RunOutcome::failed));
    CHECK(r.get(id).status != TaskStatus::completed);
    CHECK_FALSE(fs::exists(r.root / "detail.mp3"));
}

TEST_CASE("the scheduler shares slots fairly and skips blocked sources (A43)", "[downloads][scheduler]") {
    Runs r{"scheduler"};
    auto executor = std::make_shared<runtime::BoundedExecutor>("downloads", 1, 1);
    std::mutex order_mutex;
    std::vector<std::string> order;
    downloads::DownloadScheduler scheduler{{1, 20ms},
                                           r.tasks,
                                           r.ledger,
                                           r.runner,
                                           executor,
                                           r.clock,
                                           [&](const ports::TaskRecord& claimed, const downloads::RunReport&) {
                                               const std::lock_guard lock{order_mutex};
                                               order.push_back(claimed.owner_id);
                                           }};

    // "busy" is cooling down after a 429: its task waits, the others run.
    r.ledger->feedback(download_slot(r.owner, "busy"), std::chrono::steady_clock::now() + 1h);
    const auto blocked = r.create("busy", "1");
    r.create("src", "1");
    r.create("src", "2");
    r.create("src", "3", r.other);
    scheduler.set_paused(true);
    scheduler.start();
    std::this_thread::sleep_for(200ms);
    CHECK(scheduler.stats().claimed == 0); // paused: nothing is claimed
    scheduler.set_paused(false);
    REQUIRE(eventually([&] { return scheduler.stats().claimed == 3 && scheduler.stats().running == 0; }));
    {
        const std::lock_guard lock{order_mutex};
        REQUIRE(order.size() == 3);
        CHECK(order[0] != order[1]); // principals take turns
    }
    CHECK(r.get(blocked).status == TaskStatus::queued);

    scheduler.stop_claiming();
    executor->stop_accepting();
    executor->request_stop();
    executor->join();
    scheduler.join();
}

TEST_CASE("a cancel stops the transfer and removes the work files", "[downloads][scheduler]") {
    Runs r{"cancel"};
    r.transport->answer = [&](const ports::HttpRequest& request) {
        auto reply = serve_range(request, r.audio);
        reply.hold = true;
        return reply;
    };
    auto executor = std::make_shared<runtime::BoundedExecutor>("downloads", 1, 1);
    downloads::DownloadScheduler scheduler{{1, 20ms}, r.tasks, r.ledger, r.runner, executor, r.clock};
    const auto id = r.create();
    scheduler.start();
    REQUIRE(eventually([&] { return r.transport->holding.load(); }));
    CHECK(fs::exists(r.root / ".music-service-work" / infrastructure::filesystem::path_from_utf8(id)));

    // The cancel commits first, then the runner is told; it finishes the cancel.
    CHECK(unwrap(r.tasks->request_cancel(r.owner, id)).status == TaskStatus::canceling);
    scheduler.request_stop(id);
    REQUIRE(eventually([&] { return r.get(id).status == TaskStatus::canceled; }));
    CHECK_FALSE(fs::exists(r.root / ".music-service-work" / infrastructure::filesystem::path_from_utf8(id)));
    CHECK(r.ledger->usage().total == 0);

    scheduler.stop_claiming();
    executor->stop_accepting();
    executor->request_stop();
    executor->join();
    scheduler.join();
}

TEST_CASE("a shutdown keeps the checkpoint and the next run resumes it (A27)", "[downloads][scheduler]") {
    Runs r{"shutdown"};
    r.transport->answer = [&](const ports::HttpRequest& request) {
        auto reply = serve_range(request, r.audio);
        reply.hold = true;
        return reply;
    };
    const auto id = r.create();
    {
        auto executor = std::make_shared<runtime::BoundedExecutor>("downloads", 1, 1);
        downloads::DownloadScheduler scheduler{{1, 20ms}, r.tasks, r.ledger, r.runner, executor, r.clock};
        scheduler.start();
        REQUIRE(eventually([&] { return r.transport->holding.load(); }));
        // ProcessHost's order: no new claims, then the running executions pause.
        scheduler.stop_claiming();
        CHECK_FALSE(scheduler.wait_idle(50ms));
        scheduler.stop_running();
        CHECK(scheduler.wait_idle(5s));
        executor->stop_accepting();
        executor->request_stop();
        executor->join();
        scheduler.join();
    }
    auto task = r.get(id);
    CHECK(task.status == TaskStatus::waiting_retry);
    CHECK(task.wait_reason == domain::WaitReason::admission);
    CHECK(task.resume_stage == TaskStatus::downloading);
    CHECK(task.attempt == 1);
    CHECK(task.attempt_open);
    REQUIRE(task.checkpoint_json);
    const auto durable = json::parse(*task.checkpoint_json)["durable_bytes"].get<std::uint64_t>();
    CHECK(durable == 16 * 1024); // the first chunk, flushed before it was counted

    r.transport->answer = [&](const ports::HttpRequest& request) { return serve_range(request, r.audio); };
    CHECK(r.run_next().outcome == RunOutcome::completed);
    CHECK(header(r.transport->requests().back(), "Range") == "bytes=" + std::to_string(durable) + "-");
    CHECK(read_bytes(r.root / "detail.mp3") == r.audio);
    CHECK(r.get(id).attempt == 1);
}

TEST_CASE("executions a crash left running are due again at their resume stage", "[downloads][recovery]") {
    Runs r{"requeue"};
    const auto resolving = r.create("src", "1");
    const auto downloading = r.create("src", "2");
    // Tasks created in the same millisecond have no claim order; find the second one by id.
    auto first = *unwrap(r.tasks->claim_next(r.clock->now()));
    auto second = *unwrap(r.tasks->claim_next(r.clock->now()));
    if (first.task_id == downloading) {
        std::swap(first, second);
    }
    REQUIRE(second.task_id == downloading);
    second = unwrap(r.tasks->transition(
        {second.task_id, *second.execution_id, second.version, TaskStatus::downloading, {}, {}, {}}));
    second = unwrap(r.tasks->transition(
        {second.task_id, *second.execution_id, second.version, TaskStatus::processing, {}, {}, {}}));
    (void)first;

    const auto requeued = unwrap(r.tasks->requeue_interrupted(r.clock->now()));
    CHECK(requeued.size() == 2);
    CHECK(r.get(resolving).resume_stage == TaskStatus::resolving);
    CHECK(r.get(downloading).resume_stage == TaskStatus::downloading); // processing re-reads its file
    CHECK(r.get(downloading).wait_reason == domain::WaitReason::admission);
    CHECK_FALSE(r.get(downloading).execution_id);
    CHECK(r.run_next().outcome == RunOutcome::completed);
    CHECK(r.run_next().outcome == RunOutcome::completed);
}
