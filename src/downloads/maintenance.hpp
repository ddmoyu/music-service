#pragma once

#include "application/ports/storage/maintenance.hpp"
#include "domain/result.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace music_service::downloads {

namespace ports = application::ports;

struct MaintenanceOptions {
    std::chrono::hours failed_work_retention{24}; // filesystem.failed_work_retention_hours
    std::size_t idempotency_batch{1000};          // records purged per run at most
};

enum class WorkspaceVerdict {
    kept_active,      // the task can still run, or a publication intent needs the files
    kept_retained,    // failed, still within failed_work_retention
    removed_orphan,   // no task by that id (for example after restoring an older database)
    removed_finished, // completed or canceled: nothing reads it any more
    removed_expired,  // failed longer ago than failed_work_retention
};

struct WorkspaceCleanup {
    std::string destination_id;
    std::string task_id;
    WorkspaceVerdict verdict{WorkspaceVerdict::kept_active};
};

struct MaintenanceReport {
    std::vector<WorkspaceCleanup> workspaces;
    std::vector<std::string> unavailable_destinations; // skipped, checked again next run
    std::vector<std::string> errors;                   // removals that failed; retried next run
    std::size_t idempotency_purged{0};

    [[nodiscard]] std::size_t count(WorkspaceVerdict verdict) const;
};

// Background housekeeping (runtime-and-tasks.md 续传与重试, configuration-and-deployment.md
// 观测与维护). Work directories are judged by their task, never by their age or file
// names: a task that can still run, or one with a pending finalization intent, keeps its
// directory whatever its status says, so recovery always finds its files. Removing is
// idempotent and a failure is reported and retried on the next run. Expired idempotency
// records are deleted in bounded batches. The scheduler may claim tasks meanwhile: a
// directory is only removed for tasks that are terminal (or unknown) when read, and a
// terminal task is never claimed again.
class Maintenance {
public:
    Maintenance(std::shared_ptr<ports::MaintenanceStore> store, std::shared_ptr<ports::WorkspaceDirectories> directories,
                std::vector<std::string> destination_ids, MaintenanceOptions options = {});

    domain::Result<MaintenanceReport> run(std::chrono::system_clock::time_point now);

private:
    std::shared_ptr<ports::MaintenanceStore> store_;
    std::shared_ptr<ports::WorkspaceDirectories> directories_;
    std::vector<std::string> destination_ids_;
    MaintenanceOptions options_;
};

std::string_view to_string(WorkspaceVerdict verdict) noexcept;

} // namespace music_service::downloads
