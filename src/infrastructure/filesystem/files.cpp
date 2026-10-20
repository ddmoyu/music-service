#include "infrastructure/filesystem/files.hpp"

#include "infrastructure/filesystem/paths.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#else
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace music_service::infrastructure::filesystem {

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32

std::error_code last_error() {
    return {static_cast<int>(::GetLastError()), std::system_category()};
}

// Owner (OW = owner rights) and LocalSystem only; protected from inheriting the
// parent's permissive entries.
class OwnerOnlySecurity {
public:
    explicit OwnerOnlySecurity(bool directory) {
        const wchar_t* sddl = directory ? L"D:P(A;OICI;FA;;;OW)(A;OICI;FA;;;SY)"
                                        : L"D:P(A;;FA;;;OW)(A;;FA;;;SY)";
        if (::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1,
                                                                   &descriptor_, nullptr)) {
            attributes_.nLength = sizeof attributes_;
            attributes_.lpSecurityDescriptor = descriptor_;
            attributes_.bInheritHandle = FALSE;
        }
    }
    OwnerOnlySecurity(const OwnerOnlySecurity&) = delete;
    OwnerOnlySecurity& operator=(const OwnerOnlySecurity&) = delete;
    ~OwnerOnlySecurity() {
        if (descriptor_ != nullptr) {
            ::LocalFree(descriptor_);
        }
    }

    [[nodiscard]] bool valid() const noexcept { return descriptor_ != nullptr; }
    SECURITY_ATTRIBUTES* get() noexcept { return &attributes_; }
    [[nodiscard]] PSECURITY_DESCRIPTOR descriptor() const noexcept { return descriptor_; }

private:
    PSECURITY_DESCRIPTOR descriptor_{nullptr};
    SECURITY_ATTRIBUTES attributes_{};
};

class Handle {
public:
    explicit Handle(HANDLE handle) : handle_(handle) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() {
        if (valid()) {
            ::CloseHandle(handle_);
        }
    }
    [[nodiscard]] bool valid() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }
    [[nodiscard]] HANDLE get() const noexcept { return handle_; }

private:
    HANDLE handle_;
};

#else

std::error_code last_error() { return {errno, std::system_category()}; }

class Fd {
public:
    explicit Fd(int fd) : fd_(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    [[nodiscard]] int get() const noexcept { return fd_; }

private:
    int fd_;
};

// Makes a completed create or rename durable across power loss. A file system that
// cannot sync directories at all (EINVAL, ENOTSUP) is not an error; a failed sync is.
std::error_code sync_directory(const fs::path& directory) {
    const Fd fd{::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    if (fd.get() < 0) {
        return last_error();
    }
    while (::fsync(fd.get()) != 0) {
        if (errno == EINTR) {
            continue;
        }
        if (errno == EINVAL || errno == ENOTSUP) {
            return {};
        }
        return last_error();
    }
    return {};
}

#endif

// Gives an existing directory owner-only permissions, so one restored from a backup or
// created by hand cannot keep permissive entries.
domain::Result<void> restrict_to_owner(const fs::path& path) {
#ifdef _WIN32
    OwnerOnlySecurity security{true};
    if (!security.valid()) {
        return file_error("build owner-only ACL", path, last_error());
    }
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    PACL dacl = nullptr;
    if (!::GetSecurityDescriptorDacl(security.descriptor(), &present, &dacl, &defaulted) ||
        !present) {
        return file_error("build owner-only ACL", path, last_error());
    }
    // PROTECTED drops the entries inherited from the parent; the new inheritable entries
    // replace inherited ones on everything below.
    std::wstring name = path.native();
    const auto status = ::SetNamedSecurityInfoW(
        name.data(), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr,
        dacl, nullptr);
    if (status != ERROR_SUCCESS) {
        return file_error("restrict directory to owner", path,
                          {static_cast<int>(status), std::system_category()});
    }
#else
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) {
        return file_error("restrict directory to owner", path, last_error());
    }
    if (info.st_uid != ::geteuid()) {
        // The owner could grant access again at any time.
        return file_error("restrict directory to owner", path,
                          std::make_error_code(std::errc::permission_denied));
    }
    if ((info.st_mode & 077) != 0 && ::chmod(path.c_str(), 0700) != 0) {
        return file_error("restrict directory to owner", path, last_error());
    }
#endif
    return domain::ok();
}

domain::Result<void> create_missing_directories(const fs::path& path, Privacy privacy) {
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
        return domain::ok();
    }
    if (const auto parent = path.parent_path(); !parent.empty() && parent != path) {
        if (auto created = create_missing_directories(parent, privacy); !created) {
            return created;
        }
    }
#ifdef _WIN32
    BOOL created = FALSE;
    if (privacy == Privacy::owner_only) {
        OwnerOnlySecurity security{true};
        if (!security.valid()) {
            return file_error("build owner-only ACL", path, last_error());
        }
        created = ::CreateDirectoryW(path.c_str(), security.get());
    } else {
        created = ::CreateDirectoryW(path.c_str(), nullptr);
    }
    if (!created && ::GetLastError() != ERROR_ALREADY_EXISTS) {
        return file_error("create directory", path, last_error());
    }
#else
    const mode_t mode = privacy == Privacy::owner_only ? 0700 : 0755;
    if (::mkdir(path.c_str(), mode) != 0 && errno != EEXIST) {
        return file_error("create directory", path, last_error());
    }
#endif
    if (!fs::is_directory(path, ec)) {
        return file_error("create directory", path,
                          ec ? ec : std::make_error_code(std::errc::not_a_directory));
    }
    return domain::ok();
}

} // namespace

domain::Result<void> create_directories(const fs::path& path, Privacy privacy) {
    if (auto created = create_missing_directories(path, privacy); !created) {
        return created;
    }
    if (privacy == Privacy::owner_only) {
        return restrict_to_owner(path);
    }
    return domain::ok();
}

domain::Result<void> write_new_file(const fs::path& path, std::string_view bytes,
                                    Privacy privacy) {
#ifdef _WIN32
    OwnerOnlySecurity security{false};
    if (privacy == Privacy::owner_only && !security.valid()) {
        return file_error("build owner-only ACL", path, last_error());
    }
    std::error_code failure;
    std::string_view failed_operation;
    {
        const Handle file{::CreateFileW(path.c_str(), GENERIC_WRITE, 0,
                                        privacy == Privacy::owner_only ? security.get() : nullptr,
                                        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (!file.valid()) {
            return file_error("create file", path, last_error());
        }
        std::size_t written = 0;
        while (written < bytes.size()) {
            const auto chunk = static_cast<DWORD>(
                std::min<std::size_t>(bytes.size() - written, std::size_t{1} << 20U));
            DWORD done = 0;
            if (!::WriteFile(file.get(), bytes.data() + written, chunk, &done, nullptr) ||
                done == 0) {
                failure = last_error();
                failed_operation = "write file";
                break;
            }
            written += done;
        }
        if (!failure && !::FlushFileBuffers(file.get())) {
            failure = last_error();
            failed_operation = "flush file";
        }
    } // the handle is closed here, before the partial file is removed
    if (failure) {
        ::DeleteFileW(path.c_str());
        return file_error(failed_operation, path, failure);
    }
    return domain::ok();
#else
    const mode_t mode = privacy == Privacy::owner_only ? 0600 : 0644;
    const Fd fd{::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode)};
    if (fd.get() < 0) {
        return file_error("create file", path, last_error());
    }
    std::size_t written = 0;
    while (written < bytes.size()) {
        const auto done = ::write(fd.get(), bytes.data() + written, bytes.size() - written);
        if (done < 0) {
            if (errno == EINTR) {
                continue;
            }
            const auto ec = last_error();
            ::unlink(path.c_str());
            return file_error("write file", path, ec);
        }
        written += static_cast<std::size_t>(done);
    }
    if (::fsync(fd.get()) != 0) {
        const auto ec = last_error();
        ::unlink(path.c_str());
        return file_error("flush file", path, ec);
    }
    if (const auto ec = sync_directory(path.parent_path()); ec) {
        ::unlink(path.c_str());
        return file_error("flush directory", path.parent_path(), ec);
    }
    return domain::ok();
#endif
}

domain::Result<std::string> read_file(const fs::path& path, std::size_t max_bytes) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) {
        return file_error("read file", path, ec,
                          ec == std::errc::no_such_file_or_directory ? domain::ErrorCode::not_found
                                                                     : domain::ErrorCode::file_system);
    }
    if (size > max_bytes) {
        return file_error("read file", path, std::make_error_code(std::errc::file_too_large));
    }
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return file_error("open file", path, std::make_error_code(std::errc::io_error));
    }
    std::string content(static_cast<std::size_t>(size), '\0');
    in.read(content.data(), static_cast<std::streamsize>(content.size()));
    if (in.gcount() != static_cast<std::streamsize>(content.size())) {
        return file_error("read file", path, std::make_error_code(std::errc::io_error));
    }
    return content;
}

domain::Result<void> rename_no_replace(const fs::path& from, const fs::path& to) {
#ifdef _WIN32
    if (!::MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) {
        return file_error("rename", to, last_error());
    }
    return domain::ok();
#else
#ifdef RENAME_NOREPLACE
    if (::renameat2(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE) == 0) {
        if (const auto ec = sync_directory(to.parent_path()); ec) {
            return file_error("flush directory", to.parent_path(), ec);
        }
        return domain::ok();
    }
    if (errno != EINVAL && errno != ENOSYS) {
        return file_error("rename", to, last_error());
    }
#endif
    // Fallback for file systems without RENAME_NOREPLACE: link fails if the target exists.
    if (::link(from.c_str(), to.c_str()) != 0) {
        return file_error("rename", to, last_error());
    }
    if (::unlink(from.c_str()) != 0) {
        return file_error("remove renamed source", from, last_error());
    }
    if (const auto ec = sync_directory(to.parent_path()); ec) {
        return file_error("flush directory", to.parent_path(), ec);
    }
    return domain::ok();
#endif
}

domain::Result<void> remove_file(const fs::path& path) {
    std::error_code ec;
    fs::remove(path, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
        return file_error("remove file", path, ec);
    }
    return domain::ok();
}

} // namespace music_service::infrastructure::filesystem
