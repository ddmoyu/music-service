#pragma once

#include "application/ports/providers/external_helper.hpp"
#include "domain/result.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Helper line protocol, version 1. Every message is one JSON object on one line of
// UTF-8; stdout carries nothing else, diagnostics go to stderr.
//
//   helper  -> service  {"type":"hello","protocol":1,"implementation":..,
//                        "implementation_version":..,"tool":..,"tool_version":..,
//                        "operations":[..]}
//   service -> helper   {"type":"request","protocol":1,"request_id":..,"operation":..,
//                        "budget":{"timeout_ms":..,"max_connections":..,"max_response_bytes":..},
//                        "scope":{"extractors":[..],"domains":[..]},"input":{..}}
//   helper  -> service  {"type":"result","request_id":..,"result":{..}}
//                    or {"type":"error","request_id":..,"error":{"code":..,"message":..,
//                        "retry_after_seconds":..}}
//   service -> helper   {"type":"cancel","request_id":..}   then stdin closes
//
// One process serves one request and exits after its reply.
namespace music_service::providers::external {

namespace ports = application::ports;

// What the adapter configuration allows; results outside it are refused.
struct HelperScope {
    std::vector<std::string> extractors;
    std::vector<std::string> domains;
};

struct HelperBudget {
    std::chrono::milliseconds timeout{0};
    std::uint32_t max_connections{1};
    std::uint64_t max_response_bytes{0};
};

// Longest helper-supplied text kept in an error message.
inline constexpr std::size_t max_helper_message = 200;
// Latest expiry accepted from a helper; later values are shortened to it.
inline constexpr std::chrono::hours max_helper_url_lifetime{24};

// host equals a listed domain or is one of its subdomains.
[[nodiscard]] bool host_in_scope(std::string_view host, const std::vector<std::string>& domains);

// A link the service may hand to the helper: https, no user info, host in scope.
domain::Result<void> check_link(std::string_view link, const HelperScope& scope);

domain::Result<ports::HelperIdentity> parse_hello(std::string_view line);

std::string encode_request(std::string_view request_id, std::string_view operation, const HelperBudget& budget,
                           const HelperScope& scope, const ports::HelperExtractRequest& input);
std::string encode_cancel(std::string_view request_id);

// A reply to request_id: the result object as JSON text, or the helper's error mapped
// to a service error (sanitized message, retry_after_seconds in retry_after).
struct HelperReply {
    std::optional<std::string> result_json;
    std::optional<domain::Error> error;
    std::optional<std::chrono::seconds> retry_after;
};
domain::Result<HelperReply> parse_reply(std::string_view line, std::string_view request_id);

// Re-checks an extract_audio result against the scope: https URL on an allowed host,
// declared extractor, a stable media id, well-formed headers with the sensitive ones
// moved to secret_headers and no transport-controlled ones, and an expiry that is
// still in the future.
domain::Result<ports::HelperAudioLocation> parse_audio_location(std::string_view result_json, const HelperScope& scope,
                                                                std::chrono::system_clock::time_point now);

// Printable, length-capped and scrubbed of URLs and bearer tokens.
[[nodiscard]] std::string sanitize_diagnostic(std::string_view text, std::size_t max_bytes);

} // namespace music_service::providers::external
