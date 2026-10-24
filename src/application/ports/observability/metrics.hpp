#pragma once

#include "domain/error.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>

namespace music_service::application::ports {

// Fixed dimensions: URLs, account ids, task ids and user input are never labels.
enum class MetricOperation { control, upstream, download, processing, storage_read, storage_write, media, events, count };
enum class MetricOutcome { success, failure, canceled, deferred, count };
inline constexpr std::array<std::string_view, 8> metric_operations{
    "control_request", "upstream_call", "download_execution", "audio_processing",
    "storage_read", "storage_write", "media_request", "event_request"};
inline constexpr std::array<std::string_view, 4> metric_outcomes{"success", "failure", "canceled", "deferred"};
inline constexpr std::array<std::uint64_t, 12> duration_buckets_ms{1, 5, 10, 25, 50, 100, 250, 500, 1000, 5000, 30000, 300000};

struct OperationMetrics {
    std::array<std::uint64_t, metric_outcomes.size()> outcomes{};
    std::uint64_t count{0}, duration_ns{0}, max_duration_ns{0};
    // Non-cumulative counts, including the final +Inf bucket.
    std::array<std::uint64_t, duration_buckets_ms.size() + 1> durations{};
};
struct MetricsSnapshot {
    std::array<OperationMetrics, metric_operations.size()> operations{};
};
class Metrics {
public:
    virtual ~Metrics() = default;
    virtual void record(MetricOperation operation, MetricOutcome outcome,
                        std::chrono::nanoseconds elapsed) noexcept = 0;
    [[nodiscard]] virtual MetricsSnapshot snapshot() const = 0;
};
inline MetricOutcome metric_outcome(const domain::Error& error) noexcept {
    return error.code == domain::ErrorCode::canceled ? MetricOutcome::canceled : MetricOutcome::failure;
}
inline MetricOutcome http_metric_outcome(int status) noexcept {
    if (status == 429) return MetricOutcome::deferred;
    return status >= 200 && status < 400 ? MetricOutcome::success : MetricOutcome::failure;
}
// Unwinding counts as failure. Recording cannot change the business result.
class MetricTimer {
public:
    MetricTimer(std::shared_ptr<Metrics> metrics, MetricOperation operation)
        : metrics_(std::move(metrics)), operation_(operation), start_(std::chrono::steady_clock::now()) {}
    ~MetricTimer() {
        if (metrics_) metrics_->record(operation_, outcome, std::chrono::steady_clock::now() - start_);
    }
    MetricTimer(const MetricTimer&) = delete;
    MetricTimer& operator=(const MetricTimer&) = delete;
    MetricOutcome outcome{MetricOutcome::failure};
private:
    std::shared_ptr<Metrics> metrics_;
    MetricOperation operation_;
    std::chrono::steady_clock::time_point start_;
};
} // namespace music_service::application::ports
