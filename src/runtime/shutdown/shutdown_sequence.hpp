#pragma once

#include "runtime/executors/bounded_executor.hpp"

#include <chrono>
#include <stop_token>
#include <string>
#include <vector>

namespace music_service::runtime {

// Ordered graceful stop (runtime-and-tasks.md, 崩溃恢复与优雅退出):
// 1. every executor stops accepting work;
// 2. all of them get one shared soft budget (shutdown.soft_seconds) to become idle;
// 3. the service stop signal fires and executors drop queued work and signal running
//    work, which saves checkpoints and exits at its next safe point;
// 4. worker threads are joined. An outer service manager enforces the hard limit.
// Readiness is turned off and listeners closed by the process host around this.
class ShutdownSequence {
public:
    struct Report {
        bool drained_within_budget{true};
        std::vector<std::string> still_busy; // executor names not idle at the soft budget
    };

    // Executors are drained in the order they were added.
    void add(BoundedExecutor& executor) { executors_.push_back(&executor); }

    // Service-wide stop signal for long-running work outside executors (SSE streams,
    // runner loops). Fires in step 3.
    [[nodiscard]] std::stop_token token() const noexcept { return stop_.get_token(); }
    [[nodiscard]] bool stopping() const noexcept { return stop_.stop_requested(); }

    Report run(std::chrono::milliseconds soft_budget);

private:
    std::vector<BoundedExecutor*> executors_;
    std::stop_source stop_;
};

} // namespace music_service::runtime
