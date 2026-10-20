#include "infrastructure/filesystem/instance_lock.hpp"

#include "infrastructure/filesystem/paths.hpp"

#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace music_service::infrastructure::filesystem {

namespace {

domain::Error locked_error(const std::filesystem::path& lock_file) {
    return domain::Error{domain::ErrorCode::instance_locked,
                         "another music-service process or management command holds '" +
                             path_to_utf8(lock_file) + "'",
                         false,
                         {}};
}

} // namespace

#ifdef _WIN32

domain::Result<InstanceLock> InstanceLock::acquire(const std::filesystem::path& lock_file) {
    // Share mode 0: any second open of the file fails while this handle lives.
    HANDLE handle = ::CreateFileW(lock_file.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto error = ::GetLastError();
        if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION) {
            return locked_error(lock_file);
        }
        return file_error("lock instance", lock_file,
                          std::error_code{static_cast<int>(error), std::system_category()});
    }
    const auto pid = std::to_string(::GetCurrentProcessId()) + "\n";
    DWORD written = 0;
    ::SetEndOfFile(handle);
    ::WriteFile(handle, pid.data(), static_cast<DWORD>(pid.size()), &written, nullptr);
    return InstanceLock{reinterpret_cast<std::intptr_t>(handle)};
}

void InstanceLock::release() noexcept {
    if (handle_ != none) {
        ::CloseHandle(reinterpret_cast<HANDLE>(handle_));
        handle_ = none;
    }
}

#else

domain::Result<InstanceLock> InstanceLock::acquire(const std::filesystem::path& lock_file) {
    const int fd = ::open(lock_file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        return file_error("lock instance", lock_file,
                          std::error_code{errno, std::system_category()});
    }
    // flock locks belong to the open file description, so even a second open in the
    // same process conflicts.
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int error = errno;
        ::close(fd);
        if (error == EWOULDBLOCK) {
            return locked_error(lock_file);
        }
        return file_error("lock instance", lock_file,
                          std::error_code{error, std::system_category()});
    }
    const auto pid = std::to_string(::getpid()) + "\n";
    if (::ftruncate(fd, 0) == 0) {
        [[maybe_unused]] const auto written = ::write(fd, pid.data(), pid.size());
    }
    return InstanceLock{fd};
}

void InstanceLock::release() noexcept {
    if (handle_ != none) {
        ::flock(static_cast<int>(handle_), LOCK_UN);
        ::close(static_cast<int>(handle_));
        handle_ = none;
    }
}

#endif

InstanceLock::InstanceLock(InstanceLock&& other) noexcept
    : handle_(std::exchange(other.handle_, none)) {}

InstanceLock& InstanceLock::operator=(InstanceLock&& other) noexcept {
    if (this != &other) {
        release();
        handle_ = std::exchange(other.handle_, none);
    }
    return *this;
}

InstanceLock::~InstanceLock() { release(); }

bool InstanceLock::held() const noexcept { return handle_ != none; }

} // namespace music_service::infrastructure::filesystem
