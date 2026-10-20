#pragma once

#include "application/ports/runtime/admission.hpp"
#include "application/ports/runtime/clock.hpp"
#include "application/ports/runtime/operation_context.hpp"
#include "application/ports/transport/http_transport.hpp"
#include "domain/result.hpp"

#include <memory>
#include <variant>

namespace music_service::application::transport {

// A hop that could not be admitted now. next is safe to keep: it holds no secrets
// beyond what its own origin may receive. The caller re-plans or persists the delay
// and later calls again with next; nothing is held meanwhile.
struct PendingHop {
    ports::HttpRequest next;
    ports::Deferred deferred;
};

template <class T> using HopResult = std::variant<T, PendingHop>;

// Runs one upstream call as a sequence of admitted hops: each origin gets its own
// lease from the admission port (never blocking), the transport follows redirects
// within that origin, and a redirect to another origin releases the old lease before
// the next one is requested. 429/503 Retry-After goes back to the admission port as
// cooldown feedback. Thread-safe if the ports are.
class UpstreamClient {
public:
    UpstreamClient(std::shared_ptr<ports::UpstreamAdmissionPort> admission,
                   std::shared_ptr<ports::HttpTransport> transport,
                   std::shared_ptr<const ports::Clock> clock);

    // base describes the operation (principal, source, operation, traffic class,
    // identity, rate-limit group, connections); origin and deadline are set per hop.
    domain::Result<HopResult<ports::HttpResponse>> fetch(ports::HttpRequest request,
                                                         const ports::AdmissionRequest& base,
                                                         const ports::OperationContext& context);

    domain::Result<HopResult<ports::HttpResponseHead>> stream(ports::HttpRequest request,
                                                              const ports::AdmissionRequest& base,
                                                              const ports::OperationContext& context,
                                                              const ports::HeadConsumer& on_head,
                                                              const ports::ChunkConsumer& on_chunk);

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
