#include "providers/kuwo/client.hpp"

#include <charconv>
#include <utility>

namespace music_service::providers::kuwo {

using domain::Error;
using domain::ErrorCode;

Error http_status_error(const ports::HttpResponseHead& head) {
    const auto status = head.status;
    if (status == 429) {
        // The upstream client already fed Retry-After to admission; pass it on as a hint.
        Error error{ErrorCode::upstream_rate_limited, "kuwo is limiting requests", true, {}};
        error.details.push_back({"http_status", "429"});
        if (auto wait = head.header("Retry-After"); wait && !wait->empty() &&
                                                    wait->find_first_not_of("0123456789") == std::string::npos) {
            error.details.push_back({"retry_after_seconds", std::move(*wait)});
        }
        return error;
    }
    Error error{ErrorCode::upstream_failed, "kuwo answered with an unexpected status", status >= 500, {}};
    error.details.push_back({"http_status", std::to_string(status)});
    return error;
}

Client::Client(const ports::ProviderSetup& setup)
    : source_(setup.source), upstream_(setup.upstream), rate_limit_group_(setup.rate_limit_group) {}

domain::Result<ports::HttpResponse> Client::fetch(ports::HttpRequest request,
                                                  const ports::ProviderContext& context) const {
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
    return std::get<ports::HttpResponse>(std::move(result).value());
}

domain::Result<std::string> Client::body(ports::HttpRequest request, const ports::ProviderContext& context) const {
    auto response = fetch(std::move(request), context);
    if (!response) {
        return std::move(response).error();
    }
    if (response->head.status != 200) {
        return http_status_error(response->head);
    }
    return std::move(response->body);
}

domain::Result<void> require_upstream(const ports::ProviderSetup& setup) {
    if (!setup.upstream) {
        return Error{ErrorCode::internal, "kuwo needs the upstream client", false, {}};
    }
    return domain::ok();
}

domain::Result<std::uint32_t> page_cursor(const std::optional<std::string>& cursor) {
    if (!cursor) {
        return std::uint32_t{0};
    }
    std::uint32_t page = 0;
    const auto [end, ec] = std::from_chars(cursor->data(), cursor->data() + cursor->size(), page);
    if (cursor->empty() || ec != std::errc{} || end != cursor->data() + cursor->size() || page > 10000) {
        return Error{ErrorCode::cursor_mismatch, "cursor is not a kuwo page", false, {}};
    }
    return page;
}

} // namespace music_service::providers::kuwo
