#include "infrastructure/storage/sqlite/task_repositories.hpp"

#include "infrastructure/storage/intent_codec.hpp"
#include "infrastructure/storage/sqlite/sql_support.hpp"

#include <nlohmann/json.hpp>

namespace music_service::infrastructure::storage::sqlite {

namespace ports = application::ports;
using detail::collect;
using detail::first;
using detail::from_millis;
using detail::not_found;
using detail::to_millis;
using domain::TaskStatus;

namespace {

constexpr std::string_view create_method = "POST";
constexpr std::string_view create_route = "/api/v1/downloads";

constexpr std::string_view task_columns =
    "id, owner_id, status, version, execution_id, attempt, wait_reason, next_retry_at, "
    "resume_stage, destination_id, fingerprint_schema, fingerprint_digest, fingerprint, "
    "created_at, updated_at";

constexpr std::string_view terminal_list = "('completed', 'failed', 'canceled')";

std::optional<domain::WaitReason> parse_wait(const std::optional<std::string>& text) {
    if (!text) {
        return std::nullopt;
    }
    return *text == "admission" ? domain::WaitReason::admission : domain::WaitReason::upstream_retry;
}

std::optional<TaskStatus> parse_status_opt(const std::optional<std::string>& text) {
    if (!text) {
        return std::nullopt;
    }
    auto status = domain::parse_task_status(*text);
    return status ? std::optional<TaskStatus>{*status} : std::nullopt;
}

ports::TaskRecord read_task(const Statement& s) {
    ports::TaskRecord record;
    record.task_id = s.text(0);
    record.owner_id = s.text(1);
    record.status = parse_status_opt(s.text(2)).value_or(TaskStatus::failed);
    record.version = static_cast<std::uint64_t>(s.integer(3));
    record.execution_id = s.optional_text(4);
    record.attempt = static_cast<std::uint32_t>(s.integer(5));
    record.wait_reason = parse_wait(s.optional_text(6));
    record.next_retry_at = from_millis(s.optional_integer(7));
    record.resume_stage = parse_status_opt(s.optional_text(8));
    record.destination_id = s.text(9);
    record.fingerprint.schema_version = static_cast<std::uint32_t>(s.integer(10));
    record.fingerprint.digest = s.text(11);
    record.fingerprint.canonical = s.text(12);
    record.created_at = from_millis(s.integer(13));
    record.updated_at = from_millis(s.integer(14));
    return record;
}

domain::Result<std::optional<ports::TaskRecord>>
load_task(Connection& db, const std::string& id, const std::optional<std::string>& owner = {}) {
    auto s = db.prepare("SELECT " + std::string{task_columns} +
                        " FROM download_tasks WHERE id = ? AND (? IS NULL OR owner_id = ?)");
    if (!s) {
        return std::move(s).error();
    }
    s->bind(1, id).bind(2, owner).bind(3, owner);
    return first<ports::TaskRecord>(*s, read_task);
}

domain::Result<ports::TaskRecord> require_task(Connection& db, const std::string& id,
                                               const std::optional<std::string>& owner = {}) {
    auto found = load_task(db, id, owner);
    if (!found) {
        return std::move(found).error();
    }
    if (!found.value()) {
        return not_found("task");
    }
    return *found.value();
}

domain::Error version_conflict() {
    return domain::Error{domain::ErrorCode::version_conflict,
                         "task changed or belongs to another execution", true, {}};
}

// A write from a runner: the task must still be at the version it read and owned by
// the same execution generation.
domain::Result<ports::TaskRecord> require_runner(Connection& db, const std::string& id,
                                                 const std::string& execution_id,
                                                 std::uint64_t expected_version) {
    auto task = require_task(db, id);
    if (!task) {
        return task;
    }
    if (task->execution_id != execution_id || task->version != expected_version) {
        return version_conflict();
    }
    return task;
}

struct Update {
    TaskStatus to{TaskStatus::failed};
    std::optional<std::string> execution_id;
    std::optional<domain::WaitReason> wait_reason;
    std::optional<std::int64_t> next_retry_at;
    std::optional<TaskStatus> resume_stage;
    bool started{false};
    bool cancel_requested{false};
    std::optional<std::string> result_json;
    std::optional<std::string> error_json;
};

domain::Result<ports::TaskRecord> apply(Connection& db, const std::string& id, const Update& u,
                                        std::int64_t now) {
    auto s = db.prepare(
        "UPDATE download_tasks SET status = ?, version = version + 1, execution_id = ?,"
        " wait_reason = ?, next_retry_at = ?, resume_stage = ?, updated_at = ?,"
        " started_at = COALESCE(started_at, ?), finished_at = ?,"
        " cancel_requested_at = COALESCE(cancel_requested_at, ?),"
        " result = COALESCE(?, result), error = COALESCE(?, error) WHERE id = ?");
    if (!s) {
        return std::move(s).error();
    }
    const auto terminal = domain::is_terminal(u.to);
    s->bind(1, domain::to_string(u.to))
        .bind(2, u.execution_id)
        .bind(3, u.wait_reason ? std::optional<std::string>{std::string{domain::to_string(*u.wait_reason)}}
                               : std::nullopt)
        .bind(4, u.next_retry_at)
        .bind(5, u.resume_stage ? std::optional<std::string>{std::string{domain::to_string(*u.resume_stage)}}
                                : std::nullopt)
        .bind(6, now)
        .bind(7, u.started ? std::optional<std::int64_t>{now} : std::nullopt)
        .bind(8, terminal ? std::optional<std::int64_t>{now} : std::nullopt)
        .bind(9, u.cancel_requested ? std::optional<std::int64_t>{now} : std::nullopt)
        .bind(10, u.result_json)
        .bind(11, u.error_json)
        .bind(12, id);
    if (auto done = s->run(); !done) {
        return done.error();
    }
    return require_task(db, id);
}

std::string path_key(std::string_view relative_path) {
    // Conservative uniqueness: Windows and many NAS shares compare names without case.
    std::string key{relative_path};
    for (auto& c : key) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        } else if (c == '\\') {
            c = '/';
        }
    }
    return key;
}

domain::Result<std::int64_t> count(Connection& db, std::string_view sql,
                                   const std::optional<std::string>& owner) {
    auto s = db.prepare(sql);
    if (!s) {
        return std::move(s).error();
    }
    if (owner) {
        s->bind(1, *owner);
    }
    auto row = s->step();
    if (!row) {
        return std::move(row).error();
    }
    return s->integer(0);
}

constexpr std::string_view asset_columns =
    "id, owner_id, destination_id, relative_path, size_bytes, container, etag, status, "
    "properties, version, created_by_task, created_at";

ports::FileAssetRecord read_asset(const Statement& s) {
    return ports::FileAssetRecord{s.text(0),
                                  s.text(1),
                                  s.text(2),
                                  s.text(3),
                                  static_cast<std::uint64_t>(s.integer(4)),
                                  s.optional_text(5),
                                  s.text(6),
                                  s.text(7) == "missing" ? ports::FileAssetStatus::missing
                                                         : ports::FileAssetStatus::available,
                                  s.text(8),
                                  static_cast<std::uint64_t>(s.integer(9)),
                                  s.optional_text(10),
                                  from_millis(s.integer(11))};
}

domain::Result<std::optional<ports::FileAssetRecord>>
load_asset(Connection& db, const std::string& id, const std::optional<std::string>& owner = {}) {
    auto s = db.prepare("SELECT " + std::string{asset_columns} +
                        " FROM file_assets WHERE id = ? AND (? IS NULL OR owner_id = ?)");
    if (!s) {
        return std::move(s).error();
    }
    s->bind(1, id).bind(2, owner).bind(3, owner);
    return first<ports::FileAssetRecord>(*s, read_asset);
}

} // namespace

// ===================================================================== tasks

domain::Result<ports::CreateTaskOutcome>
SqliteTaskRepository::create(const ports::CreateTaskCommand& command) {
    const auto& intent = command.intent;
    const auto& owner = intent.owner_id();
    if (command.idempotency_key.empty() || command.request_hash.empty()) {
        return domain::Error::validation("Idempotency-Key", "is required");
    }
    const auto fingerprint = domain::fingerprint(intent);
    const auto encoded = encode_intent(intent);
    const auto new_id = ctx_.ids->next("tsk_");
    const auto now = to_millis(ctx_.clock->now());
    const auto expires =
        now + std::chrono::duration_cast<std::chrono::milliseconds>(options_.idempotency_retention)
                  .count();

    return ctx_.database->write([&](Connection& db) -> domain::Result<ports::CreateTaskOutcome> {
        // 1. Idempotency: a committed key replays its first outcome.
        auto purge = db.prepare("DELETE FROM idempotency_records WHERE owner_id = ? AND method = ?"
                                " AND route = ? AND idempotency_key = ? AND expires_at <= ?");
        if (!purge) {
            return std::move(purge).error();
        }
        purge->bind(1, owner).bind(2, create_method).bind(3, create_route).bind(
            4, command.idempotency_key).bind(5, now);
        if (auto done = purge->run(); !done) {
            return done.error();
        }
        auto lookup = db.prepare("SELECT request_hash, response FROM idempotency_records"
                                 " WHERE owner_id = ? AND method = ? AND route = ?"
                                 " AND idempotency_key = ?");
        if (!lookup) {
            return std::move(lookup).error();
        }
        lookup->bind(1, owner).bind(2, create_method).bind(3, create_route).bind(
            4, command.idempotency_key);
        auto record = first<std::pair<std::string, std::string>>(*lookup, [](const Statement& st) {
            return std::pair{st.text(0), st.text(1)};
        });
        if (!record) {
            return std::move(record).error();
        }
        if (record.value()) {
            if (record.value()->first != command.request_hash) {
                return domain::Error{domain::ErrorCode::idempotency_conflict,
                                     "Idempotency-Key was used for a different request", false, {}};
            }
            const auto response = nlohmann::json::parse(record.value()->second, nullptr, false);
            if (response.is_discarded() || !response.contains("task_id")) {
                return domain::Error{domain::ErrorCode::internal, "stored response is invalid",
                                     false, {}};
            }
            auto task = require_task(db, response["task_id"].get<std::string>(), owner);
            if (!task) {
                return std::move(task).error();
            }
            return ports::CreateTaskOutcome{std::move(task).value(),
                                            response.value("reused", false), true};
        }

        const auto remember = [&](const std::string& task_id, bool reused) -> domain::Result<void> {
            auto insert = db.prepare(
                "INSERT INTO idempotency_records (owner_id, method, route, idempotency_key,"
                " request_hash, status_code, response, created_at, expires_at)"
                " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
            if (!insert) {
                return std::move(insert).error();
            }
            const nlohmann::json response{{"task_id", task_id}, {"reused", reused}};
            insert->bind(1, owner).bind(2, create_method).bind(3, create_route).bind(
                4, command.idempotency_key).bind(5, command.request_hash).bind(
                6, std::int64_t{reused ? 200 : 202}).bind(7, response.dump()).bind(8, now).bind(
                9, expires);
            return insert->run();
        };

        // 2. An equal active intent is reused (or rejected under force).
        auto active = db.prepare("SELECT id FROM download_tasks WHERE owner_id = ?"
                                 " AND fingerprint_digest = ? AND fingerprint = ?"
                                 " AND status NOT IN " +
                                 std::string{terminal_list});
        if (!active) {
            return std::move(active).error();
        }
        active->bind(1, owner).bind(2, fingerprint.digest).bind(3, fingerprint.canonical);
        auto existing = first<std::string>(*active, [](const Statement& st) { return st.text(0); });
        if (!existing) {
            return std::move(existing).error();
        }
        if (existing.value()) {
            if (command.force) {
                return domain::Error{domain::ErrorCode::active_duplicate,
                                     "an equal download is already active", false, {}};
            }
            if (auto remembered = remember(*existing.value(), true); !remembered) {
                return remembered.error();
            }
            auto task = require_task(db, *existing.value());
            if (!task) {
                return std::move(task).error();
            }
            return ports::CreateTaskOutcome{std::move(task).value(), true, false};
        }

        // 3. Active task limits.
        const auto active_filter = " status NOT IN " + std::string{terminal_list};
        auto mine = count(db, "SELECT COUNT(*) FROM download_tasks WHERE owner_id = ? AND" + active_filter, owner);
        auto all = count(db, "SELECT COUNT(*) FROM download_tasks WHERE" + active_filter, std::nullopt);
        if (!mine || !all) {
            return !mine ? std::move(mine).error() : std::move(all).error();
        }
        if (*mine >= static_cast<std::int64_t>(options_.per_principal_active) ||
            *all >= static_cast<std::int64_t>(options_.global_active)) {
            return domain::Error{domain::ErrorCode::queue_limit_exceeded,
                                 "too many unfinished downloads", true, {}};
        }

        // 4. Insert.
        auto insert = db.prepare(
            "INSERT INTO download_tasks (id, owner_id, status, version, fingerprint_schema,"
            " fingerprint_digest, fingerprint, normalized_intent, destination_id, original_source,"
            " created_at, updated_at) VALUES (?, ?, 'queued', 1, ?, ?, ?, ?, ?, ?, ?, ?)");
        if (!insert) {
            return std::move(insert).error();
        }
        insert->bind(1, new_id)
            .bind(2, owner)
            .bind(3, static_cast<std::int64_t>(fingerprint.schema_version))
            .bind(4, fingerprint.digest)
            .bind(5, fingerprint.canonical)
            .bind(6, encoded)
            .bind(7, intent.product().destination_id)
            .bind(8, intent.original_track().source.value())
            .bind(9, now)
            .bind(10, now);
        if (auto done = insert->run(); !done) {
            return done.error();
        }
        if (auto remembered = remember(new_id, false); !remembered) {
            return remembered.error();
        }
        auto task = require_task(db, new_id);
        if (!task) {
            return std::move(task).error();
        }
        return ports::CreateTaskOutcome{std::move(task).value(), false, false};
    });
}

domain::Result<ports::TaskRecord> SqliteTaskRepository::get(const std::string& owner_id,
                                                            const std::string& task_id) {
    return ctx_.database->read(
        [&](Connection& db) { return require_task(db, task_id, owner_id); });
}

domain::Result<domain::NormalizedDownloadIntent>
SqliteTaskRepository::get_intent(const std::string& task_id) {
    return ctx_.database->read([&](Connection& db) -> domain::Result<domain::NormalizedDownloadIntent> {
        auto s = db.prepare("SELECT normalized_intent FROM download_tasks WHERE id = ?");
        if (!s) {
            return std::move(s).error();
        }
        s->bind(1, task_id);
        auto text = first<std::string>(*s, [](const Statement& st) { return st.text(0); });
        if (!text) {
            return std::move(text).error();
        }
        if (!text.value()) {
            return not_found("task");
        }
        return decode_intent(*text.value());
    });
}

domain::Result<std::optional<ports::TaskRecord>>
SqliteTaskRepository::claim_next(std::chrono::system_clock::time_point now) {
    const auto now_ms = to_millis(now);
    const auto execution_id = ctx_.ids->next("exe_");
    return ctx_.database->write([&](Connection& db) -> domain::Result<std::optional<ports::TaskRecord>> {
        auto s = db.prepare("SELECT " + std::string{task_columns} +
                            " FROM download_tasks WHERE status = 'queued'"
                            " OR (status = 'waiting_retry' AND next_retry_at <= ?)"
                            " ORDER BY COALESCE(next_retry_at, created_at), created_at, id LIMIT 1");
        if (!s) {
            return std::move(s).error();
        }
        s->bind(1, now_ms);
        auto candidate = first<ports::TaskRecord>(*s, read_task);
        if (!candidate) {
            return std::move(candidate).error();
        }
        if (!candidate.value()) {
            return std::optional<ports::TaskRecord>{};
        }
        const auto& task = *candidate.value();
        const auto to = task.status == TaskStatus::waiting_retry
                            ? task.resume_stage.value_or(TaskStatus::resolving)
                            : TaskStatus::resolving;
        if (auto legal = domain::check_transition(task.status, to); !legal) {
            return legal.error();
        }
        Update update;
        update.to = to;
        update.execution_id = execution_id;
        update.started = true;
        auto claimed = apply(db, task.task_id, update, now_ms);
        if (!claimed) {
            return std::move(claimed).error();
        }
        return std::optional<ports::TaskRecord>{std::move(claimed).value()};
    });
}

domain::Result<ports::TaskRecord>
SqliteTaskRepository::transition(const ports::TransitionCommand& command) {
    const auto now = to_millis(ctx_.clock->now());
    return ctx_.database->write([&](Connection& db) -> domain::Result<ports::TaskRecord> {
        auto task = require_runner(db, command.task_id, command.execution_id,
                                   command.expected_version);
        if (!task) {
            return task;
        }
        if (auto legal = domain::check_transition(task->status, command.to); !legal) {
            return legal.error();
        }
        Update update;
        update.to = command.to;
        if (command.to == TaskStatus::waiting_retry) {
            if (!command.wait_reason) {
                return domain::Error::validation("wait_reason", "is required for waiting_retry");
            }
            // A row claim_next() cannot claim (no due time) or cannot resume (illegal
            // stage) would sit forever or block the head of the queue.
            if (!command.next_retry_at) {
                return domain::Error::validation("next_retry_at", "is required for waiting_retry");
            }
            const auto resume = command.resume_stage.value_or(TaskStatus::resolving);
            if (resume != TaskStatus::resolving && resume != TaskStatus::probing &&
                resume != TaskStatus::downloading) {
                return domain::Error::validation("resume_stage",
                                                 "must be resolving, probing or downloading");
            }
            update.wait_reason = command.wait_reason;
            update.next_retry_at = detail::to_millis(*command.next_retry_at);
            update.resume_stage = resume;
        }
        // Waiting or finished tasks belong to no execution; a later claim issues a new one.
        if (command.to != TaskStatus::waiting_retry && !domain::is_terminal(command.to)) {
            update.execution_id = command.execution_id;
        }
        return apply(db, command.task_id, update, now);
    });
}

domain::Result<ports::TaskRecord> SqliteTaskRepository::request_cancel(const std::string& owner_id,
                                                                       const std::string& task_id) {
    const auto now = to_millis(ctx_.clock->now());
    return ctx_.database->write([&](Connection& db) -> domain::Result<ports::TaskRecord> {
        auto task = require_task(db, task_id, owner_id);
        if (!task) {
            return task;
        }
        auto action = domain::decide_cancel(task->status);
        if (!action) {
            return std::move(action).error();
        }
        Update update;
        update.cancel_requested = true;
        switch (*action) {
        case domain::CancelAction::already:
            return task;
        case domain::CancelAction::cancel_now:
            update.to = TaskStatus::canceled;
            break;
        case domain::CancelAction::enter_canceling:
            update.to = TaskStatus::canceling;
            update.execution_id = task->execution_id; // the runner finishes the cancel
            break;
        }
        return apply(db, task_id, update, now);
    });
}

// =============================================================== publication

domain::Result<ports::TaskRecord> SqlitePublicationRepository::begin(const ports::PublicationIntent& intent,
                                                                     std::uint64_t expected_version) {
    const auto now = to_millis(ctx_.clock->now());
    const auto key = path_key(intent.reserved_path);
    return ctx_.database->write([&](Connection& db) -> domain::Result<ports::TaskRecord> {
        auto task = require_runner(db, intent.task_id, intent.execution_id, expected_version);
        if (!task) {
            return task;
        }
        if (auto legal = domain::check_transition(task->status, TaskStatus::finalizing); !legal) {
            return legal.error();
        }
        if (task->destination_id != intent.destination_id) {
            return domain::Error::validation("destination_id", "differs from the task's destination");
        }
        auto taken = db.prepare("SELECT EXISTS (SELECT 1 FROM file_assets WHERE destination_id = ?"
                                " AND path_key = ?) OR EXISTS (SELECT 1 FROM finalization_intents"
                                " WHERE destination_id = ? AND path_key = ?)");
        if (!taken) {
            return std::move(taken).error();
        }
        taken->bind(1, intent.destination_id).bind(2, key).bind(3, intent.destination_id).bind(4, key);
        auto row = taken->step();
        if (!row) {
            return std::move(row).error();
        }
        if (taken->integer(0) != 0) {
            return domain::Error{domain::ErrorCode::conflict, "target path is already reserved",
                                 true, {}};
        }
        auto insert = db.prepare(
            "INSERT INTO finalization_intents (task_id, execution_id, destination_id,"
            " workspace_file, reserved_path, path_key, evidence, stage, created_at, updated_at)"
            " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
        if (!insert) {
            return std::move(insert).error();
        }
        insert->bind(1, intent.task_id)
            .bind(2, intent.execution_id)
            .bind(3, intent.destination_id)
            .bind(4, intent.workspace_file)
            .bind(5, intent.reserved_path)
            .bind(6, key)
            .bind(7, intent.evidence_json)
            .bind(8, intent.stage)
            .bind(9, now)
            .bind(10, now);
        if (auto done = insert->run(); !done) {
            return done.error();
        }
        Update update;
        update.to = TaskStatus::finalizing;
        update.execution_id = intent.execution_id;
        return apply(db, intent.task_id, update, now);
    });
}

domain::Result<void> SqlitePublicationRepository::set_stage(const std::string& task_id,
                                                            const std::string& execution_id,
                                                            const std::string& stage) {
    const auto now = to_millis(ctx_.clock->now());
    return ctx_.database->write([&](Connection& db) -> domain::Result<void> {
        auto s = db.prepare("UPDATE finalization_intents SET stage = ?, updated_at = ?"
                            " WHERE task_id = ? AND execution_id = ?");
        if (!s) {
            return std::move(s).error();
        }
        s->bind(1, stage).bind(2, now).bind(3, task_id).bind(4, execution_id);
        if (auto done = s->run(); !done) {
            return done;
        }
        if (db.changes() == 0) {
            return version_conflict();
        }
        return domain::ok();
    });
}

domain::Result<ports::CompletedPublication>
SqlitePublicationRepository::complete(const std::string& task_id, const std::string& execution_id,
                                      std::uint64_t expected_version,
                                      const ports::NewFileAsset& asset,
                                      const std::string& result_json) {
    const auto now = to_millis(ctx_.clock->now());
    const auto file_id = ctx_.ids->next("fil_");
    return ctx_.database->write([&](Connection& db) -> domain::Result<ports::CompletedPublication> {
        auto task = require_runner(db, task_id, execution_id, expected_version);
        if (!task) {
            return std::move(task).error();
        }
        if (auto legal = domain::check_transition(task->status, TaskStatus::completed); !legal) {
            return legal.error();
        }
        auto reservation = db.prepare("SELECT destination_id, reserved_path FROM finalization_intents"
                                      " WHERE task_id = ? AND execution_id = ?");
        if (!reservation) {
            return std::move(reservation).error();
        }
        reservation->bind(1, task_id).bind(2, execution_id);
        auto reserved = first<std::pair<std::string, std::string>>(*reservation, [](const Statement& st) {
            return std::pair{st.text(0), st.text(1)};
        });
        if (!reserved) {
            return std::move(reserved).error();
        }
        if (!reserved.value()) {
            return domain::Error{domain::ErrorCode::conflict,
                                 "no publication intent for this execution", false, {}};
        }
        // The asset is exactly what this task reserved: its owner, destination and path.
        const auto& [reserved_destination, reserved_path] = *reserved.value();
        if (asset.owner_id != task->owner_id) {
            return domain::Error::validation("owner_id", "differs from the task's owner");
        }
        if (asset.destination_id != reserved_destination) {
            return domain::Error::validation("destination_id", "differs from the reserved destination");
        }
        if (asset.relative_path != reserved_path) {
            return domain::Error::validation("relative_path", "differs from the reserved path");
        }
        auto remove_intent = db.prepare("DELETE FROM finalization_intents WHERE task_id = ?"
                                        " AND execution_id = ?");
        if (!remove_intent) {
            return std::move(remove_intent).error();
        }
        remove_intent->bind(1, task_id).bind(2, execution_id);
        if (auto done = remove_intent->run(); !done) {
            return done.error();
        }
        auto insert = db.prepare(
            "INSERT INTO file_assets (id, owner_id, destination_id, relative_path, path_key,"
            " size_bytes, container, etag, status, properties, version, created_by_task, created_at)"
            " VALUES (?, ?, ?, ?, ?, ?, ?, ?, 'available', ?, 1, ?, ?)");
        if (!insert) {
            return std::move(insert).error();
        }
        insert->bind(1, file_id)
            .bind(2, task->owner_id)
            .bind(3, reserved_destination)
            .bind(4, reserved_path)
            .bind(5, path_key(reserved_path))
            .bind(6, static_cast<std::int64_t>(asset.size_bytes))
            .bind(7, asset.container)
            .bind(8, asset.etag)
            .bind(9, asset.properties_json)
            .bind(10, task_id)
            .bind(11, now);
        if (auto done = insert->run(); !done) {
            return done.error();
        }
        auto relation = db.prepare("INSERT INTO task_files (task_id, file_id, relation)"
                                   " VALUES (?, ?, 'created')");
        if (!relation) {
            return std::move(relation).error();
        }
        relation->bind(1, task_id).bind(2, file_id);
        if (auto done = relation->run(); !done) {
            return done.error();
        }
        Update update;
        update.to = TaskStatus::completed;
        update.result_json = result_json;
        auto completed = apply(db, task_id, update, now);
        if (!completed) {
            return std::move(completed).error();
        }
        auto stored = load_asset(db, file_id);
        if (!stored) {
            return std::move(stored).error();
        }
        return ports::CompletedPublication{std::move(completed).value(), *stored.value()};
    });
}

domain::Result<ports::TaskRecord>
SqlitePublicationRepository::abandon(const std::string& task_id, const std::string& execution_id,
                                     std::uint64_t expected_version, TaskStatus to,
                                     const std::string& error_json) {
    if (to != TaskStatus::failed && to != TaskStatus::waiting_retry) {
        return domain::Error::validation("to", "must be failed or waiting_retry");
    }
    const auto now = to_millis(ctx_.clock->now());
    return ctx_.database->write([&](Connection& db) -> domain::Result<ports::TaskRecord> {
        auto task = require_runner(db, task_id, execution_id, expected_version);
        if (!task) {
            return task;
        }
        if (auto legal = domain::check_transition(task->status, to); !legal) {
            return legal.error();
        }
        auto remove_intent = db.prepare("DELETE FROM finalization_intents WHERE task_id = ?");
        if (!remove_intent) {
            return std::move(remove_intent).error();
        }
        remove_intent->bind(1, task_id);
        if (auto done = remove_intent->run(); !done) {
            return done.error();
        }
        Update update;
        update.to = to;
        update.error_json = error_json;
        if (to == TaskStatus::waiting_retry) {
            update.wait_reason = domain::WaitReason::upstream_retry;
            update.next_retry_at = now;
            update.resume_stage = TaskStatus::resolving;
        }
        return apply(db, task_id, update, now);
    });
}

domain::Result<std::vector<ports::PublicationIntent>> SqlitePublicationRepository::pending() {
    return ctx_.database->read([&](Connection& db) -> domain::Result<std::vector<ports::PublicationIntent>> {
        auto s = db.prepare("SELECT task_id, execution_id, destination_id, workspace_file,"
                            " reserved_path, evidence, stage FROM finalization_intents"
                            " ORDER BY created_at, task_id");
        if (!s) {
            return std::move(s).error();
        }
        return collect<ports::PublicationIntent>(*s, [](const Statement& st) {
            return ports::PublicationIntent{st.text(0), st.text(1), st.text(2), st.text(3),
                                            st.text(4), st.text(5), st.text(6)};
        });
    });
}

domain::Result<ports::FileAssetRecord> SqlitePublicationRepository::get_asset(const std::string& owner_id,
                                                                              const std::string& file_id) {
    return ctx_.database->read([&](Connection& db) -> domain::Result<ports::FileAssetRecord> {
        auto found = load_asset(db, file_id, owner_id);
        if (!found) {
            return std::move(found).error();
        }
        if (!found.value()) {
            return not_found("file");
        }
        return *found.value();
    });
}

domain::Result<ports::FileAssetRecord> SqlitePublicationRepository::mark_missing(const std::string& file_id) {
    return ctx_.database->write([&](Connection& db) -> domain::Result<ports::FileAssetRecord> {
        auto s = db.prepare("UPDATE file_assets SET status = 'missing', version = version + 1"
                            " WHERE id = ? AND status <> 'missing'");
        if (!s) {
            return std::move(s).error();
        }
        s->bind(1, file_id);
        if (auto done = s->run(); !done) {
            return done.error();
        }
        auto found = load_asset(db, file_id);
        if (!found) {
            return std::move(found).error();
        }
        if (!found.value()) {
            return not_found("file");
        }
        return *found.value();
    });
}

} // namespace music_service::infrastructure::storage::sqlite
