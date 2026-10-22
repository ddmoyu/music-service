#include "downloads/maintenance.hpp"

#include <algorithm>
#include <map>
#include <utility>

namespace music_service::downloads {

using domain::TaskStatus;

std::size_t MaintenanceReport::count(WorkspaceVerdict verdict) const {
    return static_cast<std::size_t>(
        std::count_if(workspaces.begin(), workspaces.end(), [&](const auto& w) { return w.verdict == verdict; }));
}

std::string_view to_string(WorkspaceVerdict verdict) noexcept {
    switch (verdict) {
    case WorkspaceVerdict::kept_retained:
        return "kept_retained";
    case WorkspaceVerdict::removed_orphan:
        return "removed_orphan";
    case WorkspaceVerdict::removed_finished:
        return "removed_finished";
    case WorkspaceVerdict::removed_expired:
        return "removed_expired";
    case WorkspaceVerdict::kept_active:
        break;
    }
    return "kept_active";
}

Maintenance::Maintenance(std::shared_ptr<ports::MaintenanceStore> store,
                         std::shared_ptr<ports::WorkspaceDirectories> directories,
                         std::vector<std::string> destination_ids, MaintenanceOptions options)
    : store_(std::move(store)), directories_(std::move(directories)), destination_ids_(std::move(destination_ids)),
      options_(options) {}

namespace {

WorkspaceVerdict judge(const ports::WorkspaceTask* task, std::chrono::system_clock::time_point now,
                       std::chrono::hours retention) {
    if (task == nullptr) {
        return WorkspaceVerdict::removed_orphan;
    }
    if (task->publication_pending || !domain::is_terminal(task->status)) {
        return WorkspaceVerdict::kept_active;
    }
    if (task->status != TaskStatus::failed) {
        return WorkspaceVerdict::removed_finished;
    }
    // A failed task without a finish time is kept: its age is not known.
    if (!task->finished_at || now - *task->finished_at < retention) {
        return WorkspaceVerdict::kept_retained;
    }
    return WorkspaceVerdict::removed_expired;
}

bool removes(WorkspaceVerdict verdict) {
    return verdict == WorkspaceVerdict::removed_orphan || verdict == WorkspaceVerdict::removed_finished ||
           verdict == WorkspaceVerdict::removed_expired;
}

} // namespace

domain::Result<MaintenanceReport> Maintenance::run(std::chrono::system_clock::time_point now) {
    MaintenanceReport report;
    for (const auto& destination : destination_ids_) {
        auto listed = directories_->list_workspaces(destination);
        if (!listed) {
            if (listed.error().code == domain::ErrorCode::destination_unavailable) {
                report.unavailable_destinations.push_back(destination);
            } else {
                report.errors.push_back(destination + ": " + listed.error().message);
            }
            continue;
        }
        if (listed->empty()) {
            continue;
        }
        // The database is read after listing: a directory created meanwhile belongs to a
        // task that already exists, so it is never mistaken for an orphan.
        auto tasks = store_->workspace_tasks(listed.value());
        if (!tasks) {
            return std::move(tasks).error();
        }
        std::map<std::string, const ports::WorkspaceTask*> by_id;
        for (const auto& task : tasks.value()) {
            by_id.emplace(task.task_id, &task);
        }
        for (const auto& task_id : listed.value()) {
            const auto found = by_id.find(task_id);
            WorkspaceCleanup item{destination, task_id,
                                  judge(found == by_id.end() ? nullptr : found->second, now,
                                        options_.failed_work_retention)};
            if (removes(item.verdict)) {
                if (auto removed = directories_->remove_workspace(destination, task_id); !removed) {
                    report.errors.push_back(destination + "/" + task_id + ": " + removed.error().message);
                    continue;
                }
            }
            report.workspaces.push_back(std::move(item));
        }
    }

    auto purged = store_->purge_expired_idempotency(now, options_.idempotency_batch);
    if (!purged) {
        report.errors.push_back("idempotency: " + purged.error().message);
    } else {
        report.idempotency_purged = purged.value();
    }
    return report;
}

} // namespace music_service::downloads
