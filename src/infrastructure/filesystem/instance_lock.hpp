#pragma once

#include "domain/result.hpp"

#include <cstdint>
#include <filesystem>

namespace music_service::infrastructure::filesystem {

// Exclusive, process-lifetime lock on data_dir/instance.lock. A second service process,
// or an offline management command while the service runs, gets INSTANCE_LOCKED.
// The OS drops the lock when the process exits, so a crash never leaves it stale;
// the file only carries the holder's process id for diagnostics.
class InstanceLock {
public:
    static domain::Result<InstanceLock> acquire(const std::filesystem::path& lock_file);

    InstanceLock(const InstanceLock&) = delete;
    InstanceLock& operator=(const InstanceLock&) = delete;
    InstanceLock(InstanceLock&& other) noexcept;
    InstanceLock& operator=(InstanceLock&& other) noexcept;
    ~InstanceLock();

    void release() noexcept;
    [[nodiscard]] bool held() const noexcept;

private:
    // Windows HANDLE or POSIX file descriptor; -1 means not held
    // (INVALID_HANDLE_VALUE is also -1).
    using Native = std::intptr_t;
    static constexpr Native none = -1;

    explicit InstanceLock(Native handle) : handle_(handle) {}

    Native handle_{none};
};

} // namespace music_service::infrastructure::filesystem
