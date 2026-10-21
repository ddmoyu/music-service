#pragma once

#include "domain/result.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace music_service::application::ports {

// Size and SHA-256 (lowercase hex) of a file's bytes, the evidence a publication intent
// records and recovery compares with.
struct FileFingerprint {
    std::uint64_t size_bytes{0};
    std::string sha256;

    friend bool operator==(const FileFingerprint&, const FileFingerprint&) = default;
};

// Files below a destination's bound root (feature-modules.md 10, FileWorkspace and
// AssetStore). Paths are UTF-8, relative to that root and written with '/'; work files
// live under ".music-service-work/<task_id>/" on the same file system, so publishing is a
// rename. An unknown id or a root that is offline is DESTINATION_UNAVAILABLE, never a
// fallback to another directory.
class DestinationFiles {
public:
    virtual ~DestinationFiles() = default;

    // Whether anything (file, directory or link) exists at the path.
    virtual domain::Result<bool> exists(const std::string& destination_id, const std::string& relative_path) = 0;

    // The regular file's fingerprint; empty when nothing is there.
    virtual domain::Result<std::optional<FileFingerprint>> fingerprint(const std::string& destination_id,
                                                                       const std::string& relative_path) = 0;

    // Renames within the destination, creating the target's missing parent directories.
    // Never replaces: an existing target is CONFLICT and both files stay as they were.
    virtual domain::Result<void> move(const std::string& destination_id, const std::string& from,
                                      const std::string& to) = 0;

    // Removes a task's work directory and everything in it; a missing one is fine.
    virtual domain::Result<void> remove_workspace(const std::string& destination_id, const std::string& task_id) = 0;
};

} // namespace music_service::application::ports
