#include "downloads/recovery.hpp"

#include "downloads/finalizer.hpp"

#include <algorithm>
#include <utility>

namespace music_service::downloads {

namespace {

using domain::ErrorCode;
using domain::TaskStatus;

bool offline(const domain::Error& error) { return error.code == ErrorCode::destination_unavailable; }

} // namespace

std::string_view to_string(RecoveryAction action) noexcept {
    switch (action) {
    case RecoveryAction::completed:
        return "completed";
    case RecoveryAction::retry:
        return "retry";
    case RecoveryAction::failed:
        return "failed";
    case RecoveryAction::deferred:
        return "deferred";
    case RecoveryAction::canceled:
        return "canceled";
    }
    return "deferred";
}

std::size_t RecoveryReport::count(RecoveryAction action) const {
    return static_cast<std::size_t>(
        std::count_if(tasks.begin(), tasks.end(), [action](const RecoveredTask& t) { return t.action == action; }));
}

Recovery::Recovery(std::shared_ptr<ports::PublicationRepository> publications,
                   std::shared_ptr<ports::TaskRepository> tasks, std::shared_ptr<ports::DestinationFiles> files,
                   RecoveryOptions options)
    : publications_(std::move(publications)), tasks_(std::move(tasks)), files_(std::move(files)),
      options_(options) {}

RecoveredTask Recovery::give_up(const ports::TaskRecord& task, const std::string& reason,
                                const domain::Error& cause, bool may_retry) {
    auto intent = tasks_->get_intent(task.task_id);
    const bool local_import = intent && intent->original_track().source.value() == "local_ncm";
    const bool retry = may_retry && !local_import && task.attempt < options_.max_attempts;
    auto abandoned = publications_->abandon(task.task_id, task.execution_id.value_or(""), task.version,
                                            retry ? TaskStatus::waiting_retry : TaskStatus::failed,
                                            finalization_error_json(reason, cause));
    if (!abandoned) {
        return {task.task_id, RecoveryAction::deferred, "storage"};
    }
    return {task.task_id, retry ? RecoveryAction::retry : RecoveryAction::failed, reason};
}

RecoveredTask Recovery::recover(const ports::PendingPublication& pending) {
    const auto& intent = pending.intent;
    const auto& task = pending.task;
    auto evidence = decode_evidence(intent.evidence_json);
    if (!evidence) {
        return give_up(task, "evidence_unreadable", evidence.error(), false);
    }

    auto target = publication_matches(*files_, intent.destination_id, intent.reserved_path, *evidence);
    if (!target && offline(target.error())) {
        return {task.task_id, RecoveryAction::deferred, "destination_unavailable"};
    }
    if (target && target.value()) {
        // Renamed before the crash (or linked, with the work file still present).
        auto committed = commit_publication(*publications_, *files_, pending, evidence.value());
        if (!committed) {
            return {task.task_id, RecoveryAction::deferred, "storage"};
        }
        return {task.task_id, RecoveryAction::completed, "target_matches"};
    }

    auto work = publication_matches(*files_, intent.destination_id, intent.workspace_file, *evidence);
    if (!work && offline(work.error())) {
        return {task.task_id, RecoveryAction::deferred, "destination_unavailable"};
    }
    if (!work || !work.value()) {
        // Nothing intact to publish; a half-rewritten work file is not trusted.
        const domain::Error cause{ErrorCode::file_system, "no intact product", false, {}};
        return give_up(task, "product_missing", cause, true);
    }
    auto placed = move_to_reservation(*publications_, *files_, intent, evidence->wanted_path,
                                      options_.max_name_candidates);
    if (!placed) {
        if (offline(placed.error())) {
            return {task.task_id, RecoveryAction::deferred, "destination_unavailable"};
        }
        if (is_move_unconfirmed(placed.error())) {
            return {task.task_id, RecoveryAction::deferred, std::string{move_unconfirmed}};
        }
        const auto reason = placed.error().code == ErrorCode::conflict ? "target_conflict" : "move_failed";
        return give_up(task, reason, placed.error(), false);
    }
    auto moved = pending;
    moved.intent.reserved_path = std::move(placed).value();
    auto committed = commit_publication(*publications_, *files_, moved, evidence.value());
    if (!committed) {
        return {task.task_id, RecoveryAction::deferred, "storage"};
    }
    return {task.task_id, RecoveryAction::completed,
            moved.intent.reserved_path == intent.reserved_path ? "moved" : "relocated"};
}

domain::Result<RecoveryReport> Recovery::run() {
    RecoveryReport report;
    auto pending = publications_->pending();
    if (!pending) {
        return std::move(pending).error();
    }
    for (const auto& item : pending.value()) {
        report.tasks.push_back(recover(item));
    }

    // begin() writes the intent and finalizing together, so this only finds damage.
    auto finalizing = publications_->in_status(TaskStatus::finalizing);
    if (!finalizing) {
        return std::move(finalizing).error();
    }
    for (const auto& task : finalizing.value()) {
        const auto has_intent = std::any_of(pending->begin(), pending->end(), [&](const ports::PendingPublication& p) {
            return p.task.task_id == task.task_id;
        });
        if (!has_intent) {
            const domain::Error cause{ErrorCode::internal, "finalizing without an intent", false, {}};
            report.tasks.push_back(give_up(task, "intent_missing", cause, false));
        }
    }

    auto canceling = publications_->in_status(TaskStatus::canceling);
    if (!canceling) {
        return std::move(canceling).error();
    }
    for (const auto& task : canceling.value()) {
        // Unpublished work of a canceled task is removed; an offline destination keeps
        // it until a later cleanup, the cancel itself still completes.
        (void)files_->remove_workspace(task.destination_id, task.task_id);
        auto done = tasks_->transition({task.task_id, task.execution_id.value_or(""), task.version,
                                        TaskStatus::canceled, std::nullopt, std::nullopt, std::nullopt});
        report.tasks.push_back({task.task_id, done ? RecoveryAction::canceled : RecoveryAction::deferred,
                                done ? "execution_gone" : "storage"});
    }
    return report;
}

} // namespace music_service::downloads
