#pragma once

#include <string>
#include <string_view>

namespace music_service::infrastructure::observability {

inline constexpr std::string_view redacted = "[REDACTED]";

// Field names whose values are always secret: authorization, cookies, tokens,
// passwords, API keys, signatures, sessions and media tickets (case-insensitive
// substring match). Names ending in _id or _version are references, not secrets.
[[nodiscard]] bool is_sensitive_key(std::string_view key) noexcept;

// Keeps scheme, host and path; drops user info, query and fragment, which may carry
// signed parameters or tickets. A media ticket path segment is masked as well.
[[nodiscard]] std::string redact_url(std::string_view url);

// Value-level scrub for free text: masks "Bearer <token>" and URLs.
[[nodiscard]] std::string redact_text(std::string_view text);

// Applies the rules for a log field: sensitive keys are fully masked, URL-like keys go
// through redact_url and other text through redact_text.
[[nodiscard]] std::string redact_field(std::string_view key, std::string_view value);

} // namespace music_service::infrastructure::observability
