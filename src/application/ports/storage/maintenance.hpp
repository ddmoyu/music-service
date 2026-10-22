#pragma once

#include "domain/result.hpp"
#include "domain/tasks/task_status.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace music_service::application::ports {

// What the database knows about the task a work directory is named after.
struct WorkspaceTask {
    std::string task_id;
    domain::TaskStatus status{domain::TaskStatus::queued};
    std::optional<std::chrono::system_clock::time_point> finished_at;
    bool publication_pending{false}; // a finalization intent still points at its files
};

// The database side of background maintenance (configuration-and-deployment.md,
// idempotency and filesystem retention).
class MaintenanceStore {
public:
    virtual ~MaintenanceStore() = default;

    // The tasks among task_ids that exist; unknown ids are left out.
    virtual domain::Result<std::vector<WorkspaceTask>> workspace_tasks(const std::vector<std::string>& task_ids) = 0;

    // Deletes at most limit idempotency records that expired at or before now; returns
    // how many went.
    virtual domain::Result<std::size_t> purge_expired_idempotency(std::chrono::system_clock::time_point now,
                                                                  std::size_t limit) = 0;
};

// Work directories ("<root>/.music-service-work/<task_id>") as directories, for cleanup.
class WorkspaceDirectories {
public:
    virtual ~WorkspaceDirectories() = default;

    // The task ids that have a work directory under the destination; entries that are not
    // tsk_ ids are not listed (and never removed). DESTINATION_UNAVAILABLE when offline.
    virtual domain::Result<std::vector<std::string>> list_workspaces(const std::string& destination_id) = 0;
    virtual domain::Result<void> remove_workspace(const std::string& destination_id, const std::string& task_id) = 0;
};

} // namespace music_service::application::ports
