#pragma once

#include "application/ports/providers/bundle.hpp"
#include "providers/netease/protocol/requests.hpp"

namespace music_service::providers::netease {
namespace ports = application::ports;
using domain::Error;
using domain::ErrorCode;

protocol::Call call_for(std::string_view cookie = {});

inline Error http_status_error(const ports::HttpResponseHead& head) {
    if (head.status == 429) {
        Error error{ErrorCode::upstream_rate_limited, "netease is limiting requests", true, {}};
        error.details.push_back({"http_status", "429"});
        return error;
    }
    Error error{ErrorCode::upstream_failed, "netease answered with an unexpected status", head.status >= 500, {}};
    error.details.push_back({"http_status", std::to_string(head.status)});
    return error;
}

class Client {
public:
    explicit Client(const ports::ProviderSetup& setup)
        : source_(setup.source), upstream_(setup.upstream), rate_limit_group_(setup.rate_limit_group) {}

    // A weapi call whose answer must be HTTP 200; the body is returned as is.
    domain::Result<std::string> body(ports::HttpRequest request, const ports::ProviderContext& context,
                                     std::string_view cookie = {}) const {
        if (!cookie.empty()) {
            protocol::with_cookie(request, cookie);
        }
        ports::AdmissionRequest base{context.identity.binding().principal_id(),
                                     source_,
                                     request.operation,
                                     ports::TrafficClass::short_request,
                                     context.identity,
                                     {},
                                     rate_limit_group_,
                                     context.operation.deadline};
        auto result = upstream_->fetch(std::move(request), base, context.operation);
        if (!result) {
            return std::move(result).error();
        }
        if (auto* pending = std::get_if<ports::PendingHop>(&result.value())) {
            Error error{ErrorCode::service_busy, "upstream capacity is not available now", true, {}};
            error.details.push_back({"admission", pending->deferred.blocked_dimension});
            return error;
        }
        auto response = std::get<ports::HttpResponse>(std::move(result).value());
        if (response.head.status != 200) {
            return http_status_error(response.head);
        }
        return std::move(response.body);
    }

private:
    domain::SourceId source_;
    std::shared_ptr<ports::UpstreamHttp> upstream_;
    std::string rate_limit_group_;
};

} // namespace music_service::providers::netease
