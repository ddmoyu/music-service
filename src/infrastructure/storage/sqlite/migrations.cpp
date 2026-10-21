#include "infrastructure/storage/sqlite/migrations.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <string>

namespace music_service::infrastructure::storage::sqlite {

namespace {

// Version 1: everything S1 needs. Times are UTC milliseconds since the Unix epoch.
constexpr std::string_view schema_v1 = R"sql(
CREATE TABLE principals (
    id TEXT PRIMARY KEY,
    display_name TEXT NOT NULL,
    role TEXT NOT NULL CHECK (role IN ('user', 'admin')),
    enabled INTEGER NOT NULL CHECK (enabled IN (0, 1)),
    created_at INTEGER NOT NULL
) STRICT;

CREATE TABLE access_tokens (
    id TEXT PRIMARY KEY,
    principal_id TEXT NOT NULL REFERENCES principals (id),
    token_hash TEXT NOT NULL UNIQUE,
    scopes TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    revoked_at INTEGER
) STRICT;
CREATE INDEX access_tokens_principal ON access_tokens (principal_id);

CREATE TABLE credentials (
    id TEXT PRIMARY KEY,
    owner_id TEXT NOT NULL REFERENCES principals (id),
    source TEXT NOT NULL,
    label TEXT NOT NULL,
    kind TEXT NOT NULL,
    auth_profile TEXT NOT NULL,
    secret_ref TEXT,
    version INTEGER NOT NULL,
    status TEXT NOT NULL CHECK (status IN ('active', 'invalid', 'revoked')),
    scope_summary TEXT,
    expires_at INTEGER,
    last_verified_at INTEGER,
    last_verify_result TEXT,
    created_at INTEGER NOT NULL,
    revoked_at INTEGER
) STRICT;
CREATE INDEX credentials_owner_source ON credentials (owner_id, source);

CREATE TABLE credential_defaults (
    owner_id TEXT NOT NULL REFERENCES principals (id),
    source TEXT NOT NULL,
    credential_id TEXT REFERENCES credentials (id),
    selection_version INTEGER NOT NULL,
    PRIMARY KEY (owner_id, source)
) STRICT;

CREATE TABLE destinations (
    id TEXT PRIMARY KEY,
    canonical_root TEXT NOT NULL,
    registered_at INTEGER NOT NULL
) STRICT;

CREATE TABLE download_tasks (
    id TEXT PRIMARY KEY,
    owner_id TEXT NOT NULL REFERENCES principals (id),
    status TEXT NOT NULL,
    version INTEGER NOT NULL,
    execution_id TEXT,
    attempt INTEGER NOT NULL DEFAULT 0,
    attempt_open INTEGER NOT NULL DEFAULT 0,
    fingerprint_schema INTEGER NOT NULL,
    fingerprint_digest TEXT NOT NULL,
    fingerprint TEXT NOT NULL,
    normalized_intent TEXT NOT NULL,
    destination_id TEXT NOT NULL REFERENCES destinations (id),
    original_source TEXT NOT NULL,
    retry_of TEXT REFERENCES download_tasks (id),
    wait_reason TEXT CHECK (wait_reason IN ('admission', 'upstream_retry')),
    resume_stage TEXT,
    next_retry_at INTEGER,
    deadline_at INTEGER,
    progress TEXT,
    result TEXT,
    error TEXT,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    started_at INTEGER,
    finished_at INTEGER,
    cancel_requested_at INTEGER
) STRICT;
CREATE INDEX download_tasks_owner_list ON download_tasks (owner_id, status, created_at, id);
CREATE INDEX download_tasks_due ON download_tasks (status, next_retry_at);
-- Same intent may exist many times historically, but only once while active.
CREATE UNIQUE INDEX download_tasks_active_fingerprint
    ON download_tasks (owner_id, fingerprint_digest, fingerprint)
    WHERE status NOT IN ('completed', 'failed', 'canceled');

CREATE TABLE download_batches (
    id TEXT PRIMARY KEY,
    owner_id TEXT NOT NULL REFERENCES principals (id),
    created_count INTEGER NOT NULL,
    reused_count INTEGER NOT NULL,
    created_at INTEGER NOT NULL
) STRICT;

CREATE TABLE batch_items (
    batch_id TEXT NOT NULL REFERENCES download_batches (id),
    ordinal INTEGER NOT NULL,
    task_id TEXT NOT NULL REFERENCES download_tasks (id),
    PRIMARY KEY (batch_id, ordinal)
) STRICT;
CREATE INDEX batch_items_task ON batch_items (task_id);

CREATE TABLE task_attempts (
    task_id TEXT NOT NULL REFERENCES download_tasks (id),
    attempt_no INTEGER NOT NULL,
    last_execution_id TEXT NOT NULL,
    started_at INTEGER NOT NULL,
    finished_at INTEGER,
    error TEXT,
    evidence TEXT,
    PRIMARY KEY (task_id, attempt_no)
) STRICT;

CREATE TABLE file_assets (
    id TEXT PRIMARY KEY,
    owner_id TEXT NOT NULL REFERENCES principals (id),
    destination_id TEXT NOT NULL REFERENCES destinations (id),
    relative_path TEXT NOT NULL,
    path_key TEXT NOT NULL,
    size_bytes INTEGER NOT NULL,
    container TEXT,
    etag TEXT NOT NULL,
    status TEXT NOT NULL CHECK (status IN ('available', 'missing')),
    properties TEXT NOT NULL,
    version INTEGER NOT NULL,
    created_by_task TEXT REFERENCES download_tasks (id),
    created_at INTEGER NOT NULL,
    UNIQUE (destination_id, path_key)
) STRICT;
CREATE INDEX file_assets_owner ON file_assets (owner_id, created_at);

CREATE TABLE task_files (
    task_id TEXT NOT NULL REFERENCES download_tasks (id),
    file_id TEXT NOT NULL REFERENCES file_assets (id),
    relation TEXT NOT NULL,
    PRIMARY KEY (task_id, file_id)
) STRICT;

CREATE TABLE idempotency_records (
    owner_id TEXT NOT NULL REFERENCES principals (id),
    method TEXT NOT NULL,
    route TEXT NOT NULL,
    idempotency_key TEXT NOT NULL,
    request_hash TEXT NOT NULL,
    status_code INTEGER NOT NULL,
    response TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    expires_at INTEGER NOT NULL,
    PRIMARY KEY (owner_id, method, route, idempotency_key)
) STRICT;
CREATE INDEX idempotency_records_expiry ON idempotency_records (expires_at);

CREATE TABLE finalization_intents (
    task_id TEXT PRIMARY KEY REFERENCES download_tasks (id),
    execution_id TEXT NOT NULL,
    destination_id TEXT NOT NULL REFERENCES destinations (id),
    workspace_file TEXT NOT NULL,
    reserved_path TEXT NOT NULL,
    path_key TEXT NOT NULL,
    evidence TEXT NOT NULL,
    stage TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    UNIQUE (destination_id, path_key)
) STRICT;
)sql";

// Version 2 (T17): what the runner keeps between executions. execution holds the public
// facts of the run (actual track and quality), checkpoint the private resume evidence of
// the work file, attempt_used_ms the running time of the open attempt without its
// admission pauses.
constexpr std::string_view schema_v2 = R"sql(
ALTER TABLE download_tasks ADD COLUMN execution TEXT;
ALTER TABLE download_tasks ADD COLUMN checkpoint TEXT;
ALTER TABLE download_tasks ADD COLUMN attempt_used_ms INTEGER NOT NULL DEFAULT 0;
)sql";

constexpr std::array<Migration, 2> migrations{{{1, schema_v1}, {2, schema_v2}}};

std::string checksum(std::string_view text) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    static constexpr char digits[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = digits[hash & 0xFU];
        hash >>= 4U;
    }
    return out;
}

domain::Error schema_error(std::string message) {
    return domain::Error{domain::ErrorCode::storage_unavailable, std::move(message), false, {}};
}

} // namespace

std::span<const Migration> builtin_migrations() { return migrations; }

domain::Result<std::uint32_t> migrate(Database& database, std::chrono::system_clock::time_point now,
                                      std::span<const Migration> list) {
    const auto applied_at = std::chrono::duration_cast<std::chrono::milliseconds>(
                                now.time_since_epoch())
                                .count();

    auto applied = database.write([&](Connection& db) -> domain::Result<std::map<std::uint32_t, std::string>> {
        if (auto created = db.exec("CREATE TABLE IF NOT EXISTS schema_migrations ("
                                   " version INTEGER PRIMARY KEY,"
                                   " checksum TEXT NOT NULL,"
                                   " applied_at INTEGER NOT NULL) STRICT;");
            !created) {
            return created.error();
        }
        auto statement = db.prepare("SELECT version, checksum FROM schema_migrations");
        if (!statement) {
            return std::move(statement).error();
        }
        std::map<std::uint32_t, std::string> rows;
        while (true) {
            auto row = statement->step();
            if (!row) {
                return std::move(row).error();
            }
            if (!*row) {
                break;
            }
            rows.emplace(static_cast<std::uint32_t>(statement->integer(0)), statement->text(1));
        }
        return rows;
    });
    if (!applied) {
        return std::move(applied).error();
    }

    const auto latest = list.empty() ? 0U : list.back().version;
    for (const auto& [version, recorded] : applied.value()) {
        if (version > latest) {
            return schema_error("database schema version " + std::to_string(version) +
                                " is newer than this build supports (" + std::to_string(latest) +
                                "); upgrade the program or restore a matching backup");
        }
        const auto known = std::find_if(list.begin(), list.end(),
                                        [version](const Migration& m) { return m.version == version; });
        if (known == list.end() || checksum(known->sql) != recorded) {
            return schema_error("schema migration " + std::to_string(version) +
                                " differs from this build");
        }
    }

    std::uint32_t current = applied.value().empty() ? 0U : applied.value().rbegin()->first;
    for (const auto& migration : list) {
        if (applied.value().count(migration.version) != 0) {
            continue;
        }
        auto done = database.write([&](Connection& db) -> domain::Result<void> {
            if (auto run = db.exec(migration.sql); !run) {
                return run;
            }
            auto record = db.prepare(
                "INSERT INTO schema_migrations (version, checksum, applied_at) VALUES (?, ?, ?)");
            if (!record) {
                return std::move(record).error();
            }
            record->bind(1, static_cast<std::int64_t>(migration.version))
                .bind(2, checksum(migration.sql))
                .bind(3, static_cast<std::int64_t>(applied_at));
            return record->run();
        });
        if (!done) {
            return done.error();
        }
        current = migration.version;
    }
    return current;
}

} // namespace music_service::infrastructure::storage::sqlite
