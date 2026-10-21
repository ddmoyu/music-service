// Publication of verified files and startup recovery on real SQLite and a real
// destination directory (A12, A17, A19 basics, A20): reservations, renames that never
// replace, the crash matrix of internal-contracts.md 7, and a service restart.

#include "../unit/domain/test_support.hpp"
#include "service_harness.hpp"

#include "downloads/finalizer.hpp"
#include "downloads/recovery.hpp"
#include "infrastructure/clock.hpp"
#include "infrastructure/filesystem/destination_files.hpp"
#include "infrastructure/filesystem/paths.hpp"
#include "infrastructure/observability/correlation.hpp"
#include "infrastructure/storage/sqlite/account_repositories.hpp"
#include "infrastructure/storage/sqlite/migrations.hpp"
#include "infrastructure/storage/sqlite/task_repositories.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <functional>

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

    domain::Result<bool> exists(const std::string& d, const std::string& p) override { return inner_->exists(d, p); }
    domain::Result<std::optional<ports::FileFingerprint>> fingerprint(const std::string& d,
                                                                      const std::string& p) override {
        return inner_->fingerprint(d, p);
    }
    domain::Result<void> move(const std::string& d, const std::string& from, const std::string& to) override {
        if (before_move) {
            before_move();
        }
        return inner_->move(d, from, to);
    }
    domain::Result<void> remove_workspace(const std::string& d, const std::string& t) override {
        return inner_->remove_workspace(d, t);
    }

    std::function<void()> before_move;

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
        REQUIRE(sqlite::migrate(*database, clock->now()));
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
    ports::TaskRecord processing(std::string_view track_id) {
        domain::DownloadIntentDraft draft{owner,
                                          test::track("kuwo", track_id),
                                          domain::SameSource{},
                                          {test::anonymous(owner, "kuwo")},
                                          {domain::Quality::standard, domain::QualityPolicy::strict, false},
                                          domain::MetadataPolicy::best_effort,
                                          {"dst_main", "d1", "n1", "m1"}};
        auto intent = test::unwrap(domain::NormalizedDownloadIntent::normalize(std::move(draft)));
        REQUIRE(tasks->create({std::move(intent), "key-" + std::string{track_id}, "h", false}));
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

    // Names held by assets count without regard to letter case, on every system.
    const auto second = p.processing("2");
    const auto b = test::unwrap(p.publish(second, "歌手/SONG.mp3", p.processed(second, "second")));
    CHECK(b.asset.relative_path == "歌手/SONG (3).mp3");
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
    for (const auto& bad : {failed, skipped, changed}) {
        const auto refused = p.publish(task, "Song.mp3", bad);
        REQUIRE_FALSE(refused);
        CHECK(refused.error().code == ErrorCode::audio_invalid);
    }
    CHECK(p.get(task.task_id).status == TaskStatus::processing);
    CHECK_FALSE(fs::exists(p.at("Song.mp3")));
    CHECK(read_bytes(p.at(p.work_file(task))) == "<html>error</html>");
    CHECK(p.publications->pending()->empty());

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
                               std::vector<ports::ProviderBundle>{mock_combo("both")}};
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
