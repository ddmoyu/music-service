#pragma once

#include "application/ports/providers/bundle.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace music_service::providers::kuwo {

namespace ports = application::ports;

// An unexpected HTTP status: 429 is UPSTREAM_RATE_LIMITED with any Retry-After as a
// hint, 5xx a retryable UPSTREAM_FAILED, anything else a final one.
domain::Error http_status_error(const ports::HttpResponseHead& head);

// Shared request path: the upstream client with the operation's admission request. A
// hop that cannot be admitted is SERVICE_BUSY (retryable, details {"admission", dimension}).
class Client {
public:
    explicit Client(const ports::ProviderSetup& setup);

    domain::Result<ports::HttpResponse> fetch(ports::HttpRequest request, const ports::ProviderContext& context) const;
    // A JSON call: anything but 200 is an error.
    domain::Result<std::string> body(ports::HttpRequest request, const ports::ProviderContext& context) const;

private:
    domain::SourceId source_;
    std::shared_ptr<ports::UpstreamHttp> upstream_;
    std::string rate_limit_group_;
};

domain::Result<void> require_upstream(const ports::ProviderSetup& setup);

// A page-number cursor: "<n>" for n in 0..10000; anything else is CURSOR_MISMATCH.
domain::Result<std::uint32_t> page_cursor(const std::optional<std::string>& cursor);

} // namespace music_service::providers::kuwo
