#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace music_service::domain {

// Stable error categories shared across modules. to_string() yields the code used
// by the public API; adapters map codes to HTTP status, the domain does not.
enum class ErrorCode {
    validation_failed,
    unknown_provider,
    not_found,
    conflict,
    invalid_transition,
    version_conflict,
    task_finalizing,
    task_terminal,
    capability_unsupported,
    provider_disabled,
    context_required,
    quality_unavailable,
    preview_rejected,
    credentials_required,
    resource_expired,
    upstream_failed,
    upstream_timeout,
    upstream_rate_limited,
    deadline_exceeded,
    canceled,
    storage_unavailable,
    destination_unavailable,
    instance_locked,
    file_system,
    internal,
};

std::string_view to_string(ErrorCode code) noexcept;

struct ErrorDetail {
    std::string field;
    std::string reason;

    friend bool operator==(const ErrorDetail&, const ErrorDetail&) = default;
};

// Structured, sanitized error. Messages must never contain secrets.
struct Error {
    ErrorCode code{ErrorCode::internal};
    std::string message;
    bool retryable{false};
    std::vector<ErrorDetail> details;

    static Error validation(std::string field, std::string reason);
};

} // namespace music_service::domain
