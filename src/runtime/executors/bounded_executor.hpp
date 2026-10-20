#pragma once

#include "domain/result.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace music_service::runtime {

// Fixed worker threads with a bounded queue (runtime-and-tasks.md, 执行器和额度).
// submit() never blocks: a full queue or a stopping executor is SERVICE_BUSY at once.
// Work receives the executor's stop token and must check it at safe points; a stop
// request is a signal, not a guarantee that blocking third-party code returns.
// An exception escaping work is caught at the executor boundary: it counts as failed,
// is reported to the optional handler (internal diagnostics only) and the worker
// keeps running.
class BoundedExecutor {
public:
    using Work = std::function<void(std::stop_token)>;
    // Receives the executor name and the exception's what(); must not throw.
    using FailureHandler = std::function<void(std::string_view executor, std::string_view what)>;

    struct Stats {
        std::size_t queued{0};
        std::size_t running{0};
        std::uint64_t completed{0}; // includes failed
        std::uint64_t failed{0};
        std::uint64_t rejected{0};
    };

    BoundedExecutor(std::string name, std::size_t threads, std::size_t queue_capacity,
                    FailureHandler on_failure = {});
    BoundedExecutor(const BoundedExecutor&) = delete;
    BoundedExecutor& operator=(const BoundedExecutor&) = delete;
    // Requests stop and joins.
    ~BoundedExecutor();

    domain::Result<void> submit(Work work);

    // Shutdown steps, usually driven by ShutdownSequence:
    // 1. refuse new work (queued work still runs);
    void stop_accepting();
    // 2. wait until nothing is queued or running, at most for budget; true when idle;
    bool wait_idle(std::chrono::milliseconds budget);
    // 3. signal running work to stop and drop work that has not started;
    void request_stop();
    // 4. wait for the worker threads to exit.
    void join();

    [[nodiscard]] Stats stats() const;
    [[nodiscard]] const std::string& name() const noexcept { return name_; }

private:
    void run(std::stop_token stop);
    // false when work threw; the exception is reported, never propagated.
    bool run_guarded(Work& work, std::stop_token stop) noexcept;

    std::string name_;
    std::size_t capacity_;
    FailureHandler on_failure_;
    mutable std::mutex mutex_;
    std::condition_variable_any work_available_;
    std::condition_variable idle_;
    std::deque<Work> queue_;
    bool accepting_{true};
    Stats stats_;
    std::stop_source stop_;
    std::vector<std::jthread> workers_;
};

} // namespace music_service::runtime
