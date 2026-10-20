#pragma once

#include <chrono>

namespace music_service::application::ports {

// Injected time source so deadlines, budgets and signatures can be tested deterministically.
class Clock {
public:
    virtual ~Clock() = default;

    [[nodiscard]] virtual std::chrono::system_clock::time_point now() const = 0;
    [[nodiscard]] virtual std::chrono::steady_clock::time_point steady_now() const = 0;
};

} // namespace music_service::application::ports
