#include "runtime/cancellation/deadline_timer.hpp"

#include <exception>
#include <string>

namespace music_service::runtime {

DeadlineTimer::DeadlineTimer(FailureHandler on_failure)
    : on_failure_(std::move(on_failure)), thread_([this](std::stop_token stop) { run(stop); }) {}

DeadlineTimer::~DeadlineTimer() {
    thread_.request_stop();
    changed_.notify_all();
}

DeadlineTimer::Id DeadlineTimer::schedule(Clock::time_point when, std::function<void()> callback) {
    Id id = 0;
    {
        const std::lock_guard lock{mutex_};
        id = ++next_id_;
        entries_.emplace(when, std::pair{id, std::move(callback)});
        index_.emplace(id, when);
    }
    changed_.notify_all();
    return id;
}

bool DeadlineTimer::cancel(Id id) {
    const std::lock_guard lock{mutex_};
    const auto found = index_.find(id);
    if (found == index_.end()) {
        return false;
    }
    auto [first, last] = entries_.equal_range(found->second);
    for (auto it = first; it != last; ++it) {
        if (it->second.first == id) {
            entries_.erase(it);
            break;
        }
    }
    index_.erase(found);
    return true;
}

std::size_t DeadlineTimer::pending() const {
    const std::lock_guard lock{mutex_};
    return entries_.size();
}

std::uint64_t DeadlineTimer::failed() const {
    const std::lock_guard lock{mutex_};
    return failed_;
}

// Called without the mutex held.
void DeadlineTimer::fire(std::function<void()>& callback) noexcept {
    std::string what;
    try {
        callback();
        return;
    } catch (const std::exception& e) {
        what = e.what();
    } catch (...) {
        what = "unknown exception";
    }
    {
        const std::lock_guard lock{mutex_};
        ++failed_;
    }
    if (on_failure_) {
        try {
            on_failure_(what);
        } catch (...) { // a failing diagnostic sink must not take the timer down
        }
    }
}

void DeadlineTimer::run(std::stop_token stop) {
    std::unique_lock lock{mutex_};
    while (!stop.stop_requested()) {
        if (entries_.empty()) {
            changed_.wait(lock, stop, [this] { return !entries_.empty(); });
            continue;
        }
        const auto next = entries_.begin()->first;
        if (Clock::now() < next) {
            // Re-evaluates when an earlier entry is scheduled or one is cancelled.
            changed_.wait_until(lock, stop, next, [this, next] {
                return entries_.empty() || entries_.begin()->first < next;
            });
            continue;
        }
        auto callback = std::move(entries_.begin()->second.second);
        index_.erase(entries_.begin()->second.first);
        entries_.erase(entries_.begin());
        lock.unlock();
        fire(callback);
        lock.lock();
    }
}

} // namespace music_service::runtime
