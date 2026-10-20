#pragma once

#include "domain/result.hpp"
#include "domain/tasks/download_intent.hpp"
#include "domain/tasks/task_status.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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
    std::optional<std::string> retry_of;
    std::optional<std::chrono::system_clock::time_point> started_at;
    std::optional<std::chrono::system_clock::time_point> finished_at;
    std::optional<std::chrono::system_clock::time_point> cancel_requested_at;
    // Written by the runner (T17); opaque JSON kept as committed.
    std::optional<std::string> progress_json;
    std::optional<std::string> result_json;
    std::optional<std::string> error_json;
};

// A completed task whose file was checked outside the transaction. The create
// transaction reuses it only if the task, its file and their versions are unchanged.
struct ReuseCandidate {
    std::string task_id;
    std::uint64_t task_version{0};
    std::string file_id;
    std::uint64_t file_version{0};
};

// A completed task with the same intent and its file, for the outside checks.
struct CompletedResult {
    ReuseCandidate candidate;
    std::string destination_id;
    std::string relative_path;
    bool file_available{false};
};

struct CreateTaskCommand {
    domain::NormalizedDownloadIntent intent;
    std::string idempotency_key;
    std::string request_hash; // normalized request body, see data-models.md 幂等与下载去重
    // duplicate_policy=force: an equal active task is ACTIVE_DUPLICATE instead of reused,
    // and completed results are not considered.
    bool force{false};
    // Idempotency scope: POST on this route.
    std::string route{"/api/v1/downloads"};
    // A manual retry of this failed or canceled task.
    std::optional<std::string> retry_of{};
    std::optional<ReuseCandidate> completed{};
};

struct CreateTaskOutcome {
    TaskRecord task;
    bool reused{false};   // an equal active intent or a checked completed result
    bool replayed{false}; // the idempotency key was already committed
};

struct StoredResponse {
    std::string request_hash;
    int status_code{0};
    std::string response_json;
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

// Newest first: created_at, then task_id, both descending.
struct TaskQuery {
    std::string owner_id;
    std::optional<domain::TaskStatus> status;
    std::optional<std::string> source;   // original track source
    std::optional<std::string> batch_id; // tasks referenced by this batch, each once
    std::uint32_t limit{50};
    std::optional<std::pair<std::chrono::system_clock::time_point, std::string>> after;
};

struct BatchItemCommand {
    domain::NormalizedDownloadIntent intent;
    bool force{false};
    std::optional<ReuseCandidate> completed{};
};

struct CreateBatchCommand {
    std::string owner_id;
    std::string idempotency_key;
    std::string request_hash;
    std::vector<BatchItemCommand> items;
};

struct BatchRecord {
    std::string batch_id;
    std::string owner_id;
    std::uint32_t created_count{0};
    std::uint32_t reused_count{0};
    std::chrono::system_clock::time_point created_at;
    std::vector<TaskRecord> items; // input order, duplicates kept
};

struct BatchOutcome {
    BatchRecord batch;
    std::vector<bool> reused; // per item
    bool replayed{false};
};

// Single-writer persistence of download tasks (SQLite implementation since T05).
// Every write checks the expected version and, for runner writes, the execution id,
// so a late callback from an older execution cannot overwrite newer state.
class TaskRepository {
public:
    virtual ~TaskRepository() = default;

    // The stored first response of a committed idempotency key, if any (expired
    // records are ignored).
    virtual domain::Result<std::optional<StoredResponse>>
    find_idempotent(const std::string& owner_id, const std::string& route, const std::string& idempotency_key) = 0;

    // Completed tasks of this owner with the same intent, newest first, with their file.
    virtual domain::Result<std::vector<CompletedResult>>
    completed_results(const std::string& owner_id, const domain::DownloadFingerprint& fingerprint) = 0;

    // One transaction: idempotency record (same key and request replays the first
    // outcome, a different request is IDEMPOTENCY_CONFLICT), reuse of an equal active
    // intent, reuse of the checked completed result if still unchanged, per-principal
    // and global active limits (QUEUE_LIMIT_EXCEEDED), insert. A retry_of task must be
    // the owner's and failed or canceled (CONFLICT otherwise).
    virtual domain::Result<CreateTaskOutcome> create(const CreateTaskCommand& command) = 0;

    // All items in one transaction under one idempotency key: every item is created or
    // reused in input order (later duplicates reuse earlier items), or nothing is
    // stored. A failing item's error carries details {"items[i]", reason}.
    virtual domain::Result<BatchOutcome> create_batch(const CreateBatchCommand& command) = 0;

    virtual domain::Result<BatchRecord> get_batch(const std::string& owner_id, const std::string& batch_id) = 0;

    virtual domain::Result<TaskRecord> get(const std::string& owner_id,
                                           const std::string& task_id) = 0;

    virtual domain::Result<std::vector<TaskRecord>> list(const TaskQuery& query) = 0;

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
