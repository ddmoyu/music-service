#pragma once

#include "application/ports/storage/destination_files.hpp"
#include "application/ports/storage/publication_repository.hpp"
#include "application/ports/storage/task_repository.hpp"
#include "domain/result.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace music_service::downloads {

namespace ports = application::ports;

struct RecoveryOptions {
    std::uint32_t max_attempts{3}; // downloads.max_attempts
    std::uint32_t max_name_candidates{100};
};

enum class RecoveryAction {
    completed, // the published or still-to-move file matched the intent's evidence
    retry,     // no intact product: waiting_retry to download again
    failed,    // no intact product and no attempts left, or the file cannot be placed
    deferred,  // destination offline: left finalizing for the next start
    canceled,  // a canceling task whose execution is gone
};

struct RecoveredTask {
    std::string task_id;
    RecoveryAction action{RecoveryAction::deferred};
    std::string reason;
};

struct RecoveryReport {
    std::vector<RecoveredTask> tasks;
    [[nodiscard]] std::size_t count(RecoveryAction action) const;
};

// Startup recovery before any runner or listener starts (runtime-and-tasks.md 崩溃恢复,
// internal-contracts.md 7). For each pending publication intent the files decide, never
// the recorded stage:
//   target holds the evidence bytes      -> commit asset and completed (A20);
//   work file holds them                 -> rename, past unknown files, then commit;
//   neither                              -> waiting_retry while attempts remain, else
//                                           failed with FINALIZATION_FAILED;
//   destination offline                  -> left as is until it is back.
// A file at the target that does not match the evidence is never overwritten or claimed.
// Canceling tasks finish as canceled after their work directory is removed. Tasks in the
// running stages (resolving..processing) are resumed by the scheduler (T17), not here.
class Recovery {
public:
    Recovery(std::shared_ptr<ports::PublicationRepository> publications, std::shared_ptr<ports::TaskRepository> tasks,
             std::shared_ptr<ports::DestinationFiles> files, RecoveryOptions options = {});

    domain::Result<RecoveryReport> run();

private:
    RecoveredTask recover(const ports::PendingPublication& pending);
    RecoveredTask give_up(const ports::TaskRecord& task, const std::string& reason, const domain::Error& cause,
                          bool may_retry);

    std::shared_ptr<ports::PublicationRepository> publications_;
    std::shared_ptr<ports::TaskRepository> tasks_;
    std::shared_ptr<ports::DestinationFiles> files_;
    RecoveryOptions options_;
};

std::string_view to_string(RecoveryAction action) noexcept;

} // namespace music_service::downloads
