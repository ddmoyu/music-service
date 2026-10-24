#include "application/transport/upstream_client.hpp"

#include "application/transport/url.hpp"
#include "application/transport/http_rules.hpp"

#include <charconv>
#include <chrono>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace music_service::application::transport {

namespace {

// Longer than any cooldown the admission ledger keeps, and small enough that adding
// it to the steady clock cannot overflow.
constexpr std::chrono::seconds max_retry_after = std::chrono::hours{24 * 365};

} // namespace

std::optional<std::chrono::seconds> parse_retry_after(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    // Unsigned, so a sign is rejected rather than parsed.
    unsigned long long seconds = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), seconds);
    if (end != text.data() + text.size()) {
        return std::nullopt;
    }
    // A value too large to represent still cools down, capped before it reaches the clock.
    if (ec == std::errc::result_out_of_range ||
        (ec == std::errc{} && seconds > static_cast<unsigned long long>(max_retry_after.count()))) {
        return max_retry_after;
    }
    if (ec != std::errc{}) {
        return std::nullopt;
    }
    return std::chrono::seconds{static_cast<std::chrono::seconds::rep>(seconds)};
}

std::optional<std::chrono::seconds> retry_after(const ports::HttpResponseHead& head) {
    const auto value = head.header("Retry-After");
    if (!value) {
        return std::nullopt;
    }
    return parse_retry_after(*value);
}

UpstreamClient::UpstreamClient(std::shared_ptr<ports::UpstreamAdmissionPort> admission,
                               std::shared_ptr<ports::HttpTransport> transport,
                               std::shared_ptr<const ports::Clock> clock, TrafficTimeouts timeouts)
    : admission_(std::move(admission)), transport_(std::move(transport)), clock_(std::move(clock)), timeouts_(timeouts) {}

void UpstreamClient::report(const ports::AdmissionRequest& admitted, const ports::HttpResponseHead& head) {
    if (head.status != 429 && head.status != 503) {
        return;
    }
    if (const auto delay = retry_after(head)) {
        admission_->feedback(admitted, clock_->steady_now() + *delay);
    }
}

template <class T, class Call>
domain::Result<HopResult<T>> UpstreamClient::run(ports::HttpRequest request, const ports::AdmissionRequest& base,
                                                 const ports::OperationContext& context, Call&& call) {
    if (base.traffic_class == ports::TrafficClass::download) {
        request.connect_timeout = timeouts_.download_connect;
        request.idle_timeout = timeouts_.download_idle;
    } else if (base.traffic_class == ports::TrafficClass::media) {
        request.connect_timeout = timeouts_.media_connect;
        request.idle_timeout = timeouts_.media_idle;
    }
    while (true) {
        auto url = Url::parse(request.url);
        if (!url) {
            return std::move(url).error();
        }
        auto admitted = base;
        admitted.origin = url->origin();
        admitted.deadline = context.deadline;

        auto granted = admission_->try_acquire(admitted);
        if (!granted) {
            return std::move(granted).error();
        }
        if (auto* deferred = std::get_if<ports::Deferred>(&granted.value())) {
            return HopResult<T>{PendingHop{std::move(request), *deferred}};
        }
        auto& lease = std::get<ports::UpstreamLease>(granted.value());

        auto result = call(request, lease, context);
        // The transport returns only after its I/O ended; the lease goes back now,
        // before any next hop asks for quota.
        lease.release();
        if (!result) {
            return std::move(result).error();
        }
        const ports::HttpResponseHead* head = nullptr;
        if constexpr (std::is_same_v<T, ports::HttpResponse>) {
            head = &result->head;
        } else {
            head = &result.value();
        }
        report(admitted, *head);
        if (!head->redirect) {
            return HopResult<T>{std::move(result).value()};
        }
        if (!request.allow_redirects) {
            return transport_error("redirect_rejected", "redirects are disabled for this request", false);
        }
        request = *head->redirect;
    }
}

domain::Result<HopResult<ports::HttpResponse>> UpstreamClient::fetch(ports::HttpRequest request,
                                                                     const ports::AdmissionRequest& base,
                                                                     const ports::OperationContext& context) {
    return run<ports::HttpResponse>(
        std::move(request), base, context,
        [this](const ports::HttpRequest& r, const ports::UpstreamLease& lease, const ports::OperationContext& c) {
            return transport_->request(r, lease, c);
        });
}

domain::Result<HopResult<ports::HttpResponseHead>>
UpstreamClient::stream(ports::HttpRequest request, const ports::AdmissionRequest& base,
                       const ports::OperationContext& context, const ports::HeadConsumer& on_head,
                       const ports::ChunkConsumer& on_chunk) {
    return run<ports::HttpResponseHead>(
        std::move(request), base, context,
        [&](const ports::HttpRequest& r, const ports::UpstreamLease& lease, const ports::OperationContext& c) {
            return transport_->receive(r, lease, c, on_head, on_chunk);
        });
}

} // namespace music_service::application::transport
