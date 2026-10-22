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
    // Public facts of the execution: actual track, quality, container, warnings.
    std::optional<std::string> execution_json;
    // Private resume evidence of the work file (never returned by the API).
    std::optional<std::string> checkpoint_json;
    // Whether the current logical attempt is still open (an admission pause keeps it).
    bool attempt_open{false};
    // Absolute deadline, fixed by the first begin_attempt and never extended.
    std::optional<std::chrono::system_clock::time_point> deadline_at;
    // Running time the open attempt has used, without its admission pauses.
    std::chrono::milliseconds attempt_used{0};
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
    std::uint64_t size_bytes{0}; // what was published; a file of another size is not it
    bool file_available{false};
    std::string properties_json{"{}"};
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
    // Replace the stored values when set.
    std::optional<std::string> progress_json{};
    std::optional<std::string> execution_json{};
    std::optional<std::string> error_json{};
};

// A task the scheduler may claim now: queued, or waiting_retry and due.
struct RunnableTask {
    std::string task_id;
    std::string owner_id;
    std::string source; // original track source, for the admission readiness hint
    std::chrono::system_clock::time_point due;
};

struct BeginAttemptCommand {
    std::string task_id;
    std::string execution_id;
    std::uint64_t expected_version{0};
    // From the first attempt on the task must end within this budget.
    std::chrono::seconds absolute_budget{1800};
};

// The execution stops without finishing: its I/O has ended. admission keeps the attempt
// open and does not count; upstream_retry closes it, so the next claim starts a new one.
struct DeferCommand {
    std::string task_id;
    std::string execution_id;
    std::uint64_t expected_version{0};
    domain::WaitReason reason{domain::WaitReason::admission};
    std::chrono::system_clock::time_point next_retry_at;
    domain::TaskStatus resume_stage{domain::TaskStatus::resolving}; // resolving | probing | downloading
    std::chrono::milliseconds attempt_used{0};
    std::optional<std::string> checkpoint_json{}; // replaces the stored evidence when set
    bool clear_checkpoint{false};                  // the partial file must not be resumed
    std::optional<std::string> progress_json{};
    std::optional<std::string> error_json{}; // the failure an upstream_retry waits after
};

// Progress and resume evidence of the running execution; refused once it was canceled or
// changed (VERSION_CONFLICT), so a late checkpoint cannot revive anything.
struct CheckpointCommand {
    std::string task_id;
    std::string execution_id;
    std::uint64_t expected_version{0};
    std::optional<std::string> progress_json{};
    std::optional<std::string> execution_json{};
    std::optional<std::string> checkpoint_json{};
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

    // Runnable tasks for fair dispatch: per owner at most per_owner of them, earliest due
    // first, owners in no particular order; at most limit in total.
    virtual domain::Result<std::vector<RunnableTask>>
    runnable(std::chrono::system_clock::time_point now, std::uint32_t per_owner, std::uint32_t limit) = 0;

    // Claims this task if it is still runnable (nullopt otherwise), as claim_next does.
    virtual domain::Result<std::optional<TaskRecord>> claim(const std::string& task_id,
                                                            std::chrono::system_clock::time_point now) = 0;

    // Starts a new logical attempt unless the claimed one is still open: increments the
    // attempt, records it in task_attempts and fixes the absolute deadline the first time.
    // A still open attempt is only linked to the new execution.
    virtual domain::Result<TaskRecord> begin_attempt(const BeginAttemptCommand& command) = 0;

    // Running (resolving .. processing) -> waiting_retry, see DeferCommand.
    virtual domain::Result<TaskRecord> defer(const DeferCommand& command) = 0;

    virtual domain::Result<TaskRecord> checkpoint(const CheckpointCommand& command) = 0;

    // Leaving failed closes the open attempt.
    virtual domain::Result<TaskRecord> transition(const TransitionCommand& command) = 0;

    // Startup only, before any runner: tasks left in a running stage by the previous
    // process become due waiting_retry (admission) at the stage they can resume from
    // (processing resumes from its transfer-complete file through downloading). Keeps
    // attempts, deadlines and evidence. Returns them.
    virtual domain::Result<std::vector<TaskRecord>> requeue_interrupted(std::chrono::system_clock::time_point now) = 0;

    // Applies domain::decide_cancel atomically against the current status.
    virtual domain::Result<TaskRecord> request_cancel(const std::string& owner_id,
                                                      const std::string& task_id) = 0;
};

} // namespace music_service::application::ports
