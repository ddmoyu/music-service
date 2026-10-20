#pragma once

#include "infrastructure/storage/sqlite/connection.hpp"

#include "domain/result.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Small helpers shared by the SQLite repositories. Internal to storage.
namespace music_service::infrastructure::storage::sqlite::detail {

inline std::int64_t to_millis(std::chrono::system_clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
}

inline std::chrono::system_clock::time_point from_millis(std::int64_t millis) {
    return std::chrono::system_clock::time_point{
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::milliseconds{millis})};
}

inline std::optional<std::int64_t>
to_millis(const std::optional<std::chrono::system_clock::time_point>& time) {
    return time ? std::optional<std::int64_t>{to_millis(*time)} : std::nullopt;
}

inline std::optional<std::chrono::system_clock::time_point>
from_millis(const std::optional<std::int64_t>& millis) {
    return millis ? std::optional{from_millis(*millis)} : std::nullopt;
}

// Runs a query and maps every row.
template <class T, class Map>
domain::Result<std::vector<T>> collect(Statement& statement, Map&& map) {
    std::vector<T> rows;
    while (true) {
        auto row = statement.step();
        if (!row) {
            return std::move(row).error();
        }
        if (!*row) {
            return rows;
        }
        rows.push_back(map(statement));
    }
}

// Runs a query and maps the first row, if any.
template <class T, class Map>
domain::Result<std::optional<T>> first(Statement& statement, Map&& map) {
    auto row = statement.step();
    if (!row) {
        return std::move(row).error();
    }
    if (!*row) {
        return std::optional<T>{};
    }
    return std::optional<T>{map(statement)};
}

inline domain::Error not_found(std::string what) {
    return domain::Error{domain::ErrorCode::not_found, std::move(what) + " not found", false, {}};
}

} // namespace music_service::infrastructure::storage::sqlite::detail
