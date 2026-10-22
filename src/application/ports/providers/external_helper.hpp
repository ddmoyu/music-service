#pragma once

#include "application/ports/runtime/admission.hpp"
#include "application/ports/runtime/operation_context.hpp"
#include "application/ports/transport/http_transport.hpp"
#include "domain/result.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace music_service::application::ports {

// Optional external helpers (provider-extensibility.md, 外部执行契约): a fixed program
// the administrator installed, run as one bounded process per call and spoken to over
// a versioned line protocol. The helper only extracts; the service's own transport
// moves the audio. Helpers own no queue, database or published file.

inline constexpr std::uint32_t helper_protocol_version = 1;

// Protocol operations a helper may offer. Only extract_audio exists in version 1.
inline constexpr std::string_view helper_operation_extract_audio = "extract_audio";

[[nodiscard]] inline bool is_known_helper_operation(std::string_view operation) noexcept {
    return operation == helper_operation_extract_audio;
}

// Who is calling and for which source. adapter names an external_adapters.adapters
// entry bound to base.source. base carries principal, source, operation, traffic
// class, identity and rate-limit group; the runner sets origin, connections, the
// helper process and the deadline itself.
struct HelperCall {
    std::string adapter;
    AdmissionRequest base;
};

// extract_audio input. The link travels as protocol data, never on the command line,
// and must lie within the adapter's domains.
struct HelperExtractRequest {
    std::string link;
};

// One independent HTTP audio stream the helper found, already checked against the
// adapter's declared scope. headers are plain request headers for that URL;
// secret_headers (cookies, authorization) are kept apart as HttpRequest requires.
struct HelperAudioLocation {
    std::string url;
    std::vector<HttpHeader> headers;
    std::vector<HttpHeader> secret_headers;
    std::optional<std::chrono::system_clock::time_point> expires_at;
    std::string extractor;
    std::string media_id;  // the extractor's stable id of the item
    std::string container; // e.g. m4a, webm; empty when the helper does not know
    std::string codec;
    std::optional<std::uint32_t> bitrate_kbps;
    std::optional<std::uint64_t> size_bytes;
    std::optional<std::uint64_t> duration_ms;
};

// What the helper reported about itself in its hello.
struct HelperIdentity {
    std::uint32_t protocol{0};
    std::string implementation;
    std::string implementation_version;
    std::string tool;
    std::string tool_version;
    std::vector<std::string> operations;
};

template <class T> using HelperOutcome = std::variant<T, Deferred>;

// Implemented by providers::external::HelperRunner. Calls block the calling worker
// until the helper process has really exited; Deferred means no helper process or
// connection quota was free and nothing was started.
class ExternalHelper {
public:
    virtual ~ExternalHelper() = default;

    virtual domain::Result<HelperOutcome<HelperAudioLocation>>
    extract_audio(const HelperCall& call, const HelperExtractRequest& request, const OperationContext& context) = 0;
};

} // namespace music_service::application::ports
