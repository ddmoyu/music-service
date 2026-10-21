#pragma once

#include "domain/result.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace music_service::application::ports {

// One file a runner writes in its task's work directory. Writes go to the end; flush()
// makes everything written so far durable before a checkpoint may claim it.
class WorkFile {
public:
    virtual ~WorkFile() = default;

    virtual domain::Result<void> write(std::span<const char> bytes) = 0;
    virtual domain::Result<void> flush() = 0;
    // Back to length bytes (only ever shortening); the next write continues there.
    virtual domain::Result<void> truncate(std::uint64_t length) = 0;
    [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;
};

// Work files under "<root>/.music-service-work/<task_id>/" of a destination, on the same
// file system as the published files (feature-modules.md 10, FileWorkspace). The runner
// owns its task's directory exclusively. An unknown id or an offline root is
// DESTINATION_UNAVAILABLE.
class WorkFiles {
public:
    virtual ~WorkFiles() = default;

    // "<work directory>/<task_id>/<name>", relative to the destination root.
    [[nodiscard]] virtual std::string relative_path(const std::string& task_id, const std::string& name) const = 0;

    // Opens the work file, creating it and its directory when missing. Existing bytes
    // beyond keep_bytes are cut off, so the file never holds more than the caller
    // trusts; a shorter file stays as it is (its size() tells).
    virtual domain::Result<std::unique_ptr<WorkFile>> open(const std::string& destination_id,
                                                           const std::string& task_id, const std::string& name,
                                                           std::uint64_t keep_bytes) = 0;

    // The local path of a file below the destination root, for processors that read it.
    virtual domain::Result<std::filesystem::path> local_path(const std::string& destination_id,
                                                             const std::string& relative_path) = 0;
};

} // namespace music_service::application::ports
