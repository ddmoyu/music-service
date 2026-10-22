#pragma once

#include "domain/result.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace music_service::infrastructure::filesystem {

enum class Privacy {
    normal,     // inherits the directory's permissions
    owner_only, // POSIX 0600/0700; Windows protected ACL for the owner, SYSTEM and Administrators
};

// Creates the directory and missing parents. With owner_only, the parents this call
// creates get owner-only permissions, and so does the directory itself even when it
// already existed: permissive entries are replaced, and on POSIX a directory owned by
// another user is refused. Existing parents are left unchanged.
domain::Result<void> create_directories(const std::filesystem::path& path, Privacy privacy);

// Creates a new file exclusively (fails if anything exists at the path), writes the
// bytes and flushes them (and on POSIX the new directory entry) to the device before
// returning. On failure the partial file is removed. Never overwrites.
domain::Result<void> write_new_file(const std::filesystem::path& path, std::string_view bytes,
                                    Privacy privacy);

// Reads a whole regular file of at most max_bytes.
domain::Result<std::string> read_file(const std::filesystem::path& path, std::size_t max_bytes);

// Renames within one file system and fails instead of replacing an existing target
// (Windows MoveFileEx without REPLACE_EXISTING, Linux renameat2 RENAME_NOREPLACE or
// link+unlink as a fallback). On POSIX the target directory is flushed afterwards; if
// that fails, the rename has happened but may not survive power loss, and an error is
// returned.
domain::Result<void> rename_no_replace(const std::filesystem::path& from,
                                       const std::filesystem::path& to);

// Removes a file; a missing file is not an error.
domain::Result<void> remove_file(const std::filesystem::path& path);

// A file written at its end and flushed on request (download work files). Opening creates
// it when missing and never replaces another file system object. Move-only.
class AppendFile {
public:
    static domain::Result<AppendFile> open(const std::filesystem::path& path);

    AppendFile(AppendFile&& other) noexcept;
    AppendFile& operator=(AppendFile&& other) noexcept;
    AppendFile(const AppendFile&) = delete;
    AppendFile& operator=(const AppendFile&) = delete;
    ~AppendFile();

    domain::Result<void> write(std::string_view bytes);
    // Data and size reach the device before it returns (FlushFileBuffers / fsync).
    domain::Result<void> flush();
    // Shortens the file; a length beyond the end is ignored.
    domain::Result<void> truncate(std::uint64_t length);
    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }

private:
    AppendFile(std::intptr_t handle, std::filesystem::path path, std::uint64_t size)
        : handle_(handle), path_(std::move(path)), size_(size) {}
    void close() noexcept;

    std::intptr_t handle_{-1}; // HANDLE on Windows, file descriptor elsewhere
    std::filesystem::path path_;
    std::uint64_t size_{0};
};

} // namespace music_service::infrastructure::filesystem
