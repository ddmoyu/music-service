#pragma once

#include <utility>

namespace music_service::application::ports {

// Stack cleanup without allocating. Cleanup must not throw, including on unwinding.
template <class Function> class ScopeExit {
public:
    explicit ScopeExit(Function action) : action_(std::move(action)) {}
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ~ScopeExit() noexcept { action_(); }

private:
    Function action_;
};

} // namespace music_service::application::ports
