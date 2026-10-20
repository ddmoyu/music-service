#pragma once

#include "domain/result.hpp"
#include "domain/tasks/download_intent.hpp"
#include "domain/tasks/task_status.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace music_dl::application::ports {

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
    domain::DownloadFingerprint fingerprint;
};

struct CreateTaskCommand {
    domain::NormalizedDownloadIntent intent;
    std::string idempotency_key;
    std::string request_hash;
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
};

// Single-writer persistence of download tasks (implemented on SQLite in T05).
// Every write checks the expected version and, for runner writes, the execution id,
// so a late callback from an older execution cannot overwrite newer state.
class TaskRepository {
public:
    virtual ~TaskRepository() = default;

    // One transaction: idempotency, fingerprint uniqueness among active tasks, insert.
    virtual domain::Result<CreateTaskOutcome> create(const CreateTaskCommand& command) = 0;

    virtual domain::Result<TaskRecord> get(const std::string& owner_id,
                                           const std::string& task_id) = 0;

    // Claims the next runnable task (queued, or waiting_retry that is due) and assigns a
    // new execution id. Claiming does not consume an attempt.
    virtual domain::Result<std::optional<TaskRecord>>
    claim_next(std::chrono::system_clock::time_point now) = 0;

    virtual domain::Result<TaskRecord> transition(const TransitionCommand& command) = 0;

    // Applies domain::decide_cancel atomically against the current status.
    virtual domain::Result<TaskRecord> request_cancel(const std::string& owner_id,
                                                      const std::string& task_id) = 0;
};

} // namespace music_dl::application::ports
