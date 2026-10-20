#pragma once

#include "infrastructure/storage/sqlite/database.hpp"

#include <chrono>
#include <cstdint>
#include <span>
#include <string_view>

namespace music_service::infrastructure::storage::sqlite {

struct Migration {
    std::uint32_t version;
    std::string_view sql;
};

// The schema migrations shipped with this build, in ascending version order.
std::span<const Migration> builtin_migrations();

// Brings the database up to the newest known version. Each migration runs in its own
// write transaction and is recorded in schema_migrations with a checksum of its SQL.
// Refuses to touch a database that was migrated by a newer build or whose recorded
// checksums differ from this build's migrations. Returns the resulting version.
domain::Result<std::uint32_t> migrate(Database& database,
                                      std::chrono::system_clock::time_point now,
                                      std::span<const Migration> migrations = builtin_migrations());

} // namespace music_service::infrastructure::storage::sqlite
