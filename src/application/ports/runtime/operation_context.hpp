#pragma once

#include <chrono>
#include <stop_token>

namespace music_service::application::ports {

// Deadline and cancellation shared by every bounded operation. The deadline uses the
// monotonic clock; persistent deadlines are stored separately in UTC.
struct OperationContext {
    std::chrono::steady_clock::time_point deadline;
    std::stop_token stop;

    [[nodiscard]] bool expired(std::chrono::steady_clock::time_point now) const noexcept {
        return now >= deadline;
    }
};

} // namespace music_service::application::ports
