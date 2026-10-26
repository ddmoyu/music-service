#include "infrastructure/storage/sqlite/schema.hpp"

#include <string>

namespace music_service::infrastructure::storage::sqlite {
namespace {

// The complete current schema. Times are UTC milliseconds since the Unix epoch.
constexpr std::string_view schema_sql = R"sql(
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

CREATE TABLE source_download_statistics (
    owner_id TEXT NOT NULL REFERENCES principals (id),
    source TEXT NOT NULL,
    successes INTEGER NOT NULL CHECK (successes >= 0),
    failures INTEGER NOT NULL CHECK (failures >= 0),
    PRIMARY KEY (owner_id, source)
) STRICT;

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
    revoked_at INTEGER,
    refresh_state TEXT NOT NULL DEFAULT 'idle'
        CHECK (refresh_state IN ('idle', 'pending', 'reauthorization_required')),
    oauth_application_id TEXT,
    oauth_application_version INTEGER NOT NULL DEFAULT 0,
    refresh_after INTEGER,
    verification_id TEXT,
    verification_state TEXT NOT NULL DEFAULT 'idle' CHECK (verification_state IN ('idle', 'pending', 'complete')),
    verification_error TEXT,
    membership_status TEXT NOT NULL DEFAULT 'unknown' CHECK (membership_status IN ('unknown', 'none', 'active', 'expired')),
    membership_type TEXT NOT NULL DEFAULT 'unknown' CHECK (membership_type IN ('unknown', 'none', 'vip', 'svip', 'music_package')),
    membership_name TEXT,
    membership_expires_at INTEGER,
    membership_checked_at INTEGER,
    membership_error TEXT,
    membership_expires_on TEXT,
    enabled INTEGER NOT NULL DEFAULT 1 CHECK (enabled IN (0, 1)),
    CHECK (membership_expires_at IS NULL OR membership_expires_on IS NULL),
    CHECK ((enabled = 0 AND secret_ref IS NULL) OR (enabled = 1 AND secret_ref IS NOT NULL AND status != 'revoked'))
) STRICT;
CREATE INDEX credentials_refresh_due ON credentials
    (oauth_application_id, oauth_application_version, refresh_state, status, refresh_after);
CREATE INDEX credentials_owner_source ON credentials (owner_id, source);
CREATE UNIQUE INDEX credentials_one_enabled ON credentials (owner_id, source) WHERE enabled = 1;

CREATE TABLE credential_secret_deletions (
    secret_ref TEXT PRIMARY KEY,
    credential_id TEXT NOT NULL REFERENCES credentials (id),
    owner_id TEXT NOT NULL,
    source TEXT NOT NULL
) STRICT;
CREATE INDEX credential_secret_deletions_scope ON credential_secret_deletions (owner_id, source);
CREATE INDEX credential_secret_deletions_account ON credential_secret_deletions (credential_id);

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
    cancel_requested_at INTEGER,
    execution TEXT,
    checkpoint TEXT,
    attempt_used_ms INTEGER NOT NULL DEFAULT 0
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

domain::Result<std::uint32_t> schema_version(Connection& db) {
    auto table = db.prepare("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'schema_metadata'");
    if (!table) return std::move(table).error();
    auto exists = table->step();
    if (!exists) return std::move(exists).error();
    if (!*exists) {
        auto objects = db.prepare("SELECT 1 FROM sqlite_master WHERE name NOT GLOB 'sqlite_*' LIMIT 1");
        if (!objects) return std::move(objects).error();
        auto any = objects->step();
        if (!any) return std::move(any).error();
        if (*any) return schema_error("database does not use the current schema; use a new data_dir");
        return 0U;
    }
    auto record = db.prepare("SELECT version, checksum FROM schema_metadata WHERE id = 1");
    if (!record) return std::move(record).error();
    auto row = record->step();
    if (!row) return std::move(row).error();
    if (!*row || record->integer(0) != current_schema_version || record->text(1) != checksum(schema_sql)) {
        return schema_error("database schema differs from this build; use a new data_dir");
    }
    return current_schema_version;
}

domain::Result<std::uint32_t> initialize_schema(Database& database, std::chrono::system_clock::time_point now) {
    return database.write([&](Connection& db) -> domain::Result<std::uint32_t> {
        auto current = schema_version(db);
        if (!current) return std::move(current).error();
        if (*current != 0) return *current;
        if (auto created = db.exec(schema_sql); !created) return std::move(created).error();
        if (auto created = db.exec("CREATE TABLE schema_metadata ("
                                  "id INTEGER PRIMARY KEY CHECK (id = 1),"
                                  "version INTEGER NOT NULL, checksum TEXT NOT NULL,"
                                  "created_at INTEGER NOT NULL) STRICT"); !created) {
            return std::move(created).error();
        }
        auto record = db.prepare("INSERT INTO schema_metadata (id, version, checksum, created_at) VALUES (1, ?, ?, ?)");
        if (!record) return std::move(record).error();
        record->bind(1, static_cast<std::int64_t>(current_schema_version))
            .bind(2, checksum(schema_sql))
            .bind(3, std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
        if (auto saved = record->run(); !saved) return std::move(saved).error();
        return current_schema_version;
    });
}

} // namespace music_service::infrastructure::storage::sqlite
