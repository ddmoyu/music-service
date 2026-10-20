#pragma once

#include "domain/catalog/refs.hpp"
#include "domain/identity/account.hpp"
#include "domain/result.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace music_dl::application::ports {

enum class TrafficClass { short_request, download, media, auth };

// One concrete upstream operation asking for quota. S0 enforces the global,
// principal, source and origin dimensions; rate_limit_group and helper_processes are
// carried now and enforced once T24/T26 enable them.
struct AdmissionRequest {
    std::string principal_id;
    domain::SourceId source;
    std::string operation;
    TrafficClass traffic_class{TrafficClass::short_request};
    std::optional<domain::AuthIdentityKey> identity;
    std::string origin; // normalized scheme://host[:port]
    std::string rate_limit_group;
    std::chrono::steady_clock::time_point deadline;
    std::uint32_t connections{1};
    std::uint32_t helper_processes{0};
};

// Exclusive quota for one operation. Returned exactly once: by release() after the
// I/O has really finished, or by the destructor. Move-only.
class UpstreamLease {
public:
    using Releaser = std::function<void(std::uint64_t lease_id)>;

    UpstreamLease(std::uint64_t id, AdmissionRequest request, Releaser releaser)
        : id_(id), request_(std::move(request)), releaser_(std::move(releaser)) {}
    UpstreamLease(const UpstreamLease&) = delete;
    UpstreamLease& operator=(const UpstreamLease&) = delete;
    UpstreamLease(UpstreamLease&& other) noexcept
        : id_(other.id_), request_(std::move(other.request_)),
          releaser_(std::exchange(other.releaser_, nullptr)) {}
    UpstreamLease& operator=(UpstreamLease&& other) noexcept {
        if (this != &other) {
            release();
            id_ = other.id_;
            request_ = std::move(other.request_);
            releaser_ = std::exchange(other.releaser_, nullptr);
        }
        return *this;
    }
    ~UpstreamLease() { release(); }

    void release() noexcept {
        if (releaser_) {
            auto releaser = std::exchange(releaser_, nullptr);
            releaser(id_);
        }
    }

    [[nodiscard]] bool active() const noexcept { return static_cast<bool>(releaser_); }
    [[nodiscard]] std::uint64_t id() const noexcept { return id_; }
    [[nodiscard]] const AdmissionRequest& request() const noexcept { return request_; }

private:
    std::uint64_t id_;
    AdmissionRequest request_;
    Releaser releaser_;
};

// Admission could not be granted now. Not an upstream error; the caller re-plans or
// persists a delay without holding a worker.
struct Deferred {
    std::string blocked_dimension;
    std::chrono::steady_clock::time_point retry_at;
};

using AdmissionOutcome = std::variant<UpstreamLease, Deferred>;

// The single upstream quota ledger (implemented in runtime/admission).
// try_acquire never blocks: it grants every dimension at once or none of them.
class UpstreamAdmissionPort {
public:
    virtual ~UpstreamAdmissionPort() = default;

    virtual domain::Result<AdmissionOutcome> try_acquire(const AdmissionRequest& request) = 0;

    // Trusted upstream feedback such as Retry-After; updates cooldowns only.
    virtual void feedback(const AdmissionRequest& request,
                          std::optional<std::chrono::steady_clock::time_point> retry_after) = 0;
};

} // namespace music_dl::application::ports
