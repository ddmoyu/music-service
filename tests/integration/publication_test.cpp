// Publication of verified files and startup recovery on real SQLite and a real
// destination directory (A12, A17, A19 basics, A20): reservations, renames that never
// replace, the crash matrix of internal-contracts.md 7, and a service restart.

#include "../unit/domain/test_support.hpp"
#include "service_harness.hpp"

#include "downloads/finalizer.hpp"
#include "downloads/sidecars.hpp"
#include "application/media/file_access.hpp"
#include "downloads/recovery.hpp"
#include "infrastructure/clock.hpp"
#include "infrastructure/filesystem/destination_files.hpp"
#include "infrastructure/filesystem/paths.hpp"
#include "infrastructure/observability/correlation.hpp"
#include "infrastructure/storage/sqlite/account_repositories.hpp"
#include "infrastructure/storage/sqlite/schema.hpp"
#include "infrastructure/storage/sqlite/task_repositories.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <functional>
#include <array>
#include <barrier>
#include <thread>

using namespace music_service;
namespace fs = std::filesystem;
namespace sqlite = infrastructure::storage::sqlite;
namespace ports = application::ports;
using domain::ErrorCode;
using domain::TaskStatus;
using downloads::RecoveryAction;
using infrastructure::filesystem::path_from_utf8;

namespace {

void write_bytes(const fs::path& path, std::string_view bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << bytes;
}

std::string read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

// Passes through to the real files, with a hook before each move.
class HookedFiles final : public ports::DestinationFiles {
public:
    explicit HookedFiles(std::shared_ptr<ports::DestinationFiles> inner) : inner_(std::move(inner)) {}

    domain::Result<bool> exists(const std::string& d, const std::string& p) override {
        if (inspect_error) return *inspect_error;
        return inner_->exists(d, p);
    }
    domain::Result<std::optional<ports::FileFingerprint>> fingerprint(const std::string& d,
                                                                      const std::string& p) override {
        return inner_->fingerprint(d, p);
    }
    domain::Result<void> move(const std::string& d, const std::string& from, const std::string& to) override {
        if (before_move) {
            before_move();
        }
        auto moved = inner_->move(d, from, to);
        if (moved && flush_fails) {
            // Renamed, then the directory flush failed, as rename_no_replace reports it.
            return domain::Error{ErrorCode::file_system, "flush directory failed", false, {}};
        }
        return moved;
    }
    domain::Result<void> remove_workspace(const std::string& d, const std::string& t) override {
        return inner_->remove_workspace(d, t);
    }

    std::function<void()> before_move;
    std::optional<domain::Error> inspect_error;
    bool flush_fails{false};

private:
    std::shared_ptr<ports::DestinationFiles> inner_;
};

struct Publishing {
    explicit Publishing(std::string_view name)
        : dir(fs::temp_directory_path() / path_from_utf8("music-service-发布-" + std::string{name})) {
        fs::remove_all(dir);
        fs::create_directories(dir / "音乐");
        root = test::unwrap(infrastructure::filesystem::canonical_directory(dir / "音乐"));
        open();
        REQUIRE(destinations->bind("dst_main", infrastructure::filesystem::path_to_utf8(root)));
        owner = test::unwrap(identities->create_principal("owner", ports::PrincipalRole::user, std::nullopt)).id;
    }
    ~Publishing() {
        close();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    // A restart: every repository and the database are closed and opened again.
    void open() {
        database = sqlite::Database::open(dir / "service.sqlite3", {}).value();
        REQUIRE(sqlite::initialize_schema(*database, clock->now()));
        const sqlite::RepositoryContext ctx{database.get(), clock, ids};
        identities = std::make_shared<sqlite::SqliteIdentityRepository>(ctx);
        destinations = std::make_shared<sqlite::SqliteDestinationRepository>(ctx);
        tasks = std::make_shared<sqlite::SqliteTaskRepository>(ctx, sqlite::TaskRepositoryOptions{});
        publications = std::make_shared<sqlite::SqlitePublicationRepository>(ctx);
        real_files = std::make_shared<infrastructure::filesystem::LocalDestinationFiles>(
            std::map<std::string, fs::path>{{"dst_main", root}});
        files = std::make_shared<HookedFiles>(real_files);
        finalizer = std::make_unique<downloads::Finalizer>(publications, files);
    }
    void close() {
        finalizer.reset();
        files.reset();
        real_files.reset();
        publications.reset();
        tasks.reset();
        destinations.reset();
        identities.reset();
        database.reset();
    }
    void restart() {
        close();
        open();
    }

    downloads::RecoveryReport recover(std::uint32_t max_attempts = 3) {
        return test::unwrap(downloads::Recovery(publications, tasks, files, {max_attempts}).run());
    }

    ports::TaskRecord step(const ports::TaskRecord& task, TaskStatus to) {
        return test::unwrap(tasks->transition({task.task_id, *task.execution_id, task.version, to, {}, {}, {}}));
    }

    // A claimed task that reached processing.
    ports::TaskRecord processing(std::string_view track_id, std::string_view request_suffix = {}) {
        domain::DownloadIntentDraft draft{owner,
                                          test::track("kuwo", track_id),
                                          {test::anonymous(owner, "kuwo")},
                                          {domain::Quality::standard, domain::QualityPolicy::strict, false},
                                          domain::MetadataPolicy::best_effort,
                                          {"dst_main", "d1", "n1", "m1"}};
        auto intent = test::unwrap(domain::NormalizedDownloadIntent::normalize(std::move(draft)));
        REQUIRE(tasks->create({std::move(intent), "key-" + std::string{track_id} + std::string{request_suffix}, "h", false}));
        auto task = *test::unwrap(tasks->claim_next(clock->now()));
        task = step(task, TaskStatus::downloading);
        return step(task, TaskStatus::processing);
    }

    std::string work_file(const ports::TaskRecord& task) const {
        return ".music-service-work/" + task.task_id + "/processed.part";
    }
    fs::path at(std::string_view relative) const { return root / path_from_utf8(relative); }

    // Writes the processed file and returns the report a successful validation gives.
    ports::ProcessingReport processed(const ports::TaskRecord& task, std::string_view bytes) {
        write_bytes(at(work_file(task)), bytes);
        ports::ProcessingReport report;
        report.processor_id = "audio.verify";
        report.processor_version = 1;
        report.facts.container = "mp3";
        report.facts.size_bytes = static_cast<std::int64_t>(bytes.size());
        report.facts.sha256 = test::unwrap(real_files->fingerprint("dst_main", work_file(task)))->sha256;
        report.completeness = domain::Completeness::full;
        report.stages = {{"decrypt", ports::StageOutcome::skipped},
                         {"validate", ports::StageOutcome::done},
                         {"tags", ports::StageOutcome::skipped}};
        return report;
    }

    domain::Result<ports::CompletedPublication> publish(const ports::TaskRecord& task, const std::string& target,
                                                        const ports::ProcessingReport& report) {
        return finalizer->publish({task, work_file(task), target, report, R"({"quality":"standard"})"});
    }

    // The state a crash leaves right after the intent commit: processing -> finalizing
    // with the evidence of the work file, nothing renamed yet.
    ports::TaskRecord crash_after_intent(std::string_view track_id, std::string_view bytes, const std::string& target) {
        auto task = processing(track_id);
        write_bytes(at(work_file(task)), bytes);
        const auto file = *test::unwrap(real_files->fingerprint("dst_main", work_file(task)));
        const downloads::PublicationEvidence evidence{file, target, "mp3", "{}", R"({"metadata_status":"skipped"})"};
        return test::unwrap(publications->begin({task.task_id, *task.execution_id, "dst_main", work_file(task), target,
                                                 downloads::encode_evidence(evidence), "reserved"},
                                                task.version));
    }

    ports::TaskRecord get(const std::string& task_id) { return test::unwrap(tasks->get(owner, task_id)); }

    fs::path dir;
    fs::path root;
    std::shared_ptr<infrastructure::ManualClock> clock = std::make_shared<infrastructure::ManualClock>();
    std::shared_ptr<infrastructure::observability::IdGenerator> ids =
        std::make_shared<infrastructure::observability::IdGenerator>(16);
    std::unique_ptr<sqlite::Database> database;
    std::shared_ptr<sqlite::SqliteIdentityRepository> identities;
    std::shared_ptr<sqlite::SqliteDestinationRepository> destinations;
    std::shared_ptr<sqlite::SqliteTaskRepository> tasks;
    std::shared_ptr<sqlite::SqlitePublicationRepository> publications;
    std::shared_ptr<infrastructure::filesystem::LocalDestinationFiles> real_files;
    std::shared_ptr<HookedFiles> files;
    std::unique_ptr<downloads::Finalizer> finalizer;
    std::string owner;
};

} // namespace

namespace {
downloads::FinalizeRequest bundle_request(Publishing& p, const ports::TaskRecord& task) {
    auto report = p.processed(task, "audio bytes");
    report.output = p.at(p.work_file(task));
    downloads::FinalizeRequest request{task, p.work_file(task), "Song.mp3", report, "{}"};
    ports::TrackMetadata metadata;
    metadata.lyrics = "[00:00.00]test\n";
    metadata.cover = ports::Picture{"image/png", read_bytes(fs::path{MUSIC_SERVICE_TEST_FIXTURES} / "audio/cover.png")};
    domain::OutputOptions options;
    options.export_lyrics = options.export_cover = options.sidecars_required = true;
    REQUIRE(downloads::prepare_sidecars(request, *p.real_files, metadata, options,
        {std::chrono::steady_clock::now() + std::chrono::seconds{10}, {}}));
    return request;
}
}

TEST_CASE("bundle publication survives every durable boundary and never exposes a partial asset", "[publication][sidecars]") {
    for (int boundary = 0; boundary < 4; ++boundary) {
        CAPTURE(boundary);
        Publishing p{"bundle-boundary"};
        const auto task = p.processing("1");
        auto request = bundle_request(p, task);
        if (boundary == 0) p.files->before_move = [] { throw std::runtime_error{"simulated exit after intent"}; };
        if (boundary == 1) p.files->flush_fails = true;
        if (boundary == 2) REQUIRE(p.database->write([](sqlite::Connection& db) {
            return db.exec("CREATE TRIGGER reject_asset BEFORE INSERT ON file_assets BEGIN SELECT RAISE(ABORT, 'injected commit failure'); END");
        }));
        if (boundary == 0) REQUIRE_THROWS(p.finalizer->publish(request));
        else {
            auto done = p.finalizer->publish(request);
            CHECK(static_cast<bool>(done) == (boundary == 3));
        }
        if (boundary != 3) {
            CHECK(p.get(task.task_id).status == TaskStatus::finalizing);
            CHECK_FALSE(p.get(task.task_id).result_json);
        }
        p.restart();
        REQUIRE(p.database->write([](sqlite::Connection& db) { return db.exec("DROP TRIGGER IF EXISTS reject_asset"); }));
        auto recovered = p.recover();
        CHECK(recovered.count(RecoveryAction::completed) == (boundary == 3 ? 0 : 1));
        CHECK(p.get(task.task_id).status == TaskStatus::completed);
        CHECK(read_bytes(p.at("Song.files/Song.mp3")) == "audio bytes");
        CHECK(read_bytes(p.at("Song.files/Song.lrc")) == "[00:00.00]test\n");
        CHECK(read_bytes(p.at("Song.files/Song.png")) == read_bytes(fs::path{MUSIC_SERVICE_TEST_FIXTURES} / "audio/cover.png"));
        CHECK(p.publications->pending()->empty());
        CHECK(p.recover().tasks.empty());
    }
}

TEST_CASE("bundles relocate together and missing or rewritten sidecars invalidate access", "[publication][sidecars]") {
    Publishing p{"bundle-access"};
    write_bytes(p.at("Song.files/keep.txt"), "keep");
    const auto task = p.processing("1");
    const auto done = test::unwrap(p.finalizer->publish(bundle_request(p, task)));
    CHECK(done.asset.relative_path == "Song (2).files/Song.mp3");
    CHECK(read_bytes(p.at("Song.files/keep.txt")) == "keep");
    application::media::FileAccessService access{p.publications, p.real_files, [](const auto&, const auto&) { return true; }};
    REQUIRE(access.open(p.owner, done.asset.id, "lyrics"));
    REQUIRE(access.open(p.owner, done.asset.id, "cover"));
    CHECK_FALSE(access.open("prn_other", done.asset.id, "cover"));
    CHECK_FALSE(access.open(p.owner, done.asset.id, "../input"));
    SECTION("same size corruption") { write_bytes(p.at("Song (2).files/Song.lrc"), "[00:00.00]FAIL\n"); }
    SECTION("deleted sidecar") { fs::remove(p.at("Song (2).files/Song.png")); }
    SECTION("deleted product directory") { fs::remove_all(p.at("Song (2).files")); }
    const auto opened = access.open(p.owner, done.asset.id);
    REQUIRE_FALSE(opened);
    CHECK(opened.error().code == ErrorCode::file_missing);
    CHECK(p.publications->get_asset(p.owner, done.asset.id)->status == ports::FileAssetStatus::missing);
}

TEST_CASE("recovery refuses a bundle whose sidecar changed after directory rename", "[publication][sidecars]") {
    Publishing p{"bundle-bad-recovery"};
    const auto task = p.processing("1");
    p.files->flush_fails = true;
    REQUIRE_FALSE(p.finalizer->publish(bundle_request(p, task)));
    fs::remove(p.at("Song.files/Song.lrc"));
    p.restart();
    CHECK(p.recover(0).count(RecoveryAction::failed) == 1);
    CHECK(p.get(task.task_id).status == TaskStatus::failed);
    CHECK_FALSE(p.get(task.task_id).result_json);
}

TEST_CASE("required missing sidecars fail before publication while optional failures are reported", "[sidecars]") {
    Publishing p{"bundle-required"};
    const auto task = p.processing("1");
    auto report = p.processed(task, "audio");
    report.output = p.at(p.work_file(task));
    downloads::FinalizeRequest request{task, p.work_file(task), "Song.mp3", report, "{}"};
    domain::OutputOptions options;
    options.export_lyrics = options.sidecars_required = true;
    const ports::OperationContext ctx{std::chrono::steady_clock::now() + std::chrono::seconds{10}, {}};
    REQUIRE_FALSE(downloads::prepare_sidecars(request, *p.real_files, {}, options, ctx));
    CHECK(p.publications->pending()->empty());
    CHECK_FALSE(fs::exists(p.at("Song.files")));
    options.sidecars_required = false;
    REQUIRE(downloads::prepare_sidecars(request, *p.real_files, {}, options, ctx));
    const auto done = test::unwrap(p.finalizer->publish(request));
    CHECK(nlohmann::json::parse(*done.task.result_json)["metadata_fields"]["lyrics_file"] == "unavailable");
    CHECK(nlohmann::json::parse(*done.task.result_json)["warnings"][0] == "SIDECAR_LYRICS_UNAVAILABLE");
}

TEST_CASE("a verified file is published as an immutable asset", "[publication]") {
    Publishing p{"publish"};
    const auto task = p.processing("1");
    const auto report = p.processed(task, "ID3 audio bytes");

    const auto done = test::unwrap(p.publish(task, "歌手/Song.mp3", report));
    CHECK(done.task.status == TaskStatus::completed);
    CHECK(done.asset.relative_path == "歌手/Song.mp3");
    CHECK(done.asset.size_bytes == 15);
    CHECK(done.asset.container == "mp3");
    CHECK(done.asset.etag.rfind("\"sha256-", 0) == 0);
    CHECK(done.asset.created_by_task == task.task_id);
    const auto result = nlohmann::json::parse(*done.task.result_json);
    CHECK(result["file_id"] == done.asset.id);
    CHECK(result["metadata_status"] == "skipped");
    CHECK(result["processing"]["processor"] == "audio.verify");

    CHECK(read_bytes(p.at("歌手/Song.mp3")) == "ID3 audio bytes");
    CHECK_FALSE(fs::exists(p.at(".music-service-work/" + task.task_id)));
    CHECK(p.publications->pending()->empty());
    CHECK(p.recover().tasks.empty());
}

TEST_CASE("publication never overwrites an unknown file or another reservation", "[publication]") {
    Publishing p{"names"};
    write_bytes(p.at("歌手/Song.mp3"), "someone else's file");

    const auto first = p.processing("1");
    const auto a = test::unwrap(p.publish(first, "歌手/Song.mp3", p.processed(first, "first")));
    CHECK(a.asset.relative_path == "歌手/Song (2).mp3");
    CHECK(read_bytes(p.at("歌手/Song.mp3")) == "someone else's file");

    // Names held by assets count without regard to letter case, on every system; files on
    // disk follow the file system, so only an asset decides it here.
    const auto second = p.processing("2");
    const auto b = test::unwrap(p.publish(second, "歌手/SONG (2).mp3", p.processed(second, "second")));
    CHECK(b.asset.relative_path == "歌手/SONG (2) (2).mp3");
    CHECK(read_bytes(p.at("歌手/Song (2).mp3")) == "first");

    // A file that appears after the reservation is not replaced either.
    const auto third = p.processing("3");
    const auto report = p.processed(third, "third");
    p.files->before_move = [&p] {
        p.files->before_move = nullptr;
        write_bytes(p.at("专辑/Track.flac"), "raced in");
    };
    const auto c = test::unwrap(p.publish(third, "专辑/Track.flac", report));
    CHECK(c.asset.relative_path == "专辑/Track (2).flac");
    CHECK(read_bytes(p.at("专辑/Track.flac")) == "raced in");
    CHECK(read_bytes(p.at("专辑/Track (2).flac")) == "third");
}

TEST_CASE("a deleted publication keeps its history but releases its original name", "[publication][downloads][redownload]") {
    Publishing p{"deleted-name"};
    const auto first = p.processing("1");
    const auto old = test::unwrap(p.publish(first, "歌手/Song.mp3", p.processed(first, "old audio")));
    REQUIRE(fs::remove(p.at(old.asset.relative_path)));
    SECTION("the original schema 8 asset is still marked available") {}
    SECTION("a file access already marked the old asset missing") {
        REQUIRE(p.publications->mark_missing(old.asset.id));
    }
    p.restart();
    const auto second = p.processing("1", "-replacement");
    const auto replacement = test::unwrap(p.publish(second, "歌手/Song.mp3", p.processed(second, "new audio")));
    CHECK(replacement.asset.relative_path == "歌手/Song.mp3");
    CHECK(replacement.asset.id != old.asset.id);
    CHECK(read_bytes(p.at("歌手/Song.mp3")) == "new audio");
    CHECK_FALSE(fs::exists(p.at("歌手/Song (2).mp3")));
    const auto historical = test::unwrap(p.publications->get_asset(p.owner, old.asset.id));
    CHECK(historical.status == ports::FileAssetStatus::missing);
    CHECK(historical.relative_path == old.asset.relative_path);
    CHECK(p.get(first.task_id).status == TaskStatus::completed);
    CHECK(p.get(first.task_id).result_json == old.task.result_json);
    application::media::FileAccessService access{p.publications, p.real_files,
        [](const std::string&, const std::string&) { return true; }};
    const auto old_access = access.open(p.owner, old.asset.id);
    REQUIRE_FALSE(old_access);
    CHECK(old_access.error().code == ErrorCode::file_missing);
    REQUIRE(access.open(p.owner, replacement.asset.id));
    p.restart();
    CHECK(p.publications->get_asset(p.owner, old.asset.id)->status == ports::FileAssetStatus::missing);
    CHECK(p.publications->get_asset(p.owner, replacement.asset.id)->status == ports::FileAssetStatus::available);
    REQUIRE(fs::remove(p.at(replacement.asset.relative_path)));
    const auto third = p.processing("1", "-third");
    const auto again = test::unwrap(p.publish(third, "歌手/Song.mp3", p.processed(third, "third audio")));
    CHECK(again.asset.relative_path == "歌手/Song.mp3");
    CHECK(p.publications->get_asset(p.owner, replacement.asset.id)->status == ports::FileAssetStatus::missing);
}

TEST_CASE("a deleted bundle reuses its directory while a partial bundle keeps its name", "[publication][sidecars][redownload]") {
    Publishing p{"deleted-bundle"};
    const auto first = p.processing("1");
    const auto old = test::unwrap(p.finalizer->publish(bundle_request(p, first)));
    bool whole_bundle = false;
    SECTION("the complete bundle was removed") {
        REQUIRE(fs::remove_all(p.at("Song.files")) > 0);
        whole_bundle = true;
    }
    SECTION("only its audio was removed") {
        REQUIRE(fs::remove(p.at(old.asset.relative_path)));
    }
    const auto second = p.processing("2");
    const auto replacement = test::unwrap(p.finalizer->publish(bundle_request(p, second)));
    CHECK(replacement.asset.relative_path == (whole_bundle ? "Song.files/Song.mp3" : "Song (2).files/Song.mp3"));
    if (whole_bundle) CHECK(p.publications->get_asset(p.owner, old.asset.id)->status == ports::FileAssetStatus::missing);
    else CHECK(fs::exists(p.at("Song.files/Song.lrc")));
}

TEST_CASE("a reclaimed name never overwrites a file restored before rename", "[publication][redownload]") {
    Publishing p{"restored-name"};
    const auto first = p.processing("1");
    const auto old = test::unwrap(p.publish(first, "Song.mp3", p.processed(first, "old audio")));
    REQUIRE(fs::remove(p.at(old.asset.relative_path)));
    const auto second = p.processing("2");
    p.files->before_move = [&p] {
        p.files->before_move = nullptr;
        write_bytes(p.at("Song.mp3"), "externally restored");
    };
    const auto replacement = test::unwrap(p.publish(second, "Song.mp3", p.processed(second, "new audio")));
    CHECK(replacement.asset.relative_path == "Song (2).mp3");
    CHECK(read_bytes(p.at("Song.mp3")) == "externally restored");
    CHECK(read_bytes(p.at("Song (2).mp3")) == "new audio");
    CHECK(p.publications->get_asset(p.owner, old.asset.id)->status == ports::FileAssetStatus::missing);
}

TEST_CASE("reclaimed publication names remain exclusive across concurrent tasks", "[publication][redownload]") {
    Publishing p{"concurrent-name"};
    const auto old_task = p.processing("1");
    const auto old = test::unwrap(p.publish(old_task, "Song.mp3", p.processed(old_task, "old audio")));
    REQUIRE(fs::remove(p.at(old.asset.relative_path)));
    const auto a = p.processing("2");
    const auto b = p.processing("3");
    const auto report_a = p.processed(a, "audio A");
    const auto report_b = p.processed(b, "audio B");
    std::array<std::optional<domain::Result<ports::CompletedPublication>>, 2> results;
    std::barrier start{2};
    {
        std::jthread one{[&] { start.arrive_and_wait(); results[0] = p.publish(a, "Song.mp3", report_a); }};
        std::jthread two{[&] { start.arrive_and_wait(); results[1] = p.publish(b, "Song.mp3", report_b); }};
    }
    REQUIRE(results[0].has_value());
    REQUIRE(results[1].has_value());
    const auto done_a = test::unwrap(std::move(*results[0]));
    const auto done_b = test::unwrap(std::move(*results[1]));
    CHECK(done_a.asset.relative_path != done_b.asset.relative_path);
    CHECK((done_a.asset.relative_path == "Song.mp3" || done_b.asset.relative_path == "Song.mp3"));
    CHECK(read_bytes(p.at(done_a.asset.relative_path)) == "audio A");
    CHECK(read_bytes(p.at(done_b.asset.relative_path)) == "audio B");
    CHECK(p.publications->get_asset(p.owner, old.asset.id)->status == ports::FileAssetStatus::missing);
}

TEST_CASE("reclaiming a publication path rolls back on stale evidence or storage failure", "[publication][redownload]") {
    Publishing p{"reclaim-rollback"};
    const auto first = p.processing("1");
    const auto old = test::unwrap(p.publish(first, "Song.mp3", p.processed(first, "old audio")));
    REQUIRE(fs::remove(p.at(old.asset.relative_path)));
    const auto next = p.processing("2");
    (void)p.processed(next, "new audio");
    ports::PublicationIntent intent{next.task_id, *next.execution_id, "dst_main", p.work_file(next),
                                    "Song.mp3", "{}", "reserved"};
    ports::MissingPathAsset claim{old.asset.id, old.asset.version};
    SECTION("a stale asset version cannot release the path") {
        REQUIRE(p.publications->mark_missing(old.asset.id));
    }
    SECTION("a claim cannot release another path") {
        intent.reserved_path = "Other.mp3";
    }
    SECTION("cancellation wins before any path is released") {
        REQUIRE(p.tasks->request_cancel(p.owner, next.task_id));
    }
    SECTION("a failed reservation insert rolls back the released name") {
        REQUIRE(p.database->write([](sqlite::Connection& db) {
            return db.exec("CREATE TRIGGER reject_reservation BEFORE INSERT ON finalization_intents "
                           "BEGIN SELECT RAISE(ABORT, 'injected reservation failure'); END");
        }));
    }
    const auto before = test::unwrap(p.publications->get_asset(p.owner, old.asset.id));
    const auto task_before = p.get(next.task_id);
    REQUIRE_FALSE(p.publications->begin(intent, next.version, claim));
    const auto after = test::unwrap(p.publications->get_asset(p.owner, old.asset.id));
    CHECK(after.status == before.status);
    CHECK(after.version == before.version);
    const auto occupant = test::unwrap(p.publications->asset_at_path("dst_main", "Song.mp3"));
    REQUIRE(occupant);
    CHECK(occupant->id == old.asset.id);
    CHECK(p.get(next.task_id).status == task_before.status);
    CHECK(p.publications->pending()->empty());
}

TEST_CASE("a crash after reclaiming a deleted name recovers at the original path", "[publication][recovery][redownload]") {
    Publishing p{"reclaim-recovery"};
    const auto first = p.processing("1");
    const auto old = test::unwrap(p.publish(first, "Song.mp3", p.processed(first, "old audio")));
    REQUIRE(fs::remove(p.at(old.asset.relative_path)));
    const auto second = p.processing("2");
    SECTION("interrupted before rename") {
        p.files->before_move = [] { throw std::runtime_error("crash after path reservation"); };
        REQUIRE_THROWS(p.publish(second, "Song.mp3", p.processed(second, "new audio")));
    }
    SECTION("interrupted after rename but before asset commit") {
        p.files->flush_fails = true;
        REQUIRE_FALSE(p.publish(second, "Song.mp3", p.processed(second, "new audio")));
    }
    CHECK(p.get(second.task_id).status == TaskStatus::finalizing);
    p.restart();
    CHECK(p.recover().count(RecoveryAction::completed) == 1);
    CHECK(read_bytes(p.at("Song.mp3")) == "new audio");
    CHECK_FALSE(fs::exists(p.at("Song (2).mp3")));
    CHECK(p.publications->get_asset(p.owner, old.asset.id)->status == ports::FileAssetStatus::missing);
}

TEST_CASE("an inspection failure never releases an old asset name", "[publication][redownload]") {
    Publishing p{"reclaim-inspect-failure"};
    const auto first = p.processing("1");
    const auto old = test::unwrap(p.publish(first, "Song.mp3", p.processed(first, "old audio")));
    REQUIRE(fs::remove(p.at(old.asset.relative_path)));
    SECTION("destination offline") {
        p.files->inspect_error = domain::Error{ErrorCode::destination_unavailable, "offline", true, {}};
    }
    SECTION("permission or disk error") {
        p.files->inspect_error = domain::Error{ErrorCode::file_system, "inspect failed", false, {}};
    }
    const auto second = p.processing("2");
    const auto result = p.publish(second, "Song.mp3", p.processed(second, "new audio"));
    REQUIRE_FALSE(result);
    CHECK(result.error().code == p.files->inspect_error->code);
    const auto historical = test::unwrap(p.publications->get_asset(p.owner, old.asset.id));
    CHECK(historical.status == ports::FileAssetStatus::available);
    CHECK(historical.version == old.asset.version);
    CHECK(p.publications->pending()->empty());
}

TEST_CASE("relocation can reclaim a deleted numbered name after an external collision", "[publication][redownload]") {
    Publishing p{"relocate-reclaim"};
    const auto first = p.processing("1");
    const auto old = test::unwrap(p.publish(first, "Song (2).mp3", p.processed(first, "old audio")));
    REQUIRE(fs::remove(p.at(old.asset.relative_path)));
    const auto second = p.processing("2");
    p.files->before_move = [&p] {
        p.files->before_move = nullptr;
        write_bytes(p.at("Song.mp3"), "external audio");
    };
    const auto result = test::unwrap(p.publish(second, "Song.mp3", p.processed(second, "new audio")));
    CHECK(result.asset.relative_path == "Song (2).mp3");
    CHECK(read_bytes(p.at("Song.mp3")) == "external audio");
    CHECK(read_bytes(p.at("Song (2).mp3")) == "new audio");
    CHECK(p.publications->get_asset(p.owner, old.asset.id)->status == ports::FileAssetStatus::missing);
}

TEST_CASE("a file that failed or skipped validation is not published (A19)", "[publication]") {
    Publishing p{"unverified"};
    const auto task = p.processing("1");
    auto report = p.processed(task, "<html>error</html>");

    auto failed = report;
    failed.stages[1].outcome = ports::StageOutcome::failed;
    auto skipped = report;
    skipped.stages[1].outcome = ports::StageOutcome::skipped;
    auto changed = report;
    changed.facts.size_bytes += 1; // the bytes are not the ones that were verified
    auto unhashed = report;
    unhashed.facts.sha256.clear(); // nothing names the verified bytes
    for (const auto& bad : {failed, skipped, changed, unhashed}) {
        const auto refused = p.publish(task, "Song.mp3", bad);
        REQUIRE_FALSE(refused);
        CHECK(refused.error().code == ErrorCode::audio_invalid);
    }
    CHECK(p.get(task.task_id).status == TaskStatus::processing);
    CHECK_FALSE(fs::exists(p.at("Song.mp3")));
    CHECK(read_bytes(p.at(p.work_file(task))) == "<html>error</html>");
    CHECK(p.publications->pending()->empty());

    // Rewritten with as many other bytes after validation: same size, other hash.
    write_bytes(p.at(p.work_file(task)), std::string(18, '\0'));
    const auto rewritten = p.publish(task, "Song.mp3", report);
    REQUIRE_FALSE(rewritten);
    CHECK(rewritten.error().code == ErrorCode::audio_invalid);
    CHECK(rewritten.error().details.front().reason == "changed_after_validation");
    CHECK_FALSE(fs::exists(p.at("Song.mp3")));
    CHECK(p.publications->pending()->empty());
    write_bytes(p.at(p.work_file(task)), "<html>error</html>");

    // A failed tag stage under best_effort is a warning, not a refusal.
    auto tags_failed = report;
    tags_failed.stages[2].outcome = ports::StageOutcome::failed;
    const auto done = test::unwrap(p.publish(task, "Song.mp3", tags_failed));
    CHECK(nlohmann::json::parse(*done.task.result_json)["metadata_status"] == "failed");
}

TEST_CASE("cancel and publication are ordered by the task version (A12)", "[publication]") {
    Publishing p{"cancel"};

    SECTION("cancel first: nothing is reserved or moved") {
        const auto task = p.processing("1");
        const auto report = p.processed(task, "bytes");
        CHECK(p.tasks->request_cancel(p.owner, task.task_id)->status == TaskStatus::canceling);
        const auto lost = p.publish(task, "Song.mp3", report);
        REQUIRE_FALSE(lost);
        CHECK(lost.error().code == ErrorCode::version_conflict);
        CHECK(p.get(task.task_id).status == TaskStatus::canceling);
        CHECK_FALSE(fs::exists(p.at("Song.mp3")));
        CHECK(p.publications->pending()->empty());

        // The execution is gone at the next start: canceled, work removed.
        p.restart();
        const auto report_after = p.recover();
        REQUIRE(report_after.tasks.size() == 1);
        CHECK(report_after.tasks[0].action == RecoveryAction::canceled);
        CHECK(p.get(task.task_id).status == TaskStatus::canceled);
        CHECK_FALSE(fs::exists(p.at(".music-service-work/" + task.task_id)));
    }

    SECTION("publication first: the cancel is refused and the file completes") {
        const auto task = p.processing("1");
        const auto report = p.processed(task, "bytes");
        std::optional<domain::Error> refused;
        p.files->before_move = [&] {
            auto cancel = p.tasks->request_cancel(p.owner, task.task_id);
            if (!cancel) {
                refused = cancel.error();
            }
        };
        const auto done = test::unwrap(p.publish(task, "Song.mp3", report));
        REQUIRE(refused);
        CHECK(refused->code == ErrorCode::task_finalizing);
        CHECK(done.task.status == TaskStatus::completed);
        CHECK(p.tasks->request_cancel(p.owner, task.task_id).error().code == ErrorCode::task_terminal);
    }
}

TEST_CASE("a destination write failure fails the task without completing it (A17)", "[publication]") {
    Publishing p{"write-failure"};
    const auto task = p.processing("1");
    const auto report = p.processed(task, "bytes");
    write_bytes(p.at("歌手"), "a file where the directory should be");

    const auto failed = p.publish(task, "歌手/Song.mp3", report);
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == ErrorCode::finalization_failed);
    const auto stored = p.get(task.task_id);
    CHECK(stored.status == TaskStatus::failed);
    CHECK(nlohmann::json::parse(*stored.error_json)["code"] == "FINALIZATION_FAILED");
    // The reservation is released, the work file is kept for diagnosis.
    CHECK(p.publications->pending()->empty());
    CHECK(read_bytes(p.at(p.work_file(task))) == "bytes");

    // An offline destination is reported before anything is reserved.
    const auto other = p.processing("2");
    const auto other_report = p.processed(other, "other");
    fs::rename(p.root, p.dir / "offline");
    const auto offline = p.publish(other, "Other.mp3", other_report);
    fs::rename(p.dir / "offline", p.root);
    REQUIRE_FALSE(offline);
    CHECK(offline.error().code == ErrorCode::destination_unavailable);
    CHECK(p.get(other.task_id).status == TaskStatus::processing);
}

TEST_CASE("a move whose flush failed keeps the intent for recovery", "[publication][recovery]") {
    Publishing p{"flush-failure"};
    const auto task = p.processing("1");
    const auto report = p.processed(task, "bytes");
    p.files->flush_fails = true;
    const auto unconfirmed = p.publish(task, "Song.mp3", report);
    REQUIRE_FALSE(unconfirmed);
    CHECK(downloads::is_move_unconfirmed(unconfirmed.error()));
    // Not failed: the file is at the target, the work file is gone, the intent remains.
    CHECK(p.get(task.task_id).status == TaskStatus::finalizing);
    CHECK(p.publications->pending()->size() == 1);
    CHECK(read_bytes(p.at("Song.mp3")) == "bytes");
    CHECK_FALSE(fs::exists(p.at(p.work_file(task))));

    // Recovery moving the work file fails the same way: it waits instead of failing.
    const auto stalled = p.crash_after_intent("2", "two", "Two.mp3");
    const auto deferred = p.recover();
    REQUIRE(deferred.tasks.size() == 2);
    CHECK(deferred.count(RecoveryAction::completed) == 1); // the first one's target matches
    CHECK(deferred.count(RecoveryAction::deferred) == 1);
    CHECK(p.get(stalled.task_id).status == TaskStatus::finalizing);
    CHECK(p.publications->pending()->size() == 1);

    p.restart();
    const auto done = p.recover();
    REQUIRE(done.tasks.size() == 1);
    CHECK(done.tasks[0].action == RecoveryAction::completed);
    CHECK(p.get(task.task_id).status == TaskStatus::completed);
    CHECK(p.get(stalled.task_id).status == TaskStatus::completed);
    CHECK(read_bytes(p.at("Two.mp3")) == "two");
    CHECK(p.publications->pending()->empty());
}

TEST_CASE("recovery completes publications from the intent (A20)", "[publication][recovery]") {
    Publishing p{"recover"};
    const auto not_moved = p.crash_after_intent("1", "one", "A/One.mp3");
    const auto moved = p.crash_after_intent("2", "two", "A/Two.mp3");
    write_bytes(p.at("A/Two.mp3"), "two");
    fs::remove(p.at(p.work_file(moved)));
    const auto linked = p.crash_after_intent("3", "three", "A/Three.mp3");
    write_bytes(p.at("A/Three.mp3"), "three"); // rename by link+unlink stopped halfway
    const auto occupied = p.crash_after_intent("4", "four", "A/Four.mp3");
    write_bytes(p.at("A/Four.mp3"), "unknown");

    p.restart();
    const auto report = p.recover();
    REQUIRE(report.tasks.size() == 4);
    CHECK(report.count(RecoveryAction::completed) == 4);

    for (const auto& [task, path, bytes] :
         {std::tuple{not_moved, "A/One.mp3", "one"}, std::tuple{moved, "A/Two.mp3", "two"},
          std::tuple{linked, "A/Three.mp3", "three"}, std::tuple{occupied, "A/Four (2).mp3", "four"}}) {
        const auto stored = p.get(task.task_id);
        CHECK(stored.status == TaskStatus::completed);
        const auto file_id = nlohmann::json::parse(*stored.result_json)["file_id"].get<std::string>();
        const auto asset = test::unwrap(p.publications->get_asset(p.owner, file_id));
        CHECK(asset.relative_path == path);
        CHECK(read_bytes(p.at(path)) == bytes);
        CHECK_FALSE(fs::exists(p.at(".music-service-work/" + task.task_id)));
    }
    CHECK(read_bytes(p.at("A/Four.mp3")) == "unknown");
    CHECK(p.publications->pending()->empty());
    CHECK(p.recover().tasks.empty()); // nothing left to do on the next start
}

TEST_CASE("recovery without an intact product retries or fails, never completes", "[publication][recovery]") {
    Publishing p{"no-product"};
    const auto missing = p.crash_after_intent("1", "one", "One.mp3");
    fs::remove(p.at(p.work_file(missing)));
    const auto altered = p.crash_after_intent("2", "two", "Two.mp3");
    write_bytes(p.at(p.work_file(altered)), "TWO"); // same size, other bytes
    const auto foreign = p.crash_after_intent("3", "three", "Three.mp3");
    fs::remove(p.at(p.work_file(foreign)));
    write_bytes(p.at("Three.mp3"), "not ours");

    const auto report = p.recover();
    CHECK(report.count(RecoveryAction::retry) == 3);
    for (const auto& task : {missing, altered, foreign}) {
        const auto stored = p.get(task.task_id);
        CHECK(stored.status == TaskStatus::waiting_retry);
        CHECK(stored.wait_reason == domain::WaitReason::upstream_retry);
        CHECK(stored.resume_stage == TaskStatus::resolving);
    }
    CHECK(read_bytes(p.at("Three.mp3")) == "not ours");
    CHECK(read_bytes(p.at(p.work_file(altered))) == "TWO"); // kept for diagnosis
    CHECK(p.publications->pending()->empty());

    // Without attempts left the task fails with FINALIZATION_FAILED.
    const auto exhausted = p.crash_after_intent("4", "four", "Four.mp3");
    fs::remove(p.at(p.work_file(exhausted)));
    const auto last = p.recover(0);
    REQUIRE(last.tasks.size() == 1);
    CHECK(last.tasks[0].action == RecoveryAction::failed);
    const auto stored = p.get(exhausted.task_id);
    CHECK(stored.status == TaskStatus::failed);
    CHECK(nlohmann::json::parse(*stored.error_json)["code"] == "FINALIZATION_FAILED");
}

TEST_CASE("recovery waits for an offline destination", "[publication][recovery]") {
    Publishing p{"offline"};
    const auto task = p.crash_after_intent("1", "one", "One.mp3");

    fs::rename(p.root, p.dir / "unmounted");
    const auto deferred = p.recover();
    fs::rename(p.dir / "unmounted", p.root);
    REQUIRE(deferred.tasks.size() == 1);
    CHECK(deferred.tasks[0].action == RecoveryAction::deferred);
    CHECK(p.get(task.task_id).status == TaskStatus::finalizing);
    CHECK(p.publications->pending()->size() == 1);

    const auto back = p.recover();
    CHECK(back.count(RecoveryAction::completed) == 1);
    CHECK(read_bytes(p.at("One.mp3")) == "one");
}

TEST_CASE("a restarted service completes an interrupted publication", "[publication][server]") {
    using namespace music_service::test;
    Deployment d{"publication-restart"};
    REQUIRE(d.run({"init"}).code == server::exit_ok);
    const auto token = d.read(d.bootstrap());
    std::string task_id;
    {
        RunningService service{d, std::make_shared<infrastructure::observability::MemorySink>(),
                               std::vector<ports::ProviderBundle>{mock_combo("both")}, true};
        auto api = service.client(token);
        const auto created = api.Post("/api/v1/downloads", {{"Idempotency-Key", "publication-restart-1"}},
                                      R"({"track":{"source":"both","id":"1"},"metadata_policy":"skip"})",
                                      "application/json");
        REQUIRE(created);
        REQUIRE(created->status == 202);
        task_id = body(created)["data"]["task_id"].get<std::string>();
    }

    // What a runner leaves behind when the process ends after the rename (T17 runs this).
    const auto root = test::unwrap(infrastructure::filesystem::canonical_directory(d.dir / "下载"));
    {
        auto clock = std::make_shared<infrastructure::SystemClock>();
        auto database = sqlite::Database::open(d.dir / "data" / "service.sqlite3", {}).value();
        const sqlite::RepositoryContext ctx{database.get(), clock,
                                            std::make_shared<infrastructure::observability::IdGenerator>()};
        sqlite::SqliteTaskRepository tasks{ctx, sqlite::TaskRepositoryOptions{}};
        sqlite::SqlitePublicationRepository publications{ctx};
        auto task = *test::unwrap(tasks.claim_next(clock->now()));
        REQUIRE(task.task_id == task_id);
        for (const auto to : {TaskStatus::downloading, TaskStatus::processing}) {
            task = test::unwrap(tasks.transition({task.task_id, *task.execution_id, task.version, to, {}, {}, {}}));
        }
        write_bytes(root / path_from_utf8("歌手/歌曲.mp3"), "published bytes");
        infrastructure::filesystem::LocalDestinationFiles files{{{"dst_main", root}}};
        const auto file = *test::unwrap(files.fingerprint("dst_main", "歌手/歌曲.mp3"));
        const downloads::PublicationEvidence evidence{file, "歌手/歌曲.mp3", "mp3", "{}", "{}"};
        REQUIRE(publications.begin({task.task_id, *task.execution_id, "dst_main",
                                    ".music-service-work/" + task.task_id + "/processed.part", "歌手/歌曲.mp3",
                                    downloads::encode_evidence(evidence), "moved"},
                                   task.version));
    }

    auto sink = std::make_shared<infrastructure::observability::MemorySink>();
    RunningService service{d, sink, std::vector<ports::ProviderBundle>{mock_combo("both")}};
    const auto task = body(service.client(token).Get("/api/v1/downloads/" + task_id))["data"];
    CHECK(task["status"] == "completed");
    CHECK(task["result"]["file_id"].get<std::string>().rfind("fil_", 0) == 0);
    CHECK(read_bytes(root / path_from_utf8("歌手/歌曲.mp3")) == "published bytes");
    bool logged = false;
    for (const auto& record : sink->records()) {
        logged = logged || record.event == "startup.recovery";
    }
    CHECK(logged);
}

TEST_CASE("a starting service removes work directories of unknown tasks only", "[publication][server][maintenance]") {
    using namespace music_service::test;
    Deployment d{"startup-maintenance"};
    REQUIRE(d.run({"init"}).code == server::exit_ok);
    const auto token = d.read(d.bootstrap());
    std::string task_id;
    {
        RunningService service{d, std::make_shared<infrastructure::observability::MemorySink>(),
                               std::vector<ports::ProviderBundle>{mock_combo("both")}, true};
        auto api = service.client(token);
        const auto created = api.Post("/api/v1/downloads", {{"Idempotency-Key", "startup-maintenance-1"}},
                                      R"({"track":{"source":"both","id":"1"}})", "application/json");
        REQUIRE(created);
        REQUIRE(created->status == 202);
        task_id = body(created)["data"]["task_id"].get<std::string>();
    }
    const auto work = d.dir / "下载" / ".music-service-work";
    const std::string orphan = "tsk_LeftByAnOlderDatabase";
    for (const auto& id : {task_id, orphan}) {
        fs::create_directories(work / id);
        std::ofstream{work / id / "audio.part", std::ios::binary} << "bytes";
    }

    const auto sink = std::make_shared<infrastructure::observability::MemorySink>();
    RunningService service{d, sink, std::vector<ports::ProviderBundle>{mock_combo("both")}, true};
    CHECK(fs::exists(work / task_id / "audio.part")); // the queued task still owns it
    CHECK_FALSE(fs::exists(work / orphan));
    bool logged = false;
    for (const auto& record : sink->records()) {
        if (record.event == "maintenance.workspace_removed") {
            logged = true;
        }
    }
    CHECK(logged);
}
