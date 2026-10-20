#pragma once

#include "application/ports/runtime/admission.hpp"
#include "application/ports/runtime/clock.hpp"

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace music_service::runtime {

struct AdmissionLimits {
    std::uint32_t total_connections{16}; // upstream.max_total_connections
    std::uint32_t per_principal{8};      // upstream.per_principal
    std::uint32_t per_origin{4};         // upstream.per_origin
    // Per source and traffic class: short/auth from upstream.per_source_short,
    // download from downloads.per_source, media from media_limits.upstream.
    std::map<application::ports::TrafficClass, std::uint32_t> per_source{
        {application::ports::TrafficClass::short_request, 2},
        {application::ports::TrafficClass::auth, 2},
        {application::ports::TrafficClass::download, 2},
        {application::ports::TrafficClass::media, 6},
    };
    // Deferred.retry_at for capacity limits; releases happen at unknown times.
    std::chrono::milliseconds capacity_retry_hint{200};
    // Upper bound applied to upstream Retry-After values.
    std::chrono::seconds max_cooldown{300};
};

// The process-wide upstream quota ledger (runtime-and-tasks.md, 上游入场与租约).
// try_acquire reserves every dimension at once or none: global connections,
// principal, source per traffic class, origin, and any cooldown of the source or its
// rate-limit group. It never blocks; without capacity it returns Deferred. A request
// larger than any single limit can never be granted and is VALIDATION_FAILED instead.
// Leases keep the ledger's state alive, so a lease may be released after the ledger
// object is gone. Thread-safe.
class AdmissionLedger final : public application::ports::UpstreamAdmissionPort {
public:
    AdmissionLedger(AdmissionLimits limits, std::shared_ptr<const application::ports::Clock> clock);

    domain::Result<application::ports::AdmissionOutcome>
    try_acquire(const application::ports::AdmissionRequest& request) override;
    std::optional<application::ports::Deferred>
    readiness(const application::ports::AdmissionRequest& request) const override;
    void feedback(const application::ports::AdmissionRequest& request,
                  std::optional<std::chrono::steady_clock::time_point> retry_after) override;

    struct Usage {
        std::uint32_t total{0};
        std::map<std::string, std::uint32_t> principals;
        std::map<std::string, std::uint32_t> sources; // "source/class"
        std::map<std::string, std::uint32_t> origins;
        std::map<std::string, std::chrono::steady_clock::time_point> cooldowns;
    };
    // For admin/status and tests.
    [[nodiscard]] Usage usage() const;

private:
    struct State;

    std::shared_ptr<State> state_;
    std::shared_ptr<const application::ports::Clock> clock_;
};

} // namespace music_service::runtime
