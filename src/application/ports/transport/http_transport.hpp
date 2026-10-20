#pragma once

#include "application/ports/runtime/admission.hpp"
#include "application/ports/runtime/operation_context.hpp"
#include "domain/result.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace music_service::application::ports {

enum class HttpMethod { get, head, post };

struct HttpHeader {
    std::string name;
    std::string value;
};

struct HttpRequest {
    HttpMethod method{HttpMethod::get};
    std::string url; // validated target; arbitrary user URLs are never accepted
    std::vector<HttpHeader> headers;
    std::string body;
    std::size_t max_response_bytes{0}; // bounded responses only; 0 uses upstream.max_response
};

struct HttpResponseHead {
    int status{0};
    std::vector<HttpHeader> headers;
    std::string final_url; // after allowed redirects

    // Case-insensitive lookup of the first header with this name.
    [[nodiscard]] std::optional<std::string> header(std::string_view name) const {
        const auto lower = [](char c) {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        };
        for (const auto& item : headers) {
            if (item.name.size() == name.size() &&
                std::equal(item.name.begin(), item.name.end(), name.begin(),
                           [&](char a, char b) { return lower(a) == lower(b); })) {
                return item.value;
            }
        }
        return std::nullopt;
    }
};

struct HttpResponse {
    HttpResponseHead head;
    std::string body;
};

// Called once with the final response head; returning false stops before any body is read.
using HeadConsumer = std::function<bool(const HttpResponseHead&)>;
// Called per received chunk; the span is only valid during the call. Returning false stops.
using ChunkConsumer = std::function<bool(std::span<const char> chunk)>;

// Upstream HTTP(S). Every call needs a lease that matches the target; redirects to
// another origin are reported back for new admission instead of being followed with
// the old lease. No call queues internally.
class HttpTransport {
public:
    virtual ~HttpTransport() = default;

    virtual domain::Result<HttpResponse> request(const HttpRequest& request,
                                                 const UpstreamLease& lease,
                                                 const OperationContext& context) = 0;

    virtual domain::Result<HttpResponseHead> receive(const HttpRequest& request,
                                                     const UpstreamLease& lease,
                                                     const OperationContext& context,
                                                     const HeadConsumer& on_head,
                                                     const ChunkConsumer& on_chunk) = 0;
};

} // namespace music_service::application::ports
