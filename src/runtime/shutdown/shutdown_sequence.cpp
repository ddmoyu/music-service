#include "runtime/shutdown/shutdown_sequence.hpp"

#include <algorithm>

namespace music_service::runtime {

ShutdownSequence::Report ShutdownSequence::run(std::chrono::milliseconds soft_budget) {
    Report report;
    for (auto* executor : executors_) {
        executor->stop_accepting();
    }

    const auto deadline = std::chrono::steady_clock::now() + soft_budget;
    for (auto* executor : executors_) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (!executor->wait_idle(std::max(left, std::chrono::milliseconds{0}))) {
            report.drained_within_budget = false;
            report.still_busy.push_back(executor->name());
        }
    }

    stop_.request_stop();
    for (auto* executor : executors_) {
        executor->request_stop();
    }
    for (auto* executor : executors_) {
        executor->join();
    }
    return report;
}

} // namespace music_service::runtime
