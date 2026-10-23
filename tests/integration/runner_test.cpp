// Download execution on real SQLite, the real admission ledger and upstream client, the
// baseline audio processor and the Finalizer, with a scripted HTTP transport (T17): the
// complete run, unknown lengths (A25), admission pauses and attempts (A43, A44), late
// writes of old executions (A22), budgets that never restart (A26), cancel, shutdown
// checkpoints and resume (A27), fair dispatch and the pause policy.

#include "../unit/providers/mock_providers.hpp"

#include "application/accounts/credential_service.hpp"
#include "application/tasks/download_service.hpp"
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
#include <mpegfile.h>
#include <tpropertymap.h>
#include <tvariant.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
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
    std::function<domain::Result<ports::HttpResponse>(const ports::HttpRequest&, const ports::OperationContext&)> fetch;

    domain::Result<ports::HttpResponse> request(const ports::HttpRequest& request, const ports::UpstreamLease&,
                                                const ports::OperationContext& context) override {
        if (fetch) return fetch(request, context);
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
    std::function<domain::Result<domain::Track>(const domain::TrackRef&)> detail;
    std::function<domain::Result<domain::TrackLyrics>(const ports::ProviderContext&)> lyrics;
};

class MetadataScript final : public ports::ITrackDetail, public ports::ILyrics {
public:
    explicit MetadataScript(std::shared_ptr<ResolverScript> s) : script(std::move(s)) {}
    domain::Result<domain::Track> track_detail(const ports::ProviderContext&, const domain::TrackRef& ref) override {
        if (script->detail) return script->detail(ref);
        return domain::Track{ref, "detail", {}, {}, 1000, {}};
    }
    domain::Result<domain::TrackLyrics> lyrics(const ports::ProviderContext& c, const domain::TrackRef&) override {
        if (script->lyrics) return script->lyrics(c);
        return domain::TrackLyrics{{}, {}, {}, true, {}};
    }
    std::shared_ptr<ResolverScript> script;
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
    bundle.bindings[1] = ports::CapabilityBinding::bind<ports::ITrackDetail>(
        [script](const ports::ProviderSetup&) -> domain::Result<std::shared_ptr<ports::ITrackDetail>> {
            return std::shared_ptr<ports::ITrackDetail>{std::make_shared<MetadataScript>(script)};
        });
    if (name == "src") {
        bundle.capabilities.push_back({.key = "lyrics"});
        bundle.bindings.push_back(ports::CapabilityBinding::bind<ports::ILyrics>(
            [script](const ports::ProviderSetup&) -> domain::Result<std::shared_ptr<ports::ILyrics>> {
                return std::shared_ptr<ports::ILyrics>{std::make_shared<MetadataScript>(script)};
            }));
    }
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
                  runtime::AdmissionLimits limits = {}, std::shared_ptr<ports::AudioProcessor> processor = {},
                  std::size_t processing_workers = 1)
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
        processing = std::make_shared<runtime::BoundedExecutor>("processing", processing_workers, 4);
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
        if (processor) {
            services.processors = [processor](const ports::ProcessingPlan&)
                -> domain::Result<std::shared_ptr<ports::AudioProcessor>> { return processor; };
        }
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
                                          requirement,
                                          domain::MetadataPolicy::best_effort,
                                          {"dst_main", "d1", "n1", "m1"}};
        draft.product.output = output;
        draft.metadata_policy = metadata_policy;
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
    std::optional<domain::OutputOptions> output;
    domain::MetadataPolicy metadata_policy{domain::MetadataPolicy::best_effort};
    domain::ResourceRequirement requirement{};
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

TEST_CASE("one accepted batch permission failure leaves completed files and other items intact", "[runner][collections-b]") {
    Runs r{"batch-permissions"};
    using namespace application::tasks;
    DownloadService service{r.tasks, r.registry, r.credentials,
        {{"dst_main", infrastructure::filesystem::path_to_utf8(r.root), {r.owner}, true}}, {},
        [&](const std::string&, const std::string& path, std::uint64_t size) {
            const auto file = r.root / infrastructure::filesystem::path_from_utf8(path);
            return fs::is_regular_file(file) && fs::file_size(file) == size;
        }, 100};
    r.script->answer = [](const domain::TrackRef& ref) -> domain::Result<ports::ResolvedAudio> {
        if (ref.id.value() == "2") return domain::Error{ErrorCode::credentials_required, "subscription required", false, {}};
        return ports::ResolvedAudio{ref, domain::Quality::standard, domain::Completeness::unknown,
            "mp3", 128000, "https://cdn.test/" + ref.id.value() + ".mp3", {}, {}, 30010, ports::ProcessingPlan{}};
    };
    const std::vector<DownloadRequest> items{{track("src", "1")}, {track("src", "2")}, {track("src", "3")}, {track("src", "1")}};
    const auto accepted = unwrap(service.create_batch(r.owner, items, "batch-permission-001", "four"));
    REQUIRE(accepted.created_count == 3);
    REQUIRE(accepted.reused_count == 1);
    for (int i = 0; i < 3; ++i) (void)r.run_next();
    const auto view = unwrap(service.get_batch(r.owner, accepted.batch_id));
    CHECK(view.items[0].task.status == TaskStatus::completed);
    CHECK(view.items[1].task.status == TaskStatus::failed);
    REQUIRE(view.items[1].task.error_json);
    CHECK(json::parse(*view.items[1].task.error_json)["code"] == "CREDENTIALS_REQUIRED");
    CHECK(view.items[2].task.status == TaskStatus::completed);
    CHECK(view.items[3].task.task_id == view.items[0].task.task_id);
    for (const std::size_t index : {0U, 2U}) {
        const auto result = json::parse(*view.items[index].task.result_json);
        const auto asset = unwrap(r.publications->get_asset(r.owner, result["file_id"].get<std::string>()));
        CHECK(without_id3v2(read_bytes(r.root / infrastructure::filesystem::path_from_utf8(asset.relative_path))) == r.audio);
    }
    const auto reused = unwrap(service.create_batch(r.owner, {items[0], items[2]}, "batch-permission-002", "reuse"));
    CHECK(reused.created_count == 0);
    CHECK(reused.reused_count == 2);
}

TEST_CASE("new downloads fetch bounded metadata and publish readable tags in folders", "[runner][output]") {
    Runs r{"metadata-output"};
    r.output = domain::OutputOptions{};
    r.output->folder_layout = "artist_album";
    r.script->detail = [](const domain::TrackRef& ref) -> domain::Result<domain::Track> {
        return domain::Track{ref, "晴天", {{"歌手甲", {}}, {"歌手乙", {}}}, domain::AlbumSummary{"专辑", {}},
                             30010, {}, "https://img.test/cover.png", 3, 12};
    };
    const auto cover = read_bytes(fs::path{MUSIC_SERVICE_TEST_FIXTURES} / "audio/cover.png");
    int cover_calls = 0;
    int lyric_calls = 0;
    r.transport->fetch = [&](const ports::HttpRequest& req, const ports::OperationContext& ctx)
        -> domain::Result<ports::HttpResponse> {
        ++cover_calls;
        CHECK(req.url == "https://img.test/cover.png");
        CHECK(req.secret_headers.empty());
        CHECK(req.headers.empty());
        CHECK(req.max_response_bytes == 8 * 1024 * 1024);
        CHECK(ctx.deadline <= r.clock->steady_now() + 10s);
        return ports::HttpResponse{{200, {}, req.url}, cover};
    };
    r.script->lyrics = [&](const ports::ProviderContext& ctx) -> domain::Result<domain::TrackLyrics> {
        ++lyric_calls;
        CHECK(ctx.operation.deadline <= r.clock->steady_now() + 10s);
        return domain::TrackLyrics{"[00:01]original", "[00:02]翻译", {}, false, {}};
    };
    const auto id = r.create();
    // Changing the defaults after acceptance must not alter this task's stored snapshot.
    r.output->folder_layout = "flat";
    r.output->embed_cover = false;
    CHECK(r.run_next().outcome == RunOutcome::completed);
    const auto task = r.get(id);
    REQUIRE(task.result_json);
    const auto result = json::parse(*task.result_json);
    CHECK(result["metadata_fields"]["cover"] == "written");
    CHECK(result["metadata_fields"]["lyrics"] == "written");
    CHECK(result["metadata_fields"]["track_number"] == "written");
    const auto asset = unwrap(r.publications->get_asset(r.owner, result["file_id"].get<std::string>()));
    CHECK(asset.relative_path == "歌手甲, 歌手乙/专辑/歌手甲, 歌手乙 - 晴天.mp3");
    const auto path = r.root / infrastructure::filesystem::path_from_utf8(asset.relative_path);
    TagLib::MPEG::File tagged(path.c_str());
    REQUIRE(tagged.isValid());
    CHECK(tagged.properties()["TRACKNUMBER"] == TagLib::StringList("3/12"));
    CHECK(tagged.properties()["LYRICS"].front().to8Bit(true) == "[00:01]original\n\n[translation]\n[00:02]翻译");
    const auto pictures = tagged.complexProperties("PICTURE");
    REQUIRE(pictures.size() == 1);
    CHECK(pictures.front().value("data").toByteVector() == TagLib::ByteVector(cover.data(), static_cast<unsigned int>(cover.size())));
    CHECK(pictures.front().value("mimeType").toString() == "image/png");
    CHECK(without_id3v2(read_bytes(path)) == r.audio);
    CHECK(cover_calls == 1);
    CHECK(lyric_calls == 1);
}

TEST_CASE("sidecar fetch and tag requirements are independent", "[runner][sidecars]") {
    Runs r{"sidecar-policy"};
    r.output = domain::OutputOptions{};
    r.output->embed_cover = r.output->embed_lyrics = false;
    r.output->export_lyrics = true;
    r.metadata_policy = domain::MetadataPolicy::required;
    bool required_side = false;
    SECTION("optional lyric failure does not fail required basic tags") {}
    SECTION("required lyric failure fails even when tags are skipped") {
        required_side = true;
        r.output->sidecars_required = true;
        r.metadata_policy = domain::MetadataPolicy::skip;
    }
    r.script->lyrics = [](const ports::ProviderContext&) -> domain::Result<domain::TrackLyrics> {
        return domain::Error{ErrorCode::upstream_failed, "lyric unavailable now", false, {}};
    };
    const auto id = r.create();
    CHECK(r.run_next().outcome == (required_side ? RunOutcome::failed : RunOutcome::completed));
    const auto task = r.get(id);
    if (required_side) {
        CHECK_FALSE(task.result_json);
        CHECK(json::parse(*task.error_json)["code"] == "METADATA_FAILED");
    } else {
        const auto result = json::parse(*task.result_json);
        CHECK(result["metadata_status"] == "complete");
        CHECK(result["metadata_fields"]["lyrics_file"] == "unavailable");
    }
}

TEST_CASE("sidecar-only downloads keep audio bytes and reject corrupt bundles for reuse", "[runner][sidecars]") {
    using application::tasks::DownloadService;
    using application::tasks::DownloadRequest;
    Runs r{"sidecar-reuse"};
    domain::OutputOptions options;
    options.embed_cover = options.embed_lyrics = false;
    options.export_lyrics = options.sidecars_required = true;
    r.script->lyrics = [](const ports::ProviderContext&) -> domain::Result<domain::TrackLyrics> {
        return domain::TrackLyrics{"[00:00]test", {}, {}, false, {}};
    };
    DownloadService service{r.tasks, r.registry, r.credentials,
        {{"dst_main", infrastructure::filesystem::path_to_utf8(r.root), {r.owner}, true}}, {},
        [&](const std::string&, const std::string& path, std::uint64_t size) {
            const auto file = r.root / infrastructure::filesystem::path_from_utf8(path);
            return fs::is_regular_file(file) && fs::file_size(file) == size;
        }, 100, {}, options, r.files};
    DownloadRequest request{track("src", "1")};
    request.metadata_policy = domain::MetadataPolicy::skip;
    const auto first = unwrap(service.create(r.owner, request, "sidecar-create-0001", "same"));
    CHECK(r.run_next().outcome == RunOutcome::completed);
    const auto task = r.get(first.view.task.task_id);
    const auto file_id = json::parse(*task.result_json)["file_id"].get<std::string>();
    const auto asset = unwrap(r.publications->get_asset(r.owner, file_id));
    auto path = r.root / infrastructure::filesystem::path_from_utf8(asset.relative_path);
    CHECK(read_bytes(path) == r.audio);
    const auto intact = unwrap(service.create(r.owner, request, "sidecar-create-0002", "same"));
    CHECK(intact.reused);
    CHECK(intact.view.task.task_id == first.view.task.task_id);
    path.replace_extension(".lrc");
    { std::ofstream out{path, std::ios::binary | std::ios::trunc}; out << "[00:00]FAIL"; }
    const auto corrupt = unwrap(service.create(r.owner, request, "sidecar-create-0003", "same"));
    CHECK_FALSE(corrupt.reused);
    CHECK(corrupt.view.task.task_id != first.view.task.task_id);
}

TEST_CASE("metadata acquisition failures obey policy and never masquerade as absence", "[runner][output]") {
    Runs r{"metadata-errors"};
    r.output = domain::OutputOptions{};
    r.output->cover_max_bytes = 64;
    r.script->detail = [](const domain::TrackRef& ref) -> domain::Result<domain::Track> {
        return domain::Track{ref, "detail", {}, {}, 30010, {}, "https://img.test/a", {}, {}};
    };
    int calls = 0;
    int status = 404;
    std::string bytes = "not found";
    bool timeout = false;
    bool required = false;
    SECTION("404 best effort") {}
    SECTION("wrong image body") { status = 200; bytes = "<html>not an image</html>"; }
    SECTION("body over configured limit") { status = 200; bytes.assign(65, 'x'); }
    SECTION("child deadline exhausted") { timeout = true; }
    SECTION("required 404") { required = true; }
    SECTION("required timeout retries") { required = true; timeout = true; }
    if (required) r.metadata_policy = domain::MetadataPolicy::required;
    r.transport->fetch = [&](const ports::HttpRequest& req, const ports::OperationContext&) -> domain::Result<ports::HttpResponse> {
        ++calls;
        if (timeout) return domain::Error{ErrorCode::deadline_exceeded, "metadata budget expired", false, {}};
        return ports::HttpResponse{{status, {}, req.url}, bytes};
    };
    const auto id = r.create();
    const auto report = r.run_next();
    CHECK(calls == 1);
    const auto task = r.get(id);
    if (required) {
        CHECK(report.outcome == (timeout ? RunOutcome::waiting_retry : RunOutcome::failed));
        CHECK_FALSE(task.result_json);
        CHECK_FALSE(fs::exists(r.root / "detail.mp3"));
        CHECK(r.publications->pending()->empty());
    } else {
        CHECK(report.outcome == RunOutcome::completed);
        REQUIRE(task.result_json);
        const auto result = json::parse(*task.result_json);
        CHECK(result["metadata_status"] == "partial");
        CHECK(result["metadata_fields"]["cover"] == "fetch_failed");
        CHECK(result["metadata_fields"]["lyrics"] == "unavailable");
    }
}

TEST_CASE("metadata disabled and legacy tasks make no optional fetches", "[runner][output]") {
    Runs r{"metadata-disabled"};
    bool skip = false;
    bool legacy = false;
    SECTION("skip policy") { skip = true; }
    SECTION("independent switches") {}
    SECTION("old snapshot") { legacy = true; }
    if (!legacy) {
        r.output = domain::OutputOptions{};
        if (!skip) { r.output->embed_cover = false; r.output->embed_lyrics = false; }
    }
    if (skip) r.metadata_policy = domain::MetadataPolicy::skip;
    r.script->detail = [](const domain::TrackRef& ref) -> domain::Result<domain::Track> {
        return domain::Track{ref, "detail", {}, {}, 30010, {}, "https://img.test/a", {}, {}};
    };
    r.transport->fetch = [](const ports::HttpRequest&, const ports::OperationContext&) -> domain::Result<ports::HttpResponse> {
        CHECK(false);
        return domain::Error{ErrorCode::internal, "unexpected fetch", false, {}};
    };
    r.script->lyrics = [](const ports::ProviderContext&) -> domain::Result<domain::TrackLyrics> {
        CHECK(false);
        return domain::TrackLyrics{};
    };
    const auto id = r.create();
    CHECK(r.run_next().outcome == RunOutcome::completed);
    const auto result = json::parse(*r.get(id).result_json);
    if (!legacy) {
        CHECK(result["metadata_fields"]["cover"] == "disabled");
        CHECK(result["metadata_fields"]["lyrics"] == "disabled");
    }
}

TEST_CASE("required metadata permits confirmed absent cover and instrumental lyrics", "[runner][output]") {
    Runs r{"metadata-absent"};
    r.output = domain::OutputOptions{};
    r.metadata_policy = domain::MetadataPolicy::required;
    bool unsupported = false;
    SECTION("instrumental") {}
    SECTION("no lyric capability") { unsupported = true; }
    const auto id = r.create(unsupported ? "busy" : "src");
    CHECK(r.run_next().outcome == RunOutcome::completed);
    const auto result = json::parse(*r.get(id).result_json);
    CHECK(result["metadata_fields"]["cover"] == "unavailable");
    CHECK(result["metadata_fields"]["lyrics"] == (unsupported ? "unsupported" : "unavailable"));
    CHECK(result["metadata_status"] == "complete");
}

TEST_CASE("hostile and long metadata publishes stable paths without escaping or overwriting", "[runner][output]") {
    Runs r{"metadata-paths"};
    r.output = domain::OutputOptions{};
    r.output->folder_layout = "artist_album";
    r.output->name_format = "title";
    r.output->embed_cover = false;
    r.output->embed_lyrics = false;
    std::string label = "CON";
    SECTION("reserved and traversal components") {}
    SECTION("long UTF-8 components") { for (int i = 0; i < 100; ++i) label += "中文"; }
    r.script->detail = [&](const domain::TrackRef& ref) -> domain::Result<domain::Track> {
        return domain::Track{ref, label, {{"../../" + label, {}}}, domain::AlbumSummary{label + "<>|", {}}, 30010, {}};
    };
    std::vector<std::string> paths;
    for (const auto* track_id : {"1", "2"}) {
        const auto id = r.create("src", track_id);
        CHECK(r.run_next().outcome == RunOutcome::completed);
        const auto result = json::parse(*r.get(id).result_json);
        const auto asset = unwrap(r.publications->get_asset(r.owner, result["file_id"].get<std::string>()));
        paths.push_back(asset.relative_path);
        const auto relative = infrastructure::filesystem::path_from_utf8(asset.relative_path);
        CHECK_FALSE(relative.is_absolute());
        for (const auto& part : relative) CHECK(part != "..");
        CHECK(without_id3v2(read_bytes(r.root / relative)) == r.audio);
    }
    CHECK(paths[0] != paths[1]);
    CHECK(paths[1].ends_with(" (2).mp3"));
}

TEST_CASE("cancel during cover acquisition releases admission and never publishes", "[runner][output]") {
    Runs r{"metadata-cancel"};
    r.output = domain::OutputOptions{};
    r.script->detail = [](const domain::TrackRef& ref) -> domain::Result<domain::Track> {
        return domain::Track{ref, "detail", {}, {}, 30010, {}, "https://img.test/a", {}, {}};
    };
    std::atomic<bool> entered{false};
    r.transport->fetch = [&](const ports::HttpRequest&, const ports::OperationContext& c) -> domain::Result<ports::HttpResponse> {
        entered = true;
        while (!c.stop.stop_requested() && std::chrono::steady_clock::now() < c.deadline) std::this_thread::sleep_for(5ms);
        return domain::Error{ErrorCode::canceled, "canceled", false, {}};
    };
    auto executor = std::make_shared<runtime::BoundedExecutor>("downloads", 1, 1);
    downloads::DownloadScheduler scheduler{{1, 20ms}, r.tasks, r.ledger, r.runner, executor, r.clock};
    const auto id = r.create();
    scheduler.start();
    REQUIRE(eventually([&] { return entered.load(); }));
    CHECK(unwrap(r.tasks->request_cancel(r.owner, id)).status == TaskStatus::canceling);
    scheduler.request_stop(id);
    CHECK(eventually([&] { return r.get(id).status == TaskStatus::canceled; }));
    CHECK_FALSE(fs::exists(r.root / "detail.mp3"));
    CHECK(r.ledger->usage().total == 0);
    scheduler.stop_claiming();
    executor->stop_accepting();
    executor->request_stop();
    executor->join();
    scheduler.join();
}

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
    CHECK(without_id3v2(read_bytes(r.root / "detail.mp3")) == r.audio);
    CHECK_FALSE(fs::exists(r.root / ".music-service-work" / infrastructure::filesystem::path_from_utf8(id)));
    const auto asset = unwrap(r.publications->get_asset(r.owner, result["file_id"].get<std::string>()));
    CHECK(asset.relative_path == "detail.mp3");
    CHECK(json::parse(asset.properties_json)["actual_track"]["id"] == "1");

    // The same name again gets the next candidate, never the existing file.
    r.create("src", "2");
    CHECK(r.run_next().outcome == RunOutcome::completed);
    CHECK(without_id3v2(read_bytes(r.root / "detail (2).mp3")) == r.audio);
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
    CHECK(without_id3v2(read_bytes(r.root / "detail.mp3")) == r.audio);
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
    CHECK(without_id3v2(read_bytes(r.root / "detail.mp3")) == r.audio); // never appended to the old prefix
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
        CHECK(without_id3v2(read_bytes(r.root / "detail.mp3")) == r.audio);
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
        CHECK(without_id3v2(read_bytes(r.root / "detail.mp3")) == other); // only bytes of one resource
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
    CHECK(without_id3v2(read_bytes(r.root / "detail.mp3")) == r.audio); // never published as 64000 bytes
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
    CHECK(without_id3v2(read_bytes(r.root / "detail.mp3")) == r.audio);
    CHECK(r.get(id).attempt == 2);
}

TEST_CASE("an expired address changing quality discards the partial file", "[downloads][runner][stage-c]") {
    Runs r{"quality-change"};
    int resolved = 0;
    r.script->answer = [&](const domain::TrackRef& t) -> domain::Result<ports::ResolvedAudio> {
        ++resolved;
        return ports::ResolvedAudio{t, resolved == 1 ? domain::Quality::high : domain::Quality::standard,
            domain::Completeness::unknown, "mp3", 128000,
            "https://cdn.test/quality-" + std::to_string(resolved) + ".mp3", {}, {}, 30010, ports::ProcessingPlan{}};
    };
    int first_url = 0;
    r.transport->answer = [&](const ports::HttpRequest& request) {
        if (request.url == "https://cdn.test/quality-1.mp3") {
            if (++first_url == 1) {
                auto reply = serve_range(request, r.audio);
                reply.reset_after = 90000;
                return reply;
            }
            return Reply{403, {{"Content-Type", "text/html"}}, "expired"};
        }
        return serve_range(request, r.audio);
    };
    const auto id = r.create();
    CHECK(r.run_next().outcome == RunOutcome::waiting_retry);
    const auto task = r.get(id);
    REQUIRE(task.next_retry_at);
    CHECK(r.run_next(*task.next_retry_at).outcome == RunOutcome::completed);
    CHECK(resolved == 2);
    const auto requests = r.transport->requests();
    const auto& last = requests.back();
    CHECK(last.url == "https://cdn.test/quality-2.mp3");
    CHECK_FALSE(header(last, "Range"));
    CHECK_FALSE(header(last, "If-Range"));
    CHECK(without_id3v2(read_bytes(r.root / "detail.mp3")) == r.audio);
    CHECK(json::parse(r.get(id).execution_json.value())["actual_quality"] == "standard");
}

TEST_CASE("byte evidence downgrade obeys strict and allow_lower before publication", "[downloads][runner][stage-e]") {
    for (const auto policy : {domain::QualityPolicy::strict, domain::QualityPolicy::allow_lower}) {
        Runs r{policy == domain::QualityPolicy::strict ? "bytes-strict" : "bytes-lower"};
        r.script->answer = [](const domain::TrackRef& t) -> domain::Result<ports::ResolvedAudio> {
            return ports::ResolvedAudio{t, domain::Quality::high, domain::Completeness::full, "mp3", 320000,
                "https://cdn.test/wrong-bitrate.mp3", {}, {}, 30010, ports::ProcessingPlan{}};
        }; // 实际传输仍是测试服务器生成的 128 kbps MP3。
        domain::DownloadIntentDraft draft{r.owner, track("src","1"), domain::SameSource{},
            {anonymous(r.owner,"src")}, {domain::Quality::high, policy, false},
            domain::MetadataPolicy::best_effort, {"dst_main","d1","n1","m1"}};
        const auto created = unwrap(r.tasks->create({unwrap(domain::NormalizedDownloadIntent::normalize(std::move(draft))),
            "byte-quality-test", "h", false}));
        const auto result = r.run_next();
        const auto task = r.get(created.task.task_id);
        if (policy == domain::QualityPolicy::strict) {
            CHECK(result.outcome == RunOutcome::failed);
            CHECK(json::parse(*task.error_json)["code"] == "QUALITY_UNAVAILABLE");
            CHECK_FALSE(fs::exists(r.root / "detail.mp3"));
        } else {
            CHECK(result.outcome == RunOutcome::completed);
            const auto execution = json::parse(*task.execution_json);
            CHECK(execution["actual_quality"] == "standard");
            CHECK(std::find(execution["warnings"].begin(), execution["warnings"].end(), "QUALITY_LOWERED") != execution["warnings"].end());
            CHECK(without_id3v2(read_bytes(r.root / "detail.mp3")) == r.audio);
        }
    }
}

TEST_CASE("native Kuwo FLAC publication uses decoded quality and rejects corrupted audio", "[downloads][runner][kuwo-vinyl]") {
    for (const auto mode : {0, 1, 2, 3}) {
        CAPTURE(mode);
        Runs r{"vinyl-publish-" + std::to_string(mode)};
        const auto original = read_bytes(fs::path{MUSIC_SERVICE_TEST_FIXTURES} / "audio" /
            (mode == 1 || mode == 2 ? "verify-16-44-fixed.flac" : "verify-24-96-lpc.flac"));
        r.audio = original;
        if (mode == 3) r.audio[r.audio.size() - 8] ^= 2;
        r.audio += std::string{"\xf0\x00\xff\x0f", 4} + std::string(22, 'k') + std::string{"\x0e\x55\xff\xf0", 4};
        r.requirement = {domain::Quality::hires, mode == 2 ? domain::QualityPolicy::allow_lower : domain::QualityPolicy::strict};
        r.metadata_policy = domain::MetadataPolicy::skip;
        r.script->answer = [](const domain::TrackRef& t) -> domain::Result<ports::ResolvedAudio> {
            return ports::ResolvedAudio{t, domain::Quality::hires, domain::Completeness::full, "flac", {},
                "https://cdn.test/vinyl.flac", {}, {}, {}, {"audio.kuwo-flac", 1, {}}};
        };
        const auto id = r.create();
        const auto run = r.run_next();
        const auto task = r.get(id);
        if (mode == 1 || mode == 3) {
            CHECK(run.outcome == RunOutcome::failed);
            REQUIRE(task.error_json);
            CHECK(json::parse(*task.error_json)["code"] == (mode == 1 ? "QUALITY_UNAVAILABLE" : "AUDIO_INVALID"));
            CHECK_FALSE(fs::exists(r.root / "detail.flac"));
            CHECK_FALSE(task.result_json);
        } else {
            CHECK(run.outcome == RunOutcome::completed);
            REQUIRE(task.result_json);
            const auto result = json::parse(*task.result_json);
            CHECK(result["processing"]["processor"] == "audio.kuwo-flac");
            CHECK(std::find(result["warnings"].begin(), result["warnings"].end(), "KUWO_TRAILER_REMOVED") != result["warnings"].end());
            const auto execution = json::parse(*task.execution_json);
            CHECK(execution["actual_quality"] == (mode == 2 ? "lossless" : "hires"));
            if (mode == 2) CHECK(std::find(execution["warnings"].begin(), execution["warnings"].end(), "QUALITY_LOWERED") != execution["warnings"].end());
            CHECK(read_bytes(r.root / "detail.flac") == original);
        }
    }
}

TEST_CASE("ordinary downloads automatically decrypt NCM and reject corrupt or lower-quality payloads", "[downloads][runner][ncm]") {
    for (const auto mode : {0, 1, 2, 3}) {
        CAPTURE(mode);
        Runs r{"auto-ncm-" + std::to_string(mode)};
        const auto fixtures = fs::path{MUSIC_SERVICE_TEST_FIXTURES} / "audio";
        r.audio = read_bytes(fixtures / (mode == 1 ? "synthetic-flac.ncm" : "synthetic-mp3.ncm"));
        if (mode == 2) r.audio[14] ^= 0x51;
        r.metadata_policy = domain::MetadataPolicy::skip;
        if (mode == 3) r.requirement = {domain::Quality::hires, domain::QualityPolicy::strict};
        r.script->answer = [mode](const domain::TrackRef& t) -> domain::Result<ports::ResolvedAudio> {
            return ports::ResolvedAudio{t, mode == 3 ? domain::Quality::hires : domain::Quality::standard,
                domain::Completeness::full, "mp3", {}, "https://cdn.test/audio.mp3", {}, {}, {}, {}};
        };
        const auto id = r.create();
        const auto run = r.run_next();
        const auto task = r.get(id);
        if (mode >= 2) {
            CHECK(run.outcome == RunOutcome::failed);
            REQUIRE(task.error_json);
            CHECK(json::parse(*task.error_json)["code"] == (mode == 2 ? "AUDIO_INVALID" : "QUALITY_UNAVAILABLE"));
            CHECK_FALSE(task.result_json);
            CHECK_FALSE(fs::exists(r.root / "detail.mp3"));
        } else {
            CHECK(run.outcome == RunOutcome::completed);
            REQUIRE(task.result_json);
            CHECK(json::parse(*task.result_json)["processing"]["processor"] == "audio.ncm");
            CHECK(read_bytes(r.root / (mode == 1 ? "detail.flac" : "detail.mp3")) ==
                read_bytes(fixtures / (mode == 1 ? "sine-3s.flac" : "ncm-plain.mp3")));
        }
    }
}

TEST_CASE("NCM processing runs off the caller while queries other downloads and cancel remain usable", "[downloads][scheduler][ncm]") {
    class HeldNativeProcessor final : public ports::AudioProcessor {
    public:
        ports::ProcessingPlan plan() const override { return {}; }
        domain::Result<ports::ProcessingReport> process(const ports::ProcessingRequest& request,
                                                       const ports::OperationContext& context) override {
            if (calls.fetch_add(1) == 0) {
                worker = std::this_thread::get_id();
                entered = true;
                // A deterministic long operation at the native-processing boundary.
                // Cancellation still flows into the actual NCM processor below.
                while (!release && !context.stop.stop_requested() && !context.expired(std::chrono::steady_clock::now()))
                    std::this_thread::sleep_for(5ms);
            }
            return audio_processing::VerifyingProcessor{}.process(request, context);
        }
        std::atomic<int> calls{0};
        std::atomic<bool> entered{false}, release{false};
        std::thread::id worker;
    };
    for (const bool cancel : {false, true}) {
        CAPTURE(cancel);
        auto processor = std::make_shared<HeldNativeProcessor>();
        Runs r{"ncm-background", quick_options(), {}, processor, 2};
        r.audio = read_bytes(fs::path{MUSIC_SERVICE_TEST_FIXTURES} / "audio" / "synthetic-mp3.ncm");
        r.script->answer = [](const domain::TrackRef& t) -> domain::Result<ports::ResolvedAudio> {
            return ports::ResolvedAudio{t, domain::Quality::standard, domain::Completeness::full,
                "mp3", {}, "https://cdn.test/audio.mp3", {}, {}, {}, {}};
        };
        auto executor = std::make_shared<runtime::BoundedExecutor>("downloads", 2, 2);
        downloads::DownloadScheduler scheduler{{2, 10ms}, r.tasks, r.ledger, r.runner, executor, r.clock};
        struct StopWorkers {
            downloads::DownloadScheduler& scheduler;
            runtime::BoundedExecutor& executor;
            HeldNativeProcessor& processor;
            ~StopWorkers() {
                processor.release = true;
                scheduler.stop_claiming();
                executor.stop_accepting();
                executor.request_stop();
                executor.join();
                scheduler.join();
            }
        } cleanup{scheduler, *executor, *processor};
        const auto first = r.create("src", "first");
        scheduler.start();
        REQUIRE(eventually([&] { return processor->entered.load(); }));
        CHECK(processor->worker != std::this_thread::get_id());
        CHECK(r.get(first).status == TaskStatus::processing);
        // A second task must finish before the first processing worker is released.
        const auto second = r.create("src", "second");
        REQUIRE(eventually([&] { return r.get(second).status == TaskStatus::completed; }));
        CHECK(r.get(first).status == TaskStatus::processing);
        if (cancel) {
            CHECK(unwrap(r.tasks->request_cancel(r.owner, first)).status == TaskStatus::canceling);
            scheduler.request_stop(first);
        } else processor->release = true;
        CHECK(eventually([&] { return r.get(first).status == (cancel ? TaskStatus::canceled : TaskStatus::completed); }));
        if (cancel) {
            CHECK_FALSE(r.get(first).result_json);
            CHECK_FALSE(fs::exists(r.root / ".music-service-work" / infrastructure::filesystem::path_from_utf8(first)));
        }
    }
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

TEST_CASE("processing and observer exceptions release slots and save a terminal error", "[stability][downloads]") {
    class ThrowOnce final : public ports::AudioProcessor {
    public:
        ports::ProcessingPlan plan() const override { return {}; }
        std::atomic<int> calls{0};
        domain::Result<ports::ProcessingReport> process(const ports::ProcessingRequest& request,
                                                        const ports::OperationContext& context) override {
            if (++calls == 1) throw std::runtime_error("injected processor failure");
            return audio_processing::VerifyingProcessor{}.process(request, context);
        }
    };
    auto processor = std::make_shared<ThrowOnce>();
    Runs r{"throwing-processing", quick_options(), {}, processor};
    const auto first = r.create("src", "one"), second = r.create("src", "two");
    auto executor = std::make_shared<runtime::BoundedExecutor>("downloads", 1, 4);
    downloads::DownloadScheduler scheduler{{1, 10ms}, r.tasks, r.ledger, r.runner, executor, r.clock,
        [](const auto&, const auto&) { throw std::runtime_error("observer failed"); }};
    scheduler.start();
    const bool done = eventually([&] {
        return r.get(first).status == TaskStatus::failed && r.get(second).status == TaskStatus::completed &&
               scheduler.stats().running == 0;
    });
    scheduler.stop_claiming();
    scheduler.stop_running();
    executor->request_stop();
    executor->join();
    scheduler.join();
    REQUIRE(done);
    CHECK(json::parse(*r.get(first).error_json)["code"] == "INTERNAL_ERROR");
    CHECK(scheduler.stats().errors == 2);
    CHECK(scheduler.stats().running == 0);
}

TEST_CASE("queued processing can be canceled expired or dropped without leaving a waiter", "[stability][downloads]") {
    auto options = quick_options();
    options.attempt_budget = 1s;
    Runs r{"queued-processing", options, {}, {}, 0};
    const auto id = r.create();
    auto claimed = unwrap(r.tasks->claim(id, r.clock->now()));
    REQUIRE(claimed);
    downloads::ExecutionControl control;
    auto result = std::async(std::launch::async, [&] { return r.runner->run(*claimed, control); });
    const bool queued = eventually([&] { return r.processing->stats().queued == 1; });
    CHECK(queued);
    auto expected = RunOutcome::waiting_admission;
    SECTION("dropped queue publishes broken promise") { r.processing->request_stop(); }
    SECTION("cancel does not wait for a processing worker") {
        REQUIRE(r.tasks->request_cancel(r.owner, id));
        control.request(downloads::StopReason::canceled);
        expected = RunOutcome::canceled;
    }
    SECTION("deadline stops waiting for an unstarted operation") { expected = RunOutcome::waiting_retry; }
    const bool ready = result.wait_for(3s) == std::future_status::ready;
    r.processing->request_stop();
    REQUIRE(ready);
    CHECK(result.get().outcome == expected);
    CHECK_FALSE(domain::is_running(r.get(id).status));
}

TEST_CASE("a provider exception becomes a persisted failure", "[stability][downloads]") {
    Runs r{"throwing-resolve"};
    r.script->answer = [](const auto&) -> domain::Result<ports::ResolvedAudio> { throw std::runtime_error("secret"); };
    const auto id = r.create();
    CHECK(r.run_next().outcome == RunOutcome::failed);
    CHECK(r.get(id).status == TaskStatus::failed);
    CHECK(r.get(id).error_json->find("secret") == std::string::npos);
}

TEST_CASE("the dispatcher survives a readiness exception and resumes claiming", "[stability][scheduler]") {
    class ThrowOnceAdmission final : public ports::UpstreamAdmissionPort {
    public:
        explicit ThrowOnceAdmission(std::shared_ptr<ports::UpstreamAdmissionPort> next) : next_(std::move(next)) {}
        domain::Result<ports::AdmissionOutcome> try_acquire(const ports::AdmissionRequest& request) override {
            return next_->try_acquire(request);
        }
        std::optional<ports::Deferred> readiness(const ports::AdmissionRequest& request) const override {
            if (!thrown_.exchange(true)) throw std::runtime_error("injected readiness failure");
            return next_->readiness(request);
        }
        void feedback(const ports::AdmissionRequest& request,
                      std::optional<std::chrono::steady_clock::time_point> retry) override { next_->feedback(request, retry); }
    private:
        std::shared_ptr<ports::UpstreamAdmissionPort> next_;
        mutable std::atomic<bool> thrown_{false};
    };
    Runs r{"dispatch-exception"};
    const auto id = r.create();
    auto executor = std::make_shared<runtime::BoundedExecutor>("downloads", 1, 4);
    auto admission = std::make_shared<ThrowOnceAdmission>(r.ledger);
    downloads::DownloadScheduler scheduler{{1, 10ms}, r.tasks, admission, r.runner, executor, r.clock};
    scheduler.start();
    const bool done = eventually([&] { return r.get(id).status == TaskStatus::completed; });
    scheduler.stop_claiming();
    scheduler.stop_running();
    executor->request_stop();
    executor->join();
    scheduler.join();
    CHECK(done);
    CHECK(scheduler.stats().errors == 1);
    CHECK(scheduler.stats().running == 0);
}

TEST_CASE("dropping queued downloads returns their claim and scheduler slot", "[stability][scheduler]") {
    Runs r{"dropped-download"};
    const auto id = r.create();
    auto executor = std::make_shared<runtime::BoundedExecutor>("downloads", 0, 4);
    downloads::DownloadScheduler scheduler{{1, 10ms}, r.tasks, r.ledger, r.runner, executor, r.clock};
    scheduler.start();
    CHECK(eventually([&] { return executor->stats().queued == 1; }));
    scheduler.stop_claiming();
    scheduler.join();
    executor->request_stop();
    CHECK(scheduler.wait_idle(1s));
    CHECK(scheduler.stats().running == 0);
    CHECK(r.get(id).status == TaskStatus::waiting_retry);
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
    CHECK(without_id3v2(read_bytes(r.root / "detail.mp3")) == r.audio);
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
