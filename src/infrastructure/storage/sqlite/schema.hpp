#pragma once

#include "infrastructure/storage/sqlite/database.hpp"

#include <chrono>
#include <cstdint>

namespace music_service::infrastructure::storage::sqlite {

inline constexpr std::uint32_t current_schema_version = 4;

// Initializes an empty database atomically. An existing database must have exactly
// this build's schema and checksum; no version upgrade or data conversion is performed.
domain::Result<std::uint32_t> initialize_schema(Database& database,
                                               std::chrono::system_clock::time_point now);

// Validates the current schema without writing. Returns 0 only for an empty database.
domain::Result<std::uint32_t> schema_version(Connection& db);

} // namespace music_service::infrastructure::storage::sqlite
