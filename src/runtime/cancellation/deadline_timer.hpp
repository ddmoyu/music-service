#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <stop_token>
#include <string_view>
#include <thread>
#include <utility>

namespace music_service::runtime {

// One background thread that runs callbacks at monotonic deadlines: request-stop on an
// operation's deadline, or waking waiters when a cooldown ends. Callbacks run on the
// timer thread and must be short and non-blocking (typically stop_source::request_stop
// or a notify). No busy polling. A callback that throws is caught, counted and reported
// to the optional handler (internal diagnostics only); later callbacks still run.
class DeadlineTimer {
public:
    using Clock = std::chrono::steady_clock;
    using Id = std::uint64_t;
    // Receives the exception's what(); must not throw.
    using FailureHandler = std::function<void(std::string_view what)>;

    explicit DeadlineTimer(FailureHandler on_failure = {});
    DeadlineTimer(const DeadlineTimer&) = delete;
    DeadlineTimer& operator=(const DeadlineTimer&) = delete;
    ~DeadlineTimer();

    Id schedule(Clock::time_point when, std::function<void()> callback);
    // true when the callback was removed before it ran.
    bool cancel(Id id);
    [[nodiscard]] std::size_t pending() const;
    // Callbacks that threw.
    [[nodiscard]] std::uint64_t failed() const;

private:
    void run(std::stop_token stop);
    void fire(std::function<void()>& callback) noexcept;

    mutable std::mutex mutex_;
    std::condition_variable_any changed_;
    std::multimap<Clock::time_point, std::pair<Id, std::function<void()>>> entries_;
    std::map<Id, Clock::time_point> index_;
    Id next_id_{0};
    std::uint64_t failed_{0};
    FailureHandler on_failure_;
    std::jthread thread_;
};

} // namespace music_service::runtime
