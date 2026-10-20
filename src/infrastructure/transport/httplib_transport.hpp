#pragma once

#include "application/ports/transport/http_transport.hpp"
#include "domain/result.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace music_service::infrastructure::transport {

struct HttplibTransportOptions {
    std::chrono::seconds connect_timeout{3};  // upstream.connect_timeout
    std::chrono::seconds idle_timeout{30};    // read/write inactivity; always capped by the deadline
    std::size_t max_response_bytes{2 * 1024 * 1024}; // upstream.max_response
    std::uint32_t redirect_max{5};                   // upstream.redirect_max
    // PEM bundle that replaces the system trust store; empty uses the operating
    // system's roots (Windows certificate store, or the usual Linux CA files).
    std::optional<std::filesystem::path> ca_bundle;
    std::string user_agent{"music-service"};
};

// HttpTransport on cpp-httplib with Mbed TLS (runtime-and-tasks.md, httplib 适配规则).
// Each hop uses its own client and connection, closed afterwards, so connections are
// never shared across origins or identities. Certificate chain and host name are
// always verified. Cancellation through the context's stop token aborts the socket;
// the call returns only after the I/O has ended. Thread-safe.
class HttplibTransport final : public application::ports::HttpTransport {
public:
    // Loads and checks the CA bundle when one is configured.
    static domain::Result<std::shared_ptr<HttplibTransport>> create(HttplibTransportOptions options);

    domain::Result<application::ports::HttpResponse>
    request(const application::ports::HttpRequest& request, const application::ports::UpstreamLease& lease,
            const application::ports::OperationContext& context) override;

    domain::Result<application::ports::HttpResponseHead>
    receive(const application::ports::HttpRequest& request, const application::ports::UpstreamLease& lease,
            const application::ports::OperationContext& context, const application::ports::HeadConsumer& on_head,
            const application::ports::ChunkConsumer& on_chunk) override;

private:
    struct Sink;

    HttplibTransport(HttplibTransportOptions options, std::string ca_pem);

    domain::Result<application::ports::HttpResponseHead> perform(const application::ports::HttpRequest& request,
                                                                 const application::ports::UpstreamLease& lease,
                                                                 const application::ports::OperationContext& context,
                                                                 Sink& sink);

    HttplibTransportOptions options_;
    std::string ca_pem_; // empty: system roots
};

} // namespace music_service::infrastructure::transport
