#include "infrastructure/storage/sqlite/connection.hpp"

#include "infrastructure/filesystem/paths.hpp"

#include <sqlite3.h>

#include <utility>

namespace music_service::infrastructure::storage::sqlite {

domain::Error sqlite_error(int code, std::string_view context, std::string_view detail) {
    std::string message{context};
    message.append(": ").append(sqlite3_errstr(code));
    if (!detail.empty()) {
        message.append(" (").append(detail).append(")");
    }
    switch (code & 0xFF) {
    case SQLITE_BUSY:
    case SQLITE_LOCKED:
    case SQLITE_FULL:
    case SQLITE_IOERR:
    case SQLITE_CANTOPEN:
    case SQLITE_READONLY:
        return domain::Error{domain::ErrorCode::storage_unavailable, std::move(message), true, {}};
    case SQLITE_CONSTRAINT:
        return domain::Error{domain::ErrorCode::conflict, std::move(message), false, {}};
    default:
        return domain::Error{domain::ErrorCode::internal, std::move(message), false, {}};
    }
}

Statement::Statement(Statement&& other) noexcept
    : stmt_(std::exchange(other.stmt_, nullptr)), db_(other.db_), bind_error_(other.bind_error_) {}

Statement& Statement::operator=(Statement&& other) noexcept {
    if (this != &other) {
        sqlite3_finalize(stmt_);
        stmt_ = std::exchange(other.stmt_, nullptr);
        db_ = other.db_;
        bind_error_ = other.bind_error_;
    }
    return *this;
}

Statement::~Statement() { sqlite3_finalize(stmt_); }

Statement& Statement::bind(int index, std::string_view text) {
    const int rc = sqlite3_bind_text64(stmt_, index, text.data(), text.size(), SQLITE_TRANSIENT,
                                       SQLITE_UTF8);
    if (rc != SQLITE_OK && bind_error_ == 0) {
        bind_error_ = rc;
    }
    return *this;
}

Statement& Statement::bind(int index, std::int64_t value) {
    const int rc = sqlite3_bind_int64(stmt_, index, value);
    if (rc != SQLITE_OK && bind_error_ == 0) {
        bind_error_ = rc;
    }
    return *this;
}

Statement& Statement::bind(int index, const std::optional<std::string>& text) {
    return text ? bind(index, std::string_view{*text}) : bind_null(index);
}

Statement& Statement::bind(int index, const std::optional<std::int64_t>& value) {
    return value ? bind(index, *value) : bind_null(index);
}

Statement& Statement::bind_null(int index) {
    const int rc = sqlite3_bind_null(stmt_, index);
    if (rc != SQLITE_OK && bind_error_ == 0) {
        bind_error_ = rc;
    }
    return *this;
}

domain::Result<bool> Statement::step() {
    if (bind_error_ != 0) {
        return sqlite_error(bind_error_, "bind parameter");
    }
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) {
        return true;
    }
    if (rc == SQLITE_DONE) {
        return false;
    }
    return sqlite_error(rc, "execute statement", sqlite3_errmsg(db_));
}

domain::Result<void> Statement::run() {
    while (true) {
        auto row = step();
        if (!row) {
            return std::move(row).error();
        }
        if (!*row) {
            return domain::ok();
        }
    }
}

void Statement::reset() noexcept {
    sqlite3_reset(stmt_);
    sqlite3_clear_bindings(stmt_);
    bind_error_ = 0;
}

bool Statement::is_null(int column) const {
    return sqlite3_column_type(stmt_, column) == SQLITE_NULL;
}

std::int64_t Statement::integer(int column) const { return sqlite3_column_int64(stmt_, column); }

std::string Statement::text(int column) const {
    const auto* data = sqlite3_column_text(stmt_, column);
    const auto size = sqlite3_column_bytes(stmt_, column);
    return data == nullptr ? std::string{}
                           : std::string{reinterpret_cast<const char*>(data),
                                         static_cast<std::size_t>(size)};
}

std::optional<std::int64_t> Statement::optional_integer(int column) const {
    if (is_null(column)) {
        return std::nullopt;
    }
    return integer(column);
}

std::optional<std::string> Statement::optional_text(int column) const {
    if (is_null(column)) {
        return std::nullopt;
    }
    return text(column);
}

domain::Result<Connection> Connection::open(const std::filesystem::path& file, Mode mode) {
    sqlite3* db = nullptr;
    const int flags = (mode == Mode::read_only ? SQLITE_OPEN_READONLY
                                               : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) |
                      SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_EXRESCODE;
    const auto name = filesystem::path_to_utf8(file);
    const int rc = sqlite3_open_v2(name.c_str(), &db, flags, nullptr);
    if (rc != SQLITE_OK) {
        auto error = sqlite_error(rc, "open database", name);
        sqlite3_close(db);
        return error;
    }
    sqlite3_extended_result_codes(db, 1);
    return Connection{db};
}

Connection::Connection(Connection&& other) noexcept : db_(std::exchange(other.db_, nullptr)) {}

Connection& Connection::operator=(Connection&& other) noexcept {
    if (this != &other) {
        sqlite3_close_v2(db_);
        db_ = std::exchange(other.db_, nullptr);
    }
    return *this;
}

Connection::~Connection() { sqlite3_close_v2(db_); }

domain::Result<void> Connection::exec(std::string_view sql) {
    const std::string text{sql};
    char* message = nullptr;
    const int rc = sqlite3_exec(db_, text.c_str(), nullptr, nullptr, &message);
    if (rc != SQLITE_OK) {
        const std::string detail = message != nullptr ? message : "";
        sqlite3_free(message);
        return sqlite_error(rc, "execute SQL", detail);
    }
    return domain::ok();
}

domain::Result<Statement> Connection::prepare(std::string_view sql) {
    sqlite3_stmt* stmt = nullptr;
    const int rc = sqlite3_prepare_v3(db_, sql.data(), static_cast<int>(sql.size()), 0, &stmt,
                                      nullptr);
    if (rc != SQLITE_OK) {
        return sqlite_error(rc, "prepare statement", sqlite3_errmsg(db_));
    }
    return Statement{stmt, db_};
}

std::int64_t Connection::changes() const { return sqlite3_changes64(db_); }

domain::Result<void> Connection::copy_from(Connection& source) {
    sqlite3_backup* backup = sqlite3_backup_init(db_, "main", source.db_, "main");
    if (backup == nullptr) {
        return sqlite_error(sqlite3_extended_errcode(db_), "start backup", sqlite3_errmsg(db_));
    }
    int rc = SQLITE_OK;
    // A writer of the source may hold its lock briefly: wait and continue, bounded.
    for (int busy = 0; busy < 200;) {
        rc = sqlite3_backup_step(backup, 256);
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
            ++busy;
            sqlite3_sleep(25);
            continue;
        }
        if (rc != SQLITE_OK) {
            break;
        }
    }
    const int finished = sqlite3_backup_finish(backup);
    if (rc != SQLITE_DONE) {
        return sqlite_error(rc == SQLITE_OK ? SQLITE_BUSY : rc, "backup");
    }
    if (finished != SQLITE_OK) {
        return sqlite_error(finished, "finish backup");
    }
    return domain::ok();
}

} // namespace music_service::infrastructure::storage::sqlite
