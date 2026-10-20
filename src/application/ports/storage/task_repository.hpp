#pragma once

#include "domain/result.hpp"
#include "domain/tasks/download_intent.hpp"
#include "domain/tasks/task_status.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace music_service::application::ports {

// Persistent task snapshot as seen through the repository. Secrets, leases and
// short-lived URLs are never part of it.
struct TaskRecord {
    std::string task_id;
    std::string owner_id;
    domain::TaskStatus status{domain::TaskStatus::queued};
    std::uint64_t version{0};
    std::optional<std::string> execution_id;
    std::uint32_t attempt{0};
    std::optional<domain::WaitReason> wait_reason;
    std::optional<std::chrono::system_clock::time_point> next_retry_at;
    std::optional<domain::TaskStatus> resume_stage;
    std::string destination_id;
    domain::DownloadFingerprint fingerprint;
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point updated_at;
};

struct CreateTaskCommand {
    domain::NormalizedDownloadIntent intent;
    std::string idempotency_key;
    std::string request_hash; // normalized request body, see data-models.md 幂等与下载去重
    // duplicate_policy=force: an equal active task is ACTIVE_DUPLICATE instead of reused.
    bool force{false};
};

struct CreateTaskOutcome {
    TaskRecord task;
    bool reused{false};    // an equal active intent already existed
    bool replayed{false};  // the idempotency key was already committed
};

struct TransitionCommand {
    std::string task_id;
    std::string execution_id;
    std::uint64_t expected_version{0};
    domain::TaskStatus to{domain::TaskStatus::failed};
    std::optional<domain::WaitReason> wait_reason;
    std::optional<std::chrono::system_clock::time_point> next_retry_at;
    // Where a waiting_retry task resumes when claimed again (resolving by default).
    std::optional<domain::TaskStatus> resume_stage;
};

// Single-writer persistence of download tasks (SQLite implementation since T05).
// Every write checks the expected version and, for runner writes, the execution id,
// so a late callback from an older execution cannot overwrite newer state.
class TaskRepository {
public:
    virtual ~TaskRepository() = default;

    // One transaction: idempotency record (same key and request replays the first
    // outcome, a different request is IDEMPOTENCY_CONFLICT), reuse of an equal active
    // intent, per-principal and global active limits (QUEUE_LIMIT_EXCEEDED), insert.
    virtual domain::Result<CreateTaskOutcome> create(const CreateTaskCommand& command) = 0;

    virtual domain::Result<TaskRecord> get(const std::string& owner_id,
                                           const std::string& task_id) = 0;

    // The persisted intent, for the runner and for retries.
    virtual domain::Result<domain::NormalizedDownloadIntent>
    get_intent(const std::string& task_id) = 0;

    // Claims the next runnable task (queued, or waiting_retry that is due) and assigns a
    // new execution id. Claiming does not consume an attempt.
    virtual domain::Result<std::optional<TaskRecord>>
    claim_next(std::chrono::system_clock::time_point now) = 0;

    virtual domain::Result<TaskRecord> transition(const TransitionCommand& command) = 0;

    // Applies domain::decide_cancel atomically against the current status.
    virtual domain::Result<TaskRecord> request_cancel(const std::string& owner_id,
                                                      const std::string& task_id) = 0;
};

} // namespace music_service::application::ports
