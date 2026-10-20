#pragma once

#include "domain/result.hpp"

#include <string_view>

namespace music_dl::domain {

enum class TaskStatus {
    queued,
    resolving,
    probing,
    downloading,
    processing,
    finalizing,
    waiting_retry,
    canceling,
    completed,
    failed,
    canceled,
};

enum class WaitReason { admission, upstream_retry };

std::string_view to_string(TaskStatus status) noexcept;
std::string_view to_string(WaitReason reason) noexcept;
Result<TaskStatus> parse_task_status(std::string_view text);

[[nodiscard]] bool is_terminal(TaskStatus status) noexcept;

// Statuses in which a runner holds the task and may be stopped by a cancel request.
[[nodiscard]] bool is_running(TaskStatus status) noexcept;

// Legal persistent transitions of the download state machine (runtime-and-tasks.md).
// queued/waiting_retry -> failed covers budget exhaustion or a removed capability
// detected before work resumes.
[[nodiscard]] bool is_transition_allowed(TaskStatus from, TaskStatus to) noexcept;

Result<void> check_transition(TaskStatus from, TaskStatus to);

enum class CancelAction {
    cancel_now,      // queued / waiting_retry: becomes canceled immediately
    enter_canceling, // running: persist canceling, then stop the runner
    already,         // canceling / canceled: no new side effect
};

// Decides what a cancel request does in the given status. finalizing and the other
// terminal states reject with TASK_FINALIZING / TASK_TERMINAL.
Result<CancelAction> decide_cancel(TaskStatus status);

} // namespace music_dl::domain
