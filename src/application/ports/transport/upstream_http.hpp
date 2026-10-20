#pragma once

#include "application/ports/runtime/admission.hpp"
#include "application/ports/runtime/operation_context.hpp"
#include "application/ports/transport/http_transport.hpp"
#include "domain/result.hpp"

#include <variant>

namespace music_service::application::ports {

// A hop that could not be admitted now. next is safe to keep: it holds no secrets
// beyond what its own origin may receive. The caller re-plans or persists the delay
// and later calls again with next; nothing is held meanwhile.
struct PendingHop {
    HttpRequest next;
    Deferred deferred;
};

template <class T> using HopResult = std::variant<T, PendingHop>;

// Upstream HTTP as platform packages use it: every hop is admitted by the shared
// admission ledger first (never blocking) and then run by the HttpTransport.
// Implemented by application::transport::UpstreamClient.
class UpstreamHttp {
public:
    virtual ~UpstreamHttp() = default;

    // base describes the operation (principal, source, operation, traffic class,
    // identity, rate-limit group, connections); origin and deadline are set per hop.
    virtual domain::Result<HopResult<HttpResponse>> fetch(HttpRequest request, const AdmissionRequest& base,
                                                          const OperationContext& context) = 0;

    virtual domain::Result<HopResult<HttpResponseHead>> stream(HttpRequest request, const AdmissionRequest& base,
                                                               const OperationContext& context,
                                                               const HeadConsumer& on_head,
                                                               const ChunkConsumer& on_chunk) = 0;
};

} // namespace music_service::application::ports
