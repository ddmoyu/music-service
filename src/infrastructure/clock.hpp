#pragma once

#include "application/ports/runtime/clock.hpp"

#include <chrono>
#include <mutex>

namespace music_dl::infrastructure {

class SystemClock final : public application::ports::Clock {
public:
    [[nodiscard]] std::chrono::system_clock::time_point now() const override {
        return std::chrono::system_clock::now();
    }
    [[nodiscard]] std::chrono::steady_clock::time_point steady_now() const override {
        return std::chrono::steady_clock::now();
    }
};

// Deterministic clock for tests: time only moves when advance() or set_wall() is called.
// The wall clock may jump; the steady clock only moves forward.
class ManualClock final : public application::ports::Clock {
public:
    explicit ManualClock(std::chrono::system_clock::time_point wall =
                             std::chrono::system_clock::time_point{std::chrono::seconds{1'700'000'000}})
        : wall_(wall) {}

    [[nodiscard]] std::chrono::system_clock::time_point now() const override {
        const std::lock_guard lock{mutex_};
        return wall_;
    }
    [[nodiscard]] std::chrono::steady_clock::time_point steady_now() const override {
        const std::lock_guard lock{mutex_};
        return steady_;
    }

    void advance(std::chrono::steady_clock::duration delta) {
        const std::lock_guard lock{mutex_};
        steady_ += delta;
        wall_ += std::chrono::duration_cast<std::chrono::system_clock::duration>(delta);
    }

    // Simulates a wall-clock adjustment without moving monotonic time.
    void set_wall(std::chrono::system_clock::time_point wall) {
        const std::lock_guard lock{mutex_};
        wall_ = wall;
    }

private:
    mutable std::mutex mutex_;
    std::chrono::system_clock::time_point wall_;
    std::chrono::steady_clock::time_point steady_{};
};

} // namespace music_dl::infrastructure
