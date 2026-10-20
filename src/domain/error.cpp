#include "domain/error.hpp"

#include <utility>

namespace music_service::domain {

std::string_view to_string(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::validation_failed:
        return "VALIDATION_FAILED";
    case ErrorCode::unknown_provider:
        return "UNKNOWN_PROVIDER";
    case ErrorCode::not_found:
        return "NOT_FOUND";
    case ErrorCode::conflict:
        return "CONFLICT";
    case ErrorCode::invalid_transition:
        return "INVALID_TRANSITION";
    case ErrorCode::version_conflict:
        return "VERSION_CONFLICT";
    case ErrorCode::task_finalizing:
        return "TASK_FINALIZING";
    case ErrorCode::task_terminal:
        return "TASK_TERMINAL";
    case ErrorCode::capability_unsupported:
        return "CAPABILITY_UNSUPPORTED";
    case ErrorCode::provider_disabled:
        return "PROVIDER_DISABLED";
    case ErrorCode::context_required:
        return "CONTEXT_REQUIRED";
    case ErrorCode::quality_unavailable:
        return "QUALITY_UNAVAILABLE";
    case ErrorCode::preview_rejected:
        return "PREVIEW_REJECTED";
    case ErrorCode::credentials_required:
        return "CREDENTIALS_REQUIRED";
    case ErrorCode::resource_expired:
        return "RESOURCE_EXPIRED";
    case ErrorCode::upstream_failed:
        return "UPSTREAM_FAILED";
    case ErrorCode::upstream_timeout:
        return "UPSTREAM_TIMEOUT";
    case ErrorCode::upstream_rate_limited:
        return "UPSTREAM_RATE_LIMITED";
    case ErrorCode::deadline_exceeded:
        return "REQUEST_DEADLINE_EXCEEDED";
    case ErrorCode::canceled:
        return "CANCELED";
    case ErrorCode::storage_unavailable:
        return "STORAGE_UNAVAILABLE";
    case ErrorCode::destination_unavailable:
        return "DESTINATION_UNAVAILABLE";
    case ErrorCode::destination_root_changed:
        return "DESTINATION_ROOT_CHANGED";
    case ErrorCode::destination_in_use:
        return "DESTINATION_IN_USE";
    case ErrorCode::idempotency_conflict:
        return "IDEMPOTENCY_CONFLICT";
    case ErrorCode::active_duplicate:
        return "ACTIVE_DUPLICATE";
    case ErrorCode::queue_limit_exceeded:
        return "QUEUE_LIMIT_EXCEEDED";
    case ErrorCode::instance_locked:
        return "INSTANCE_LOCKED";
    case ErrorCode::file_system:
        return "FILE_SYSTEM_ERROR";
    case ErrorCode::internal:
        return "INTERNAL_ERROR";
    }
    return "INTERNAL_ERROR";
}

Error Error::validation(std::string field, std::string reason) {
    Error error{ErrorCode::validation_failed, "request is invalid", false, {}};
    error.details.push_back({std::move(field), std::move(reason)});
    return error;
}

} // namespace music_service::domain
