#include "runtime/admission/admission_ledger.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

namespace music_service::runtime {

namespace ports = application::ports;
using SteadyTime = std::chrono::steady_clock::time_point;

namespace {

std::string_view class_name(ports::TrafficClass traffic) {
    switch (traffic) {
    case ports::TrafficClass::short_request:
        return "short";
    case ports::TrafficClass::download:
        return "download";
    case ports::TrafficClass::media:
        return "media";
    case ports::TrafficClass::auth:
        return "auth";
    }
    return "short";
}

std::string source_key(const ports::AdmissionRequest& r) {
    return r.source.value() + "/" + std::string{class_name(r.traffic_class)};
}

// A shared application quota group cools down as a whole, regardless of which source
// or token version hit the limit.
std::string cooldown_key(const ports::AdmissionRequest& r) {
    return r.rate_limit_group.empty() ? "source:" + r.source.value() : "group:" + r.rate_limit_group;
}

} // namespace

struct AdmissionLedger::State {
    AdmissionLimits limits;
    mutable std::mutex mutex;
    Usage usage;
    std::uint64_t next_id{0};

    std::uint32_t per_source_limit(ports::TrafficClass traffic) const {
        const auto it = limits.per_source.find(traffic);
        return it == limits.per_source.end() ? 1U : it->second;
    }

    // The first dimension whose limit alone is smaller than the request: such a request
    // could never be granted, so it is rejected instead of deferred forever. Limits are
    // fixed after construction; no lock needed.
    std::optional<std::string_view> unsatisfiable(const ports::AdmissionRequest& r) const {
        if (r.connections > limits.total_connections) {
            return "total";
        }
        if (r.connections > limits.per_principal) {
            return "principal";
        }
        if (r.connections > per_source_limit(r.traffic_class)) {
            return "source";
        }
        if (r.connections > limits.per_origin) {
            return "origin";
        }
        if (r.helper_processes > limits.helper_processes) {
            return "helper";
        }
        return std::nullopt;
    }

    // Caller holds the mutex.
    std::optional<ports::Deferred> blocked(const ports::AdmissionRequest& r, SteadyTime now) const {
        if (const auto it = usage.cooldowns.find(cooldown_key(r));
            it != usage.cooldowns.end() && it->second > now) {
            return ports::Deferred{"cooldown", it->second};
        }
        const auto later = now + limits.capacity_retry_hint;
        // Compared in 64 bits: a huge request must not wrap the sum back under a limit.
        const auto fits = [n = std::uint64_t{r.connections}](std::uint32_t used, std::uint32_t limit) {
            return used + n <= limit;
        };
        const auto count = [](const std::map<std::string, std::uint32_t>& m, const std::string& k) {
            const auto it = m.find(k);
            return it == m.end() ? 0U : it->second;
        };
        const auto per_source = per_source_limit(r.traffic_class);
        if (!fits(usage.total, limits.total_connections)) {
            return ports::Deferred{"total", later};
        }
        if (!fits(count(usage.principals, r.principal_id), limits.per_principal)) {
            return ports::Deferred{"principal", later};
        }
        if (!fits(count(usage.sources, source_key(r)), per_source)) {
            return ports::Deferred{"source", later};
        }
        if (!fits(count(usage.origins, r.origin), limits.per_origin)) {
            return ports::Deferred{"origin", later};
        }
        if (std::uint64_t{usage.helpers} + r.helper_processes > limits.helper_processes) {
            return ports::Deferred{"helper", later};
        }
        return std::nullopt;
    }

    void add(const std::string& principal, const std::string& source, const std::string& origin,
             std::uint32_t n, std::uint32_t helpers) {
        usage.total += n;
        usage.helpers += helpers;
        usage.principals[principal] += n;
        usage.sources[source] += n;
        usage.origins[origin] += n;
    }

    void remove(const std::string& principal, const std::string& source, const std::string& origin,
                std::uint32_t n, std::uint32_t helpers) {
        usage.total -= n;
        usage.helpers -= helpers;
        const auto drop = [n](std::map<std::string, std::uint32_t>& m, const std::string& k) {
            if (auto it = m.find(k); it != m.end()) {
                it->second -= n;
                if (it->second == 0) {
                    m.erase(it);
                }
            }
        };
        drop(usage.principals, principal);
        drop(usage.sources, source);
        drop(usage.origins, origin);
    }
};

AdmissionLedger::AdmissionLedger(AdmissionLimits limits,
                                 std::shared_ptr<const ports::Clock> clock)
    : state_(std::make_shared<State>()), clock_(std::move(clock)) {
    state_->limits = std::move(limits);
}

domain::Result<ports::AdmissionOutcome>
AdmissionLedger::try_acquire(const ports::AdmissionRequest& request) {
    if (request.connections == 0 || request.principal_id.empty() || request.origin.empty()) {
        return domain::Error::validation("admission", "principal, origin and connections are required");
    }
    if (request.helper_processes > 0 && state_->limits.helper_processes == 0) {
        return domain::Error{domain::ErrorCode::capability_unsupported,
                             "external helper processes are not enabled", false, {}};
    }
    if (const auto dimension = state_->unsatisfiable(request)) {
        return domain::Error::validation("admission", "the request exceeds the " + std::string{*dimension} +
                                                          " limit and can never be granted");
    }
    const auto now = clock_->steady_now();
    if (request.deadline <= now) {
        return domain::Error{domain::ErrorCode::deadline_exceeded,
                             "deadline passed before admission", false, {}};
    }

    const std::lock_guard lock{state_->mutex};
    if (auto deferred = state_->blocked(request, now)) {
        return ports::AdmissionOutcome{std::move(*deferred)};
    }
    const auto id = ++state_->next_id;
    const auto source = source_key(request);
    state_->add(request.principal_id, source, request.origin, request.connections, request.helper_processes);

    // The releaser owns a reference to the state: returning quota stays safe even if
    // the ledger is destroyed while operations are still finishing.
    auto releaser = [state = state_, principal = request.principal_id, source,
                     origin = request.origin, n = request.connections,
                     helpers = request.helper_processes](std::uint64_t) {
        const std::lock_guard release_lock{state->mutex};
        state->remove(principal, source, origin, n, helpers);
    };
    return ports::AdmissionOutcome{ports::UpstreamLease{id, request, std::move(releaser)}};
}

std::optional<ports::Deferred> AdmissionLedger::readiness(const ports::AdmissionRequest& request) const {
    const auto now = clock_->steady_now();
    const std::lock_guard lock{state_->mutex};
    return state_->blocked(request, now);
}

void AdmissionLedger::feedback(const ports::AdmissionRequest& request,
                               std::optional<SteadyTime> retry_after) {
    if (!retry_after) {
        return;
    }
    const auto now = clock_->steady_now();
    const auto until = std::min(*retry_after, now + state_->limits.max_cooldown);
    if (until <= now) {
        return;
    }
    const std::lock_guard lock{state_->mutex};
    auto& cooldown = state_->usage.cooldowns[cooldown_key(request)];
    // Never shorten a cooldown another response already established.
    cooldown = std::max(cooldown, until);
}

AdmissionLedger::Usage AdmissionLedger::usage() const {
    const auto now = clock_->steady_now();
    const std::lock_guard lock{state_->mutex};
    auto copy = state_->usage;
    std::erase_if(copy.cooldowns, [now](const auto& entry) { return entry.second <= now; });
    return copy;
}

} // namespace music_service::runtime
