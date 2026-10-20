#include "runtime/executors/bounded_executor.hpp"

#include <exception>
#include <utility>

namespace music_service::runtime {

BoundedExecutor::BoundedExecutor(std::string name, std::size_t threads, std::size_t queue_capacity,
                                 FailureHandler on_failure)
    : name_(std::move(name)), capacity_(queue_capacity), on_failure_(std::move(on_failure)) {
    try {
        workers_.reserve(threads);
        for (std::size_t i = 0; i < threads; ++i) {
            workers_.emplace_back([this](std::stop_token) { run(stop_.get_token()); });
        }
    } catch (...) {
        // Workers wait on stop_, not on their jthread's own token: without this the
        // threads already started would never leave run() and ~jthread would hang.
        request_stop();
        join();
        throw;
    }
}

BoundedExecutor::~BoundedExecutor() {
    stop_accepting();
    request_stop();
    join();
}

domain::Result<void> BoundedExecutor::submit(Work work) {
    {
        const std::lock_guard lock{mutex_};
        if (!accepting_ || stop_.stop_requested() || queue_.size() >= capacity_) {
            ++stats_.rejected;
            return domain::Error{domain::ErrorCode::service_busy,
                                 name_ + " is " + (accepting_ ? "full" : "stopping"), true, {}};
        }
        queue_.push_back(std::move(work));
        ++stats_.queued;
    }
    work_available_.notify_one();
    return domain::ok();
}

bool BoundedExecutor::run_guarded(Work& work, std::stop_token stop) noexcept {
    std::string what;
    try {
        work(std::move(stop));
        return true;
    } catch (const std::exception& e) {
        what = e.what();
    } catch (...) {
        what = "unknown exception";
    }
    if (on_failure_) {
        try {
            on_failure_(name_, what);
        } catch (...) { // a failing diagnostic sink must not take the worker down
        }
    }
    return false;
}

void BoundedExecutor::run(std::stop_token stop) {
    while (true) {
        Work work;
        {
            std::unique_lock lock{mutex_};
            // Wakes on new work or on the executor's stop request.
            if (!work_available_.wait(lock, stop, [this] { return !queue_.empty(); })) {
                return;
            }
            work = std::move(queue_.front());
            queue_.pop_front();
            --stats_.queued;
            ++stats_.running;
        }
        const bool failed = !run_guarded(work, stop);
        {
            const std::lock_guard lock{mutex_};
            --stats_.running;
            ++stats_.completed;
            if (failed) {
                ++stats_.failed;
            }
            if (queue_.empty() && stats_.running == 0) {
                idle_.notify_all();
            }
        }
    }
}

void BoundedExecutor::stop_accepting() {
    const std::lock_guard lock{mutex_};
    accepting_ = false;
}

bool BoundedExecutor::wait_idle(std::chrono::milliseconds budget) {
    std::unique_lock lock{mutex_};
    return idle_.wait_for(lock, budget, [this] { return queue_.empty() && stats_.running == 0; });
}

void BoundedExecutor::request_stop() {
    {
        const std::lock_guard lock{mutex_};
        accepting_ = false;
        stats_.queued = 0;
        queue_.clear(); // work that never started is dropped, not run half-way
    }
    stop_.request_stop();
    work_available_.notify_all();
    idle_.notify_all();
}

void BoundedExecutor::join() {
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

BoundedExecutor::Stats BoundedExecutor::stats() const {
    const std::lock_guard lock{mutex_};
    return stats_;
}

} // namespace music_service::runtime
