#pragma once

#include "domain/result.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// Opaque SQLite handles: sqlite3.h stays inside the storage implementation.
struct sqlite3;
struct sqlite3_stmt;

namespace music_service::infrastructure::storage::sqlite {

// Maps a SQLite result code: busy/locked/full/I/O become retryable
// STORAGE_UNAVAILABLE, constraint violations CONFLICT, everything else INTERNAL.
domain::Error sqlite_error(int code, std::string_view context, std::string_view detail = {});

class Statement {
public:
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&& other) noexcept;
    ~Statement();

    // Parameters are 1-based, as in SQLite.
    Statement& bind(int index, std::string_view text);
    // Exact matches so strings never pick the optional overload by conversion.
    Statement& bind(int index, const std::string& text) { return bind(index, std::string_view{text}); }
    Statement& bind(int index, const char* text) { return bind(index, std::string_view{text}); }
    Statement& bind(int index, std::int64_t value);
    Statement& bind(int index, const std::optional<std::string>& text);
    Statement& bind(int index, const std::optional<std::int64_t>& value);
    Statement& bind_null(int index);

    // true when a row is available, false when the statement is done.
    domain::Result<bool> step();
    // Runs to completion, for statements that return no rows.
    domain::Result<void> run();
    void reset() noexcept;

    // Columns are 0-based.
    [[nodiscard]] bool is_null(int column) const;
    [[nodiscard]] std::int64_t integer(int column) const;
    [[nodiscard]] std::string text(int column) const;
    [[nodiscard]] std::optional<std::int64_t> optional_integer(int column) const;
    [[nodiscard]] std::optional<std::string> optional_text(int column) const;

private:
    friend class Connection;
    Statement(sqlite3_stmt* stmt, sqlite3* db) : stmt_(stmt), db_(db) {}

    sqlite3_stmt* stmt_{nullptr};
    sqlite3* db_{nullptr};
    int bind_error_{0};
};

class Connection {
public:
    enum class Mode { read_write_create, read_only };

    static domain::Result<Connection> open(const std::filesystem::path& file, Mode mode);

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&& other) noexcept;
    Connection& operator=(Connection&& other) noexcept;
    ~Connection();

    domain::Result<void> exec(std::string_view sql);
    domain::Result<Statement> prepare(std::string_view sql);
    [[nodiscard]] std::int64_t changes() const;

private:
    explicit Connection(sqlite3* db) : db_(db) {}

    sqlite3* db_{nullptr};
};

} // namespace music_service::infrastructure::storage::sqlite
