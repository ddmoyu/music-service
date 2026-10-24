#pragma once

#include "application/ports/observability/metrics.hpp"
#include <algorithm>
#include <limits>
#include <mutex>

namespace music_service::infrastructure::observability {

class InMemoryMetrics final : public application::ports::Metrics {
public:
    void record(application::ports::MetricOperation operation, application::ports::MetricOutcome outcome,
                std::chrono::nanoseconds elapsed) noexcept override {
        const auto op = static_cast<std::size_t>(operation);
        const auto result = static_cast<std::size_t>(outcome);
        if (op >= data_.operations.size() || result >= application::ports::metric_outcomes.size()) return;
        const auto ns = static_cast<std::uint64_t>(std::max<std::int64_t>(0, elapsed.count()));
        const std::lock_guard lock{mutex_};
        auto& item = data_.operations[op];
        add(item.count, 1);
        add(item.outcomes[result], 1);
        add(item.duration_ns, ns);
        item.max_duration_ns = std::max(item.max_duration_ns, ns);
        std::size_t bucket = 0;
        for (const auto bound : application::ports::duration_buckets_ms) {
            if (ns <= bound * 1'000'000) break;
            ++bucket;
        }
        add(item.durations[bucket], 1);
    }
    [[nodiscard]] application::ports::MetricsSnapshot snapshot() const override {
        const std::lock_guard lock{mutex_};
        return data_;
    }
private:
    static void add(std::uint64_t& value, std::uint64_t amount) noexcept {
        value += std::min(amount, std::numeric_limits<std::uint64_t>::max() - value);
    }
    mutable std::mutex mutex_;
    application::ports::MetricsSnapshot data_;
};
} // namespace music_service::infrastructure::observability
