#include "infrastructure/storage/sqlite/backup.hpp"

#include "infrastructure/filesystem/files.hpp"
#include "infrastructure/filesystem/paths.hpp"
#include "infrastructure/storage/sqlite/connection.hpp"

#include <string>
#include <system_error>

namespace music_service::infrastructure::storage::sqlite {

namespace fs = std::filesystem;

namespace {

domain::Error invalid_backup(const fs::path& file, std::string message) {
    return domain::Error{domain::ErrorCode::validation_failed,
                         "backup " + filesystem::path_to_utf8(file) + " " + message, false, {}};
}

fs::path sibling(const fs::path& file, std::string_view suffix) {
    auto name = file.filename();
    name += filesystem::path_from_utf8(std::string{suffix});
    return file.parent_path() / name;
}

domain::Result<void> integrity_ok(Connection& db, const fs::path& file) {
    auto check = db.prepare("PRAGMA integrity_check");
    if (!check) {
        return std::move(check).error();
    }
    auto row = check->step();
    if (!row) {
        return std::move(row).error();
    }
    if (!row.value() || check->text(0) != "ok") {
        return invalid_backup(file, "fails integrity_check");
    }
    return domain::ok();
}

domain::Result<BackupInfo> inspect(Connection& db, const fs::path& file) {
    if (auto ok = integrity_ok(db, file); !ok) {
        return std::move(ok).error();
    }
    auto version = schema_version(db);
    if (!version) {
        return invalid_backup(file, "has an unusable schema: " + version.error().message);
    }
    if (*version != current_schema_version) return invalid_backup(file, "has no current schema");
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    return BackupInfo{version.value(), ec ? 0 : static_cast<std::uint64_t>(size)};
}

// Copies source into a new file at partial and checks it there.
domain::Result<BackupInfo> copy_checked(const fs::path& source, const fs::path& partial) {
    if (auto removed = filesystem::remove_file(partial); !removed) {
        return std::move(removed).error();
    }
    {
        auto from = Connection::open(source, Connection::Mode::read_only);
        if (!from) {
            return std::move(from).error();
        }
        auto to = Connection::open(partial, Connection::Mode::read_write_create);
        if (!to) {
            return std::move(to).error();
        }
        if (auto copied = to->copy_from(from.value()); !copied) {
            return std::move(copied).error();
        }
        // The copy carries the source's WAL flag; a backup is one standalone file.
        if (auto mode = to->exec("PRAGMA journal_mode = DELETE"); !mode) {
            return std::move(mode).error();
        }
        auto checked = inspect(to.value(), partial);
        if (!checked) {
            return std::move(checked).error();
        }
    }
    std::error_code ec;
    const auto size = fs::file_size(partial, ec);
    if (ec) {
        return domain::Error{domain::ErrorCode::storage_unavailable, "cannot read the backup size", true, {}};
    }
    auto info = verify_backup(partial);
    if (info) {
        info->size_bytes = static_cast<std::uint64_t>(size);
    }
    return info;
}

} // namespace

domain::Result<BackupInfo> verify_backup(const fs::path& file) {
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        return invalid_backup(file, "is not a file");
    }
    auto db = Connection::open(file, Connection::Mode::read_only);
    if (!db) {
        return std::move(db).error();
    }
    return inspect(db.value(), file);
}

domain::Result<BackupInfo> backup_database(const fs::path& source, const fs::path& target) {
    std::error_code ec;
    if (!fs::is_regular_file(source, ec)) {
        return domain::Error{domain::ErrorCode::not_found,
                             "no database at " + filesystem::path_to_utf8(source), false, {}};
    }
    if (fs::exists(target, ec)) {
        return domain::Error{domain::ErrorCode::conflict,
                             filesystem::path_to_utf8(target) + " already exists", false, {}};
    }
    const auto partial = sibling(target, ".partial");
    auto info = copy_checked(source, partial);
    if (!info) {
        (void)filesystem::remove_file(partial);
        return info;
    }
    if (auto moved = filesystem::rename_no_replace(partial, target); !moved) {
        (void)filesystem::remove_file(partial);
        return std::move(moved).error();
    }
    return info;
}

domain::Result<BackupInfo> restore_database(const fs::path& backup, const fs::path& database,
                                            const fs::path& replaced_copy) {
    auto info = verify_backup(backup);
    if (!info) {
        return info;
    }
    std::error_code ec;
    if (fs::exists(database, ec)) {
        // The database being replaced stays available, WAL content included.
        if (auto kept = backup_database(database, replaced_copy); !kept) {
            return std::move(kept).error();
        }
    }
    const auto restoring = sibling(database, ".restoring");
    auto copied = copy_checked(backup, restoring);
    if (!copied) {
        (void)filesystem::remove_file(restoring);
        return copied;
    }
    for (const auto* suffix : {"-wal", "-shm"}) {
        if (auto removed = filesystem::remove_file(sibling(database, suffix)); !removed) {
            (void)filesystem::remove_file(restoring);
            return std::move(removed).error();
        }
    }
    fs::rename(restoring, database, ec);
    if (ec) {
        (void)filesystem::remove_file(restoring);
        return domain::Error{domain::ErrorCode::storage_unavailable,
                             "cannot replace the database: " + ec.message(), false, {}};
    }
    return copied;
}

} // namespace music_service::infrastructure::storage::sqlite
