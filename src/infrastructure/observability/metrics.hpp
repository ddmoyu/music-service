#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

namespace music_service::infrastructure::observability {

// Collection points for counters and latencies. Names are dotted, e.g.
// "upstream.requests" or "admission.wait"; areas follow the design: control, provider,
// download, processing, db, media, sse. No secrets or per-user values in names.
class Metrics {
public:
    virtual ~Metrics() = default;
    virtual void increment(std::string_view name, std::uint64_t by = 1) = 0;
    virtual void observe(std::string_view name, std::chrono::nanoseconds duration) = 0;
};

class NoopMetrics final : public Metrics {
public:
    void increment(std::string_view, std::uint64_t) override {}
    void observe(std::string_view, std::chrono::nanoseconds) override {}
};

struct LatencySummary {
    std::uint64_t count{0};
    std::chrono::nanoseconds total{0};
    std::chrono::nanoseconds max{0};
};

// Thread-safe in-process aggregation, read by admin/status and tests.
class InMemoryMetrics final : public Metrics {
public:
    void increment(std::string_view name, std::uint64_t by = 1) override {
        const std::lock_guard lock{mutex_};
        counters_[std::string{name}] += by;
    }

    void observe(std::string_view name, std::chrono::nanoseconds duration) override {
        const std::lock_guard lock{mutex_};
        auto& summary = latencies_[std::string{name}];
        ++summary.count;
        summary.total += duration;
        if (duration > summary.max) {
            summary.max = duration;
        }
    }

    [[nodiscard]] std::uint64_t counter(std::string_view name) const {
        const std::lock_guard lock{mutex_};
        const auto it = counters_.find(name);
        return it == counters_.end() ? 0 : it->second;
    }

    [[nodiscard]] LatencySummary latency(std::string_view name) const {
        const std::lock_guard lock{mutex_};
        const auto it = latencies_.find(name);
        return it == latencies_.end() ? LatencySummary{} : it->second;
    }

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::uint64_t, std::less<>> counters_;
    std::map<std::string, LatencySummary, std::less<>> latencies_;
};

} // namespace music_service::infrastructure::observability
