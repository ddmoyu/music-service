#include "domain/tasks/task_status.hpp"

#include <array>
#include <string>

namespace music_dl::domain {

namespace {

constexpr std::array all_statuses{
    TaskStatus::queued,        TaskStatus::resolving, TaskStatus::probing,
    TaskStatus::downloading,   TaskStatus::processing, TaskStatus::finalizing,
    TaskStatus::waiting_retry, TaskStatus::canceling, TaskStatus::completed,
    TaskStatus::failed,        TaskStatus::canceled,
};

} // namespace

std::string_view to_string(TaskStatus status) noexcept {
    switch (status) {
    case TaskStatus::queued:
        return "queued";
    case TaskStatus::resolving:
        return "resolving";
    case TaskStatus::probing:
        return "probing";
    case TaskStatus::downloading:
        return "downloading";
    case TaskStatus::processing:
        return "processing";
    case TaskStatus::finalizing:
        return "finalizing";
    case TaskStatus::waiting_retry:
        return "waiting_retry";
    case TaskStatus::canceling:
        return "canceling";
    case TaskStatus::completed:
        return "completed";
    case TaskStatus::failed:
        return "failed";
    case TaskStatus::canceled:
        return "canceled";
    }
    return "failed";
}

std::string_view to_string(WaitReason reason) noexcept {
    return reason == WaitReason::admission ? "admission" : "upstream_retry";
}

Result<TaskStatus> parse_task_status(std::string_view text) {
    for (const auto status : all_statuses) {
        if (to_string(status) == text) {
            return status;
        }
    }
    return Error::validation("status", "unknown task status");
}

bool is_terminal(TaskStatus status) noexcept {
    return status == TaskStatus::completed || status == TaskStatus::failed ||
           status == TaskStatus::canceled;
}

bool is_running(TaskStatus status) noexcept {
    switch (status) {
    case TaskStatus::resolving:
    case TaskStatus::probing:
    case TaskStatus::downloading:
    case TaskStatus::processing:
        return true;
    default:
        return false;
    }
}

bool is_transition_allowed(TaskStatus from, TaskStatus to) noexcept {
    using enum TaskStatus;
    switch (from) {
    case queued:
        return to == resolving || to == canceled || to == failed;
    case resolving:
        return to == probing || to == downloading || to == waiting_retry || to == failed ||
               to == canceling;
    case probing:
        return to == downloading || to == waiting_retry || to == failed || to == canceling;
    case downloading:
        return to == processing || to == waiting_retry || to == failed || to == canceling;
    case processing:
        return to == finalizing || to == failed || to == canceling;
    case finalizing:
        return to == completed || to == failed || to == waiting_retry;
    case waiting_retry:
        return to == resolving || to == probing || to == downloading || to == canceled ||
               to == failed;
    case canceling:
        return to == canceled;
    case completed:
    case failed:
    case canceled:
        return false;
    }
    return false;
}

Result<void> check_transition(TaskStatus from, TaskStatus to) {
    if (is_transition_allowed(from, to)) {
        return ok();
    }
    return Error{ErrorCode::invalid_transition,
                 std::string{"illegal task transition "} + std::string{to_string(from)} + " -> " +
                     std::string{to_string(to)},
                 false,
                 {}};
}

Result<CancelAction> decide_cancel(TaskStatus status) {
    switch (status) {
    case TaskStatus::queued:
    case TaskStatus::waiting_retry:
        return CancelAction::cancel_now;
    case TaskStatus::resolving:
    case TaskStatus::probing:
    case TaskStatus::downloading:
    case TaskStatus::processing:
        return CancelAction::enter_canceling;
    case TaskStatus::canceling:
    case TaskStatus::canceled:
        return CancelAction::already;
    case TaskStatus::finalizing:
        return Error{ErrorCode::task_finalizing, "task is being published", false, {}};
    case TaskStatus::completed:
    case TaskStatus::failed:
        return Error{ErrorCode::task_terminal, "task has already finished", false, {}};
    }
    return Error{ErrorCode::internal, "unknown task status", false, {}};
}

} // namespace music_dl::domain
