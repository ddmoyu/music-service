#pragma once

#include "application/ports/runtime/admission.hpp"
#include "application/ports/runtime/clock.hpp"
#include "application/ports/runtime/operation_context.hpp"
#include "application/ports/transport/http_transport.hpp"
#include "application/ports/transport/upstream_http.hpp"
#include "domain/result.hpp"

#include <memory>
#include <variant>

namespace music_service::application::transport {

using ports::HopResult;
using ports::PendingHop;

// Runs one upstream call as a sequence of admitted hops: each origin gets its own
// lease from the admission port (never blocking), the transport follows redirects
// within that origin, and a redirect to another origin releases the old lease before
// the next one is requested. 429/503 Retry-After goes back to the admission port as
// cooldown feedback. Thread-safe if the ports are.
class UpstreamClient final : public ports::UpstreamHttp {
public:
    UpstreamClient(std::shared_ptr<ports::UpstreamAdmissionPort> admission,
                   std::shared_ptr<ports::HttpTransport> transport,
                   std::shared_ptr<const ports::Clock> clock);

    domain::Result<HopResult<ports::HttpResponse>> fetch(ports::HttpRequest request,
                                                         const ports::AdmissionRequest& base,
                                                         const ports::OperationContext& context) override;

    domain::Result<HopResult<ports::HttpResponseHead>> stream(ports::HttpRequest request,
                                                              const ports::AdmissionRequest& base,
                                                              const ports::OperationContext& context,
                                                              const ports::HeadConsumer& on_head,
                                                              const ports::ChunkConsumer& on_chunk) override;

private:
    template <class T, class Call>
    domain::Result<HopResult<T>> run(ports::HttpRequest request, const ports::AdmissionRequest& base,
                                     const ports::OperationContext& context, Call&& call);

    void report(const ports::AdmissionRequest& admitted, const ports::HttpResponseHead& head);

    std::shared_ptr<ports::UpstreamAdmissionPort> admission_;
    std::shared_ptr<ports::HttpTransport> transport_;
    std::shared_ptr<const ports::Clock> clock_;
};

} // namespace music_service::application::transport
