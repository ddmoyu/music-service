#pragma once

#include "domain/result.hpp"
#include "infrastructure/storage/sqlite/schema.hpp"

#include <cstdint>
#include <filesystem>

namespace music_service::infrastructure::storage::sqlite {

struct BackupInfo {
    std::uint32_t schema_version{0};
    std::uint64_t size_bytes{0};
};

// A consistent copy of the database at source, safe while the service runs (SQLite online
// backup, committed WAL content included; configuration-and-deployment.md 观测与维护).
// The copy is a single self-contained file (rollback journal mode). It is written next to
// target first, must pass integrity_check and the schema check, and is then moved into
// place; an existing target is never replaced (CONFLICT).
domain::Result<BackupInfo> backup_database(const std::filesystem::path& source, const std::filesystem::path& target);

// Opens a backup read-only and checks it: integrity_check, and a schema this build knows
// (exactly the current schema and checksum).
domain::Result<BackupInfo> verify_backup(const std::filesystem::path& file);

// Replaces the database at database with a verified backup. The caller holds the instance
// lock, so nothing has the database open. The current database is first copied to
// replaced_copy (a new file) so the restore can be undone; then the backup is copied next
// to the database, its WAL and shared-memory files are removed and the copy takes the
// database's name. Both databases must use the current schema.
domain::Result<BackupInfo> restore_database(const std::filesystem::path& backup, const std::filesystem::path& database,
                                            const std::filesystem::path& replaced_copy);

} // namespace music_service::infrastructure::storage::sqlite
