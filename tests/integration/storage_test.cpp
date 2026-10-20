// SQLite storage against real database files in a temporary directory.

#include "../unit/domain/test_support.hpp"

#include "infrastructure/clock.hpp"
#include "infrastructure/filesystem/paths.hpp"
#include "infrastructure/storage/sqlite/account_repositories.hpp"
#include "infrastructure/storage/sqlite/migrations.hpp"
#include "infrastructure/storage/sqlite/task_repositories.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <stdexcept>
#include <thread>

using namespace music_service;
using namespace music_service::infrastructure::storage::sqlite;
namespace ports = application::ports;
namespace fs = std::filesystem;
using domain::ErrorCode;
using domain::TaskStatus;

namespace {

struct Store {
    explicit Store(std::string_view name, TaskRepositoryOptions task_options = {},
                   DatabaseOptions database_options = {})
        : dir(fs::temp_directory_path() /
              infrastructure::filesystem::path_from_utf8("music-service-存储-" + std::string{name})),
          options(task_options), db_options(database_options) {
        fs::remove_all(dir);
        fs::create_directories(dir);
        reopen();
    }
    ~Store() {
        repos_reset();
        database.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    void repos_reset() {
        identity.reset();
        credentials.reset();
        destinations.reset();
        tasks.reset();
        publications.reset();
    }

    void reopen() {
        repos_reset();
        database.reset();
        database = Database::open(dir / "service.sqlite3", db_options).value();
        REQUIRE(*migrate(*database, clock->now()) == 1);
        const RepositoryContext ctx{database.get(), clock, ids};
        identity = std::make_unique<SqliteIdentityRepository>(ctx);
        credentials = std::make_unique<SqliteCredentialRepository>(ctx);
        destinations = std::make_unique<SqliteDestinationRepository>(ctx);
        tasks = std::make_unique<SqliteTaskRepository>(ctx, options);
        publications = std::make_unique<SqlitePublicationRepository>(ctx);
    }

    fs::path dir;
    TaskRepositoryOptions options;
    DatabaseOptions db_options;
    std::shared_ptr<infrastructure::ManualClock> clock = std::make_shared<infrastructure::ManualClock>();
    std::shared_ptr<infrastructure::observability::IdGenerator> ids =
        std::make_shared<infrastructure::observability::IdGenerator>(1234);
    std::unique_ptr<Database> database;
    std::unique_ptr<SqliteIdentityRepository> identity;
    std::unique_ptr<SqliteCredentialRepository> credentials;
    std::unique_ptr<SqliteDestinationRepository> destinations;
    std::unique_ptr<SqliteTaskRepository> tasks;
    std::unique_ptr<SqlitePublicationRepository> publications;
};

std::string make_principal(Store& s, std::string name = "local") {
    return s.identity->create_principal(std::move(name), ports::PrincipalRole::user)->id;
}

domain::NormalizedDownloadIntent intent_for(const std::string& owner, std::string_view track_id,
                                            bool allow_preview = false) {
    domain::DownloadIntentDraft draft{
        owner,
        test::track("kuwo", track_id),
        domain::SameSource{},
        {test::anonymous(owner, "kuwo")},
        {domain::Quality::standard, domain::QualityPolicy::strict, allow_preview},
        domain::MetadataPolicy::best_effort,
        {"dst_default", "d1", "n1", "m1"},
    };
    return domain::NormalizedDownloadIntent::normalize(std::move(draft)).value();
}

ports::CreateTaskCommand create_command(const std::string& owner, std::string_view track_id,
                                        std::string key, std::string hash = "h1") {
    return ports::CreateTaskCommand{intent_for(owner, track_id), std::move(key), std::move(hash), false};
}

// Creates a task and claims it, returning the running record.
ports::TaskRecord running_task(Store& s, const std::string& owner, std::string_view track_id) {
    REQUIRE(s.tasks->create(create_command(owner, track_id, "key-" + std::string{track_id})));
    auto claimed = s.tasks->claim_next(s.clock->now());
    REQUIRE(claimed);
    REQUIRE(claimed->has_value());
    return **claimed;
}

ports::TaskRecord step(Store& s, const ports::TaskRecord& task, TaskStatus to) {
    auto next = s.tasks->transition({task.task_id, *task.execution_id, task.version, to, {}, {}, {}});
    REQUIRE(next);
    return *next;
}

} // namespace

TEST_CASE("migrations run once and reopen cleanly", "[storage][migrations]") {
    Store s{"migrate"};
    CHECK(*migrate(*s.database, s.clock->now()) == 1);

    const auto mode = s.database->read([](Connection& db) -> domain::Result<std::string> {
        auto stmt = db.prepare("PRAGMA journal_mode");
        REQUIRE(stmt);
        REQUIRE(*stmt->step());
        return stmt->text(0);
    });
    CHECK(*mode == "wal");

    s.reopen();
    CHECK(*migrate(*s.database, s.clock->now()) == 1);
}

TEST_CASE("a newer or altered schema is refused", "[storage][migrations]") {
    Store s{"newer"};
    REQUIRE(s.database->write([](Connection& db) {
        return db.exec("INSERT INTO schema_migrations VALUES (99, 'x', 0)");
    }));
    const auto newer = migrate(*s.database, s.clock->now());
    REQUIRE_FALSE(newer);
    CHECK(newer.error().code == ErrorCode::storage_unavailable);

    REQUIRE(s.database->write([](Connection& db) {
        return db.exec("DELETE FROM schema_migrations WHERE version = 99;"
                       "UPDATE schema_migrations SET checksum = 'changed' WHERE version = 1;");
    }));
    CHECK_FALSE(migrate(*s.database, s.clock->now()));
}

TEST_CASE("failed writes roll back", "[storage][database]") {
    Store s{"rollback"};
    const auto failed = s.database->write([](Connection& db) -> domain::Result<void> {
        REQUIRE(db.exec("INSERT INTO destinations VALUES ('dst_x', '/x', 0)"));
        return domain::Error{ErrorCode::internal, "boom", false, {}};
    });
    REQUIRE_FALSE(failed);
    CHECK(s.destinations->get("dst_x").error().code == ErrorCode::not_found);
}

TEST_CASE("a throwing write rolls back and frees the writer", "[storage][database]") {
    Store s{"throw"};
    CHECK_THROWS_AS(s.database->write([](Connection& db) -> domain::Result<void> {
        REQUIRE(db.exec("INSERT INTO destinations VALUES ('dst_x', '/x', 0)"));
        throw std::runtime_error{"boom"};
    }),
                    std::runtime_error);
    CHECK(s.destinations->get("dst_x").error().code == ErrorCode::not_found);
    REQUIRE(s.destinations->bind("dst_y", "/y"));
    CHECK(s.destinations->get("dst_y"));
}

TEST_CASE("the single writer is bounded by the wait budget", "[storage][database]") {
    Store s{"writer", {}, DatabaseOptions{2, 8, std::chrono::milliseconds{50}}};
    std::atomic<bool> holding{false};
    std::atomic<bool> release{false};
    std::thread holder{[&] {
        (void)s.database->write([&](Connection&) -> domain::Result<void> {
            holding = true;
            while (!release) {
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            }
            return domain::ok();
        });
    }};
    while (!holding) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    const auto blocked = s.database->write([](Connection&) -> domain::Result<void> { return domain::ok(); });
    REQUIRE_FALSE(blocked);
    CHECK(blocked.error().code == ErrorCode::storage_unavailable);
    CHECK(blocked.error().retryable);

    // Readers are not blocked by the writer.
    CHECK(s.destinations->list());
    release = true;
    holder.join();
    CHECK(s.database->write([](Connection&) -> domain::Result<void> { return domain::ok(); }));
}

TEST_CASE("principals and tokens", "[storage][identity]") {
    Store s{"identity"};
    const auto principal = s.identity->create_principal("本机", ports::PrincipalRole::admin);
    REQUIRE(principal);
    CHECK(principal->id.rfind("prn_", 0) == 0);
    CHECK(principal->enabled);

    const auto token = s.identity->insert_token(principal->id, "hash-1", {"catalog:read", "admin"});
    REQUIRE(token);
    CHECK(token->scopes == std::vector<std::string>{"catalog:read", "admin"});
    CHECK(s.identity->insert_token(principal->id, "hash-1", {"admin"}).error().code ==
          ErrorCode::conflict);

    auto auth = s.identity->authenticate("hash-1");
    REQUIRE(auth);
    REQUIRE(auth->has_value());
    CHECK((*auth)->principal.id == principal->id);
    CHECK_FALSE(s.identity->authenticate("unknown")->has_value());

    REQUIRE(s.identity->set_principal_enabled(principal->id, false));
    CHECK_FALSE(s.identity->authenticate("hash-1")->has_value());
    CHECK_FALSE(s.identity->insert_token(principal->id, "hash-2", {"admin"}));
    REQUIRE(s.identity->set_principal_enabled(principal->id, true));
    CHECK(s.identity->authenticate("hash-1")->has_value());

    REQUIRE(s.identity->revoke_token(token->id));
    CHECK(s.identity->revoke_token(token->id)); // repeated revoke succeeds
    CHECK_FALSE(s.identity->authenticate("hash-1")->has_value());
    CHECK(s.identity->revoke_token("tok_missing").error().code == ErrorCode::not_found);
    CHECK(s.identity->list_tokens(principal->id)->size() == 1);
    CHECK(s.identity->list_principals()->size() == 1);
}

// A47: default selection versions, owner checks and atomic revoke.
TEST_CASE("default credential selection", "[storage][credentials]") {
    Store s{"defaults"};
    const auto owner = make_principal(s);
    const auto other = make_principal(s, "other");
    const auto a = s.credentials->insert({owner, "qq", "A", "cookie", "cookie", "sec_a", {}}).value();
    const auto b = s.credentials->insert({owner, "qq", "B", "cookie", "cookie", "sec_b", {}}).value();
    const auto foreign = s.credentials->insert({other, "qq", "X", "cookie", "cookie", "sec_x", {}}).value();
    const auto netease = s.credentials->insert({owner, "netease", "N", "cookie", "cookie", "sec_n", {}}).value();

    CHECK(s.credentials->get_default(owner, "qq")->selection_version == 0);
    CHECK(s.credentials->set_default(owner, "qq", a.id)->selection_version == 1);
    CHECK(s.credentials->set_default(owner, "qq", a.id)->selection_version == 1);
    CHECK(s.credentials->set_default(owner, "qq", b.id)->selection_version == 2);
    CHECK(s.credentials->set_default(owner, "qq", a.id)->selection_version == 3); // A -> B -> A

    CHECK(s.credentials->set_default(owner, "qq", foreign.id).error().code == ErrorCode::not_found);
    CHECK(s.credentials->set_default(owner, "qq", netease.id).error().code ==
          ErrorCode::validation_failed);
    CHECK(s.credentials->get(other, a.id).error().code == ErrorCode::not_found);

    // Revoking the default clears it and advances both versions in one transaction.
    const auto revoked = s.credentials->revoke(owner, a.id);
    REQUIRE(revoked);
    CHECK(revoked->credential.status == ports::CredentialStatus::revoked);
    CHECK(revoked->credential.version == 2);
    CHECK(revoked->previous_secret_ref == "sec_a");
    const auto after = s.credentials->get_default(owner, "qq");
    CHECK_FALSE(after->credential_id.has_value());
    CHECK(after->selection_version == 4);
    CHECK_FALSE(s.credentials->revoke(owner, a.id)->previous_secret_ref.has_value());
    CHECK(s.credentials->set_default(owner, "qq", a.id).error().code == ErrorCode::validation_failed);

    CHECK(s.credentials->set_default(owner, "qq", b.id)->selection_version == 5);
    CHECK(s.credentials->clear_default(owner, "qq")->selection_version == 6);
    CHECK(s.credentials->clear_default(owner, "qq")->selection_version == 6);
    CHECK(s.credentials->clear_default(owner, "spotify")->selection_version == 0);

    // Defaults survive a restart.
    s.reopen();
    CHECK(s.credentials->get_default(owner, "qq")->selection_version == 6);
}

TEST_CASE("credential secrets are replaced conditionally", "[storage][credentials]") {
    Store s{"replace"};
    const auto owner = make_principal(s);
    const auto c = s.credentials->insert({owner, "qq", "A", "cookie", "cookie", "sec_1", {}}).value();

    const auto replaced = s.credentials->replace_secret(owner, c.id, 1, "sec_2", std::string{"新标签"});
    REQUIRE(replaced);
    CHECK(replaced->credential.version == 2);
    CHECK(replaced->credential.secret_ref == "sec_2");
    CHECK(replaced->credential.label == "新标签");
    CHECK(replaced->previous_secret_ref == "sec_1");

    CHECK(s.credentials->replace_secret(owner, c.id, 1, "sec_3", {}).error().code ==
          ErrorCode::version_conflict);
    REQUIRE(s.credentials->revoke(owner, c.id));
    CHECK(s.credentials->replace_secret(owner, c.id, 3, "sec_4", {}).error().code ==
          ErrorCode::validation_failed);
    CHECK(s.credentials->list(owner, std::string{"qq"})->size() == 1);
    CHECK(s.credentials->list(owner, std::string{"netease"})->empty());
}

// A49: a destination id keeps the root it was first bound to.
TEST_CASE("destination roots are immutable", "[storage][destinations]") {
    Store s{"destinations"};
    REQUIRE(s.destinations->bind("dst_default", "/music"));
    CHECK(s.destinations->bind("dst_default", "/music"));
    const auto changed = s.destinations->bind("dst_default", "/other");
    REQUIRE_FALSE(changed);
    CHECK(changed.error().code == ErrorCode::destination_root_changed);
    CHECK(s.destinations->get("dst_default")->canonical_root == "/music");
    CHECK_FALSE(*s.destinations->in_use("dst_default"));

    const auto owner = make_principal(s);
    REQUIRE(s.tasks->create(create_command(owner, "1", "key-1")));
    CHECK(*s.destinations->in_use("dst_default"));
}

// A01-A03: idempotent creation and active intent uniqueness.
TEST_CASE("task creation is idempotent and deduplicated", "[storage][tasks]") {
    Store s{"create", TaskRepositoryOptions{3, 10, std::chrono::hours{24}}};
    REQUIRE(s.destinations->bind("dst_default", "/music"));
    const auto owner = make_principal(s);
    const auto other = make_principal(s, "other");

    const auto first = s.tasks->create(create_command(owner, "1", "key-a"));
    REQUIRE(first);
    CHECK(first->task.status == TaskStatus::queued);
    CHECK(first->task.version == 1);
    CHECK_FALSE(first->reused);
    CHECK_FALSE(first->replayed);

    const auto replay = s.tasks->create(create_command(owner, "1", "key-a"));
    REQUIRE(replay);
    CHECK(replay->replayed);
    CHECK(replay->task.task_id == first->task.task_id);

    CHECK(s.tasks->create(create_command(owner, "1", "key-a", "other-hash")).error().code ==
          ErrorCode::idempotency_conflict);

    const auto reused = s.tasks->create(create_command(owner, "1", "key-b"));
    REQUIRE(reused);
    CHECK(reused->reused);
    CHECK(reused->task.task_id == first->task.task_id);

    auto forced = create_command(owner, "1", "key-c");
    forced.force = true;
    CHECK(s.tasks->create(forced).error().code == ErrorCode::active_duplicate);

    // A different preview requirement is a different intent (A35).
    auto preview = ports::CreateTaskCommand{intent_for(owner, "1", true), "key-d", "h", false};
    const auto distinct = s.tasks->create(preview);
    REQUIRE(distinct);
    CHECK(distinct->task.task_id != first->task.task_id);

    REQUIRE(s.tasks->create(create_command(owner, "2", "key-e")));
    CHECK(s.tasks->create(create_command(owner, "3", "key-f")).error().code ==
          ErrorCode::queue_limit_exceeded);

    CHECK(s.tasks->get(other, first->task.task_id).error().code == ErrorCode::not_found);
    const auto intent = s.tasks->get_intent(first->task.task_id);
    REQUIRE(intent);
    CHECK(domain::fingerprint(*intent) == first->task.fingerprint);

    // Committed tasks survive a restart.
    s.reopen();
    CHECK(s.tasks->get(owner, first->task.task_id)->status == TaskStatus::queued);
}

// A22: a late write from an older execution cannot overwrite newer state.
TEST_CASE("claims and runner transitions are fenced by execution and version", "[storage][tasks]") {
    Store s{"claim"};
    REQUIRE(s.destinations->bind("dst_default", "/music"));
    const auto owner = make_principal(s);
    auto task = running_task(s, owner, "1");
    CHECK(task.status == TaskStatus::resolving);
    CHECK(task.version == 2);
    REQUIRE(task.execution_id);
    CHECK(task.execution_id->rfind("exe_", 0) == 0);
    CHECK_FALSE(s.tasks->claim_next(s.clock->now())->has_value());

    CHECK(s.tasks->transition({task.task_id, "exe_wrong", task.version, TaskStatus::downloading, {}, {}, {}})
              .error().code == ErrorCode::version_conflict);
    CHECK(s.tasks->transition({task.task_id, *task.execution_id, task.version + 5, TaskStatus::downloading, {}, {}, {}})
              .error().code == ErrorCode::version_conflict);
    CHECK(s.tasks->transition({task.task_id, *task.execution_id, task.version, TaskStatus::completed, {}, {}, {}})
              .error().code == ErrorCode::invalid_transition);

    task = step(s, task, TaskStatus::downloading);
    const auto old_execution = *task.execution_id;

    // Wait for a retry in the future, then resume at the recorded stage.
    const auto due = s.clock->now() + std::chrono::seconds{30};
    auto waiting = s.tasks->transition({task.task_id, old_execution, task.version, TaskStatus::waiting_retry,
                                        domain::WaitReason::upstream_retry, due, TaskStatus::downloading});
    REQUIRE(waiting);
    CHECK(waiting->status == TaskStatus::waiting_retry);
    CHECK_FALSE(waiting->execution_id.has_value());
    CHECK_FALSE(s.tasks->claim_next(s.clock->now())->has_value());

    s.clock->advance(std::chrono::seconds{31});
    const auto resumed = s.tasks->claim_next(s.clock->now());
    REQUIRE(resumed->has_value());
    CHECK((*resumed)->status == TaskStatus::downloading);
    CHECK((*resumed)->execution_id != old_execution);
    CHECK_FALSE((*resumed)->wait_reason.has_value());

    CHECK(s.tasks->transition({task.task_id, old_execution, (*resumed)->version, TaskStatus::processing, {}, {}, {}})
              .error().code == ErrorCode::version_conflict);
}

// A retry row must stay claimable: a due time and a stage claim_next() can resume.
TEST_CASE("waiting_retry requires a due time and a resumable stage", "[storage][tasks]") {
    Store s{"retry"};
    REQUIRE(s.destinations->bind("dst_default", "/music"));
    const auto owner = make_principal(s);
    auto task = running_task(s, owner, "1");
    task = step(s, task, TaskStatus::downloading);
    const auto due = s.clock->now();

    CHECK(s.tasks->transition({task.task_id, *task.execution_id, task.version, TaskStatus::waiting_retry,
                               domain::WaitReason::upstream_retry, due, TaskStatus::processing})
              .error().code == ErrorCode::validation_failed);
    CHECK(s.tasks->transition({task.task_id, *task.execution_id, task.version, TaskStatus::waiting_retry,
                               domain::WaitReason::upstream_retry, std::nullopt, TaskStatus::downloading})
              .error().code == ErrorCode::validation_failed);
    CHECK(s.tasks->get(owner, task.task_id)->status == TaskStatus::downloading);

    // A valid retry does not block a later task.
    REQUIRE(s.tasks->transition({task.task_id, *task.execution_id, task.version, TaskStatus::waiting_retry,
                                 domain::WaitReason::upstream_retry, due, {}}));
    REQUIRE(s.tasks->create(create_command(owner, "2", "key-2")));
    REQUIRE(s.tasks->claim_next(s.clock->now())->has_value());
    REQUIRE(s.tasks->claim_next(s.clock->now())->has_value());
}

TEST_CASE("cancel requests follow the task status", "[storage][tasks]") {
    Store s{"cancel"};
    REQUIRE(s.destinations->bind("dst_default", "/music"));
    const auto owner = make_principal(s);

    REQUIRE(s.tasks->create(create_command(owner, "q", "key-q")));
    const auto queued_id = s.tasks->claim_next(s.clock->now()).value()->task_id;
    // queued -> canceled immediately happens before any claim; use a fresh task.
    const auto fresh = s.tasks->create(create_command(owner, "f", "key-f")).value().task;
    const auto canceled = s.tasks->request_cancel(owner, fresh.task_id);
    REQUIRE(canceled);
    CHECK(canceled->status == TaskStatus::canceled);

    // Running: canceling first, the runner then finishes the cancel.
    const auto canceling = s.tasks->request_cancel(owner, queued_id);
    REQUIRE(canceling);
    CHECK(canceling->status == TaskStatus::canceling);
    CHECK(s.tasks->request_cancel(owner, queued_id)->version == canceling->version);
    const auto done = step(s, *canceling, TaskStatus::canceled);
    CHECK(done.status == TaskStatus::canceled);
    CHECK(s.tasks->request_cancel(owner, queued_id)->status == TaskStatus::canceled);
}

// A12 and A20 building blocks: publication intent, reservation and completion.
TEST_CASE("publication reserves, completes and releases paths", "[storage][publication]") {
    Store s{"publish"};
    REQUIRE(s.destinations->bind("dst_default", "/music"));
    const auto owner = make_principal(s);

    auto task = running_task(s, owner, "1");
    task = step(s, task, TaskStatus::downloading);
    task = step(s, task, TaskStatus::processing);

    const ports::PublicationIntent intent{task.task_id, *task.execution_id, "dst_default",
                                          ".music-service-work/" + task.task_id + "/input.part",
                                          "歌手/Song.mp3", R"({"size":3})", "reserved"};
    const auto finalizing = s.publications->begin(intent, task.version);
    REQUIRE(finalizing);
    CHECK(finalizing->status == TaskStatus::finalizing);
    CHECK(s.publications->pending()->size() == 1);

    // Cancel loses against finalizing.
    CHECK(s.tasks->request_cancel(owner, task.task_id).error().code == ErrorCode::task_finalizing);

    // Another task cannot reserve the same path, even with different letter case.
    auto other = running_task(s, owner, "2");
    other = step(s, other, TaskStatus::downloading);
    other = step(s, other, TaskStatus::processing);
    auto clash = intent;
    clash.task_id = other.task_id;
    clash.execution_id = *other.execution_id;
    clash.reserved_path = "歌手/SONG.MP3";
    CHECK(s.publications->begin(clash, other.version).error().code == ErrorCode::conflict);
    CHECK(s.tasks->get(owner, other.task_id)->status == TaskStatus::processing);

    REQUIRE(s.publications->set_stage(task.task_id, *task.execution_id, "moved"));
    CHECK(s.publications->pending()->front().stage == "moved");

    // Survives a restart until completed.
    s.reopen();
    CHECK(s.publications->pending()->size() == 1);

    const ports::NewFileAsset asset{owner, "dst_default", "歌手/Song.mp3", 3, std::string{"mp3"},
                                    "\"etag\"", R"({"quality":"standard"})"};
    // The asset must match the task's owner and its own reservation.
    auto foreign = asset;
    foreign.owner_id = "prn_other";
    auto elsewhere = asset;
    elsewhere.relative_path = "歌手/Else.mp3";
    for (const auto& bad : {foreign, elsewhere}) {
        CHECK(s.publications->complete(task.task_id, *task.execution_id, finalizing->version, bad, "{}")
                  .error().code == ErrorCode::validation_failed);
    }
    CHECK(s.publications->pending()->size() == 1);
    const auto completed = s.publications->complete(task.task_id, *task.execution_id,
                                                     finalizing->version, asset, R"({"metadata_status":"complete"})");
    REQUIRE(completed);
    CHECK(completed->task.status == TaskStatus::completed);
    CHECK(completed->asset.id.rfind("fil_", 0) == 0);
    CHECK(completed->asset.created_by_task == task.task_id);
    CHECK(s.publications->pending()->empty());
    CHECK(s.tasks->request_cancel(owner, task.task_id).error().code == ErrorCode::task_terminal);

    // The path stays taken by the asset.
    CHECK(s.publications->begin(clash, other.version).error().code == ErrorCode::conflict);

    CHECK(s.publications->get_asset("prn_other", completed->asset.id).error().code == ErrorCode::not_found);
    const auto missing = s.publications->mark_missing(completed->asset.id);
    REQUIRE(missing);
    CHECK(missing->status == ports::FileAssetStatus::missing);
    CHECK(missing->version == 2);

    // A publication that fails releases its reservation.
    clash.reserved_path = "歌手/Other.mp3";
    const auto second = s.publications->begin(clash, other.version);
    REQUIRE(second);
    const auto failed = s.publications->abandon(other.task_id, *other.execution_id, second->version,
                                                TaskStatus::failed, R"({"code":"FINALIZATION_FAILED"})");
    REQUIRE(failed);
    CHECK(failed->status == TaskStatus::failed);
    CHECK(s.publications->pending()->empty());
}
