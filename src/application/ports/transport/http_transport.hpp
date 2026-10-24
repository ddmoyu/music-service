#pragma once

#include "application/ports/runtime/admission.hpp"
#include "application/ports/runtime/operation_context.hpp"
#include "domain/result.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
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

// A plaintext HTTP endpoint a platform package registered because it has no HTTPS
// equivalent: exact host and port, path prefix and operation. Plaintext requests never carry
// secrets. downgrade_redirect additionally lets an HTTPS response redirect to it
// (audio CDN downloads only); secrets are stripped on that hop.
struct PlaintextEndpoint {
    std::string host{}; // lowercase
    std::uint16_t port{80};
    std::string path_prefix{};
    std::string operation{};
    bool downgrade_redirect{false};
};

struct HttpRequest {
    HttpMethod method{HttpMethod::get};
    std::string url{}; // absolute http(s) URL; arbitrary user URLs are never accepted
    // Must not contain Cookie, Authorization or Proxy-Authorization: those go to
    // secret_headers so they are never logged, never sent in plaintext and never
    // carried to another origin.
    std::vector<HttpHeader> headers{};
    std::vector<HttpHeader> secret_headers{};
    std::string body{};
    std::string operation{};                              // names the call, for plaintext matching
    std::vector<PlaintextEndpoint> plaintext_endpoints{}; // the platform's registrations
    std::size_t max_response_bytes{0};   // request(): 0 uses upstream.max_response
    std::uint32_t redirects_followed{0}; // hops taken by earlier legs of this call
    // Opt in to encoded wire bytes. The caller must decode with its own output limit;
    // Content-Encoding and Content-Length retain their original wire meaning.
    bool preserve_content_encoding{false};
    // Set by the service's traffic-class policy; redirects retain these limits.
    std::optional<std::chrono::milliseconds> connect_timeout{};
    std::optional<std::chrono::milliseconds> idle_timeout{};
    // Token exchanges must not forward a confidential POST body through redirects.
    bool allow_redirects{true};
};

struct HttpResponseHead {
    int status{0};
    std::vector<HttpHeader> headers{};
    std::string final_url{}; // the URL that produced this response
    // Set when the response redirects to another origin. The transport does not follow
    // it with the current lease: the caller admits the next request separately. It has
    // the method rewritten as the status requires and no secret headers.
    std::shared_ptr<const HttpRequest> redirect{};
    // receive(): a consumer returned false, so the body was not read to the end.
    bool stopped_by_consumer{false};

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

// Upstream HTTP(S). Every call needs an active lease for the target's origin; it
// follows redirects within that origin (up to upstream.redirect_max hops in total)
// and reports a redirect to another origin back in HttpResponseHead::redirect. HTTP
// error statuses are responses, not errors; errors are transport failures, with the
// category in Error::details ("transport"). No call queues internally.
class HttpTransport {
public:
    virtual ~HttpTransport() = default;

    // Bounded response: larger bodies fail instead of being truncated.
    virtual domain::Result<HttpResponse> request(const HttpRequest& request,
                                                 const UpstreamLease& lease,
                                                 const OperationContext& context) = 0;

    // Streams the body to on_chunk without buffering it. on_head sees the final
    // (non-redirect) head first; redirects to another origin return without calling
    // either consumer.
    virtual domain::Result<HttpResponseHead> receive(const HttpRequest& request,
                                                     const UpstreamLease& lease,
                                                     const OperationContext& context,
                                                     const HeadConsumer& on_head,
                                                     const ChunkConsumer& on_chunk) = 0;
};

} // namespace music_service::application::ports
