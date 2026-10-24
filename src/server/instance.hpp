#pragma once

#include "application/config/service_config.hpp"
#include "domain/result.hpp"
#include "infrastructure/clock.hpp"
#include "infrastructure/filesystem/data_layout.hpp"
#include "infrastructure/filesystem/instance_lock.hpp"
#include "infrastructure/observability/correlation.hpp"
#include "infrastructure/storage/sqlite/account_repositories.hpp"
#include "infrastructure/storage/sqlite/database.hpp"
#include "infrastructure/storage/sqlite/task_repositories.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace music_service::server {

// The persistent side of one data_dir, held exclusively: the serving process and
// every offline management command open it the same way. Members are destroyed in
// reverse order, so the repositories go first, the database closes next and the
// instance lock is released last.
struct Instance {
    infrastructure::filesystem::DataLayout layout;
    infrastructure::filesystem::InstanceLock lock;
    std::shared_ptr<infrastructure::SystemClock> clock;
    std::shared_ptr<infrastructure::observability::IdGenerator> ids;
    std::unique_ptr<infrastructure::storage::sqlite::Database> database;
    std::shared_ptr<infrastructure::storage::sqlite::SqliteIdentityRepository> identities;
    std::shared_ptr<infrastructure::storage::sqlite::SqliteCredentialRepository> credentials;
    std::shared_ptr<infrastructure::storage::sqlite::SqliteDestinationRepository> destinations;
    std::shared_ptr<infrastructure::storage::sqlite::SqliteTaskRepository> tasks;
    std::shared_ptr<infrastructure::storage::sqlite::SqlitePublicationRepository> publications;
    std::shared_ptr<infrastructure::storage::sqlite::SqliteMaintenanceStore> maintenance;

    // Closes the repositories and the database, then releases the lock.
    void close() noexcept;
    ~Instance() { close(); }
};

// A file name in backups/ that does not exist yet: <prefix>-20261022T031500Z.sqlite3,
// with -2, -3, ... appended when that second is already taken.
std::filesystem::path new_backup_path(const std::filesystem::path& backups, const std::string& prefix,
                                      std::chrono::system_clock::time_point time);

// Prepares data_dir, takes the instance lock (INSTANCE_LOCKED while the service or
// another command runs), opens the database and applies migrations.
domain::Result<std::unique_ptr<Instance>> open_instance(const application::config::ServiceConfig& config,
    std::shared_ptr<application::ports::Metrics> metrics = {});

struct DestinationState {
    application::config::DestinationConfig config;
    std::filesystem::path root; // canonical, as bound
    bool available{false};
};

// Checks every configured destination against its immutable binding: a new id is
// registered (its root must exist, be writable and lie outside data_dir); a known id
// with another root is DESTINATION_ROOT_CHANGED; a bound id missing from the
// configuration that tasks, files or intents still use is DESTINATION_IN_USE. A
// known root that is offline right now stays bound and is reported unavailable.
domain::Result<std::vector<DestinationState>> register_destinations(Instance& instance,
                                                                    const application::config::ServiceConfig& config);

} // namespace music_service::server
