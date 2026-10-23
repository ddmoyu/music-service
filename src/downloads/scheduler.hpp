#pragma once

#include "application/ports/runtime/admission.hpp"
#include "application/ports/runtime/clock.hpp"
#include "application/ports/runtime/download_queue.hpp"
#include "application/ports/runtime/executor.hpp"
#include "application/ports/storage/task_repository.hpp"
#include "downloads/runner.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace music_service::downloads {

struct SchedulerOptions {
    std::uint32_t workers{3}; // downloads.workers: execution slots
    // Periodic scan for due tasks; covers lost wake-ups and waiting_retry times.
    std::chrono::milliseconds scan_interval{1000};
    std::uint32_t candidates_per_owner{8};
    std::uint32_t candidates{256};
};

// Called after each execution ends; must not throw. Carries no secrets.
using RunObserver = std::function<void(const ports::TaskRecord& claimed, const RunReport& report)>;

// Persistent download dispatch (runtime-and-tasks.md, 入队与调度). One dispatcher thread
// claims runnable tasks while execution slots are free and hands each to the executor:
//   - principals take turns; within one principal the earliest due task goes first;
//   - a task whose original source the admission ledger reports as blocked is skipped
//     (a hint, not a reservation), so a limited source never holds back others (A43);
//   - claims are conditional updates that issue a new execution id; claiming never counts
//     an attempt;
//   - paused stops claiming only: running executions continue (admin/download-policy);
//   - wake() and the periodic scan both lead to the same database query.
// Shutdown: stop_claiming(), wait_idle() within the soft budget, stop_running() asks the
// remaining executions to save their checkpoint and pause, then the executor is joined by
// its owner.
class DownloadScheduler final : public ports::DownloadQueue {
public:
    DownloadScheduler(SchedulerOptions options, std::shared_ptr<ports::TaskRepository> tasks,
                      std::shared_ptr<ports::UpstreamAdmissionPort> admission, std::shared_ptr<DownloadRunner> runner,
                      std::shared_ptr<ports::WorkExecutor> executor, std::shared_ptr<const ports::Clock> clock,
                      RunObserver observer = {});
    DownloadScheduler(const DownloadScheduler&) = delete;
    DownloadScheduler& operator=(const DownloadScheduler&) = delete;
    ~DownloadScheduler() override;

    void start();

    void wake() override;
    void request_stop(const std::string& task_id) override;

    void set_paused(bool paused);
    [[nodiscard]] bool paused() const;

    void stop_claiming();
    // true when no execution is running any more.
    bool wait_idle(std::chrono::milliseconds budget);
    // Every running execution saves its state and pauses (service_shutdown).
    void stop_running();
    // Ends the dispatcher thread; executions are joined with the executor.
    void join();

    struct Stats {
        std::uint32_t workers{0};
        std::uint32_t running{0};
        bool paused{false};
        std::uint64_t claimed{0};
        std::uint64_t errors{0}; // dispatch, cleanup or observer exceptions
    };
    [[nodiscard]] Stats stats() const;

private:
    void dispatch(std::stop_token stop);
    // Claims and submits until the slots are full or nothing is runnable; returns when to
    // look again at the latest.
    std::chrono::steady_clock::time_point fill();
    std::optional<ports::RunnableTask> choose(const std::vector<ports::RunnableTask>& candidates,
                                              std::chrono::steady_clock::time_point& retry_at);
    void finished(const std::string& task_id);

    SchedulerOptions options_;
    std::shared_ptr<ports::TaskRepository> tasks_;
    std::shared_ptr<ports::UpstreamAdmissionPort> admission_;
    std::shared_ptr<DownloadRunner> runner_;
    std::shared_ptr<ports::WorkExecutor> executor_;
    std::shared_ptr<const ports::Clock> clock_;
    RunObserver observer_;

    mutable std::mutex mutex_;
    std::condition_variable_any changed_;
    bool woken_{false};
    bool paused_{false};
    bool claiming_{true};
    std::string last_owner_; // fairness: the next turn starts after this principal
    std::map<std::string, std::shared_ptr<ExecutionControl>> running_;
    std::uint64_t claimed_{0};
    std::atomic<std::uint64_t> errors_{0};
    std::condition_variable idle_;
    std::jthread dispatcher_;
};

} // namespace music_service::downloads
