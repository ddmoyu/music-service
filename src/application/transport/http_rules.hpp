#pragma once

#include "application/ports/transport/http_transport.hpp"
#include "application/transport/url.hpp"
#include "domain/result.hpp"

#include <cstdint>
#include <string_view>

namespace music_service::application::transport {

// Target and header rules every hop must pass (runtime-and-tasks.md, httplib 适配规则).
// Kept free of any HTTP library so they are tested on their own.

// Cookie, Authorization and Proxy-Authorization, case-insensitively.
[[nodiscard]] bool is_sensitive_header(std::string_view name) noexcept;

[[nodiscard]] bool is_redirect_status(int status) noexcept;

// The registration a plaintext URL matches for this operation, if any.
[[nodiscard]] const ports::PlaintextEndpoint* find_plaintext_endpoint(const ports::HttpRequest& request,
                                                                      const Url& url) noexcept;

// Before any I/O: the URL parses; sensitive headers appear only in secret_headers;
// header names and values cannot split lines; plain http is allowed only for a
// registered endpoint and then carries no secrets. VALIDATION_FAILED otherwise.
domain::Result<Url> check_request(const ports::HttpRequest& request);

// The next request for a redirect response. 303, and 301/302 after POST, become GET
// without a body; 307/308 keep method and body. Secret headers survive only within
// the same origin. HTTPS to HTTP only reaches an endpoint registered with
// downgrade_redirect for the same operation. More than redirect_max hops in total,
// or a rejected target, is UPSTREAM_FAILED with the reason in details.
domain::Result<ports::HttpRequest> next_redirect(const ports::HttpRequest& current, const Url& current_url,
                                                 int status, std::string_view location,
                                                 std::uint32_t redirect_max);

// UPSTREAM_FAILED carrying details {"transport", category}.
domain::Error transport_error(std::string category, std::string message, bool retryable);

} // namespace music_service::application::transport
