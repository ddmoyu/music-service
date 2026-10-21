#pragma once

#include "domain/result.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace music_service::application::ports {

// A published file opened for reading. Reads may come from any offset; one reader per
// response, not shared between threads.
class ReadableFile {
public:
    virtual ~ReadableFile() = default;

    [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;
    // Reads up to buffer.size() bytes at offset; fewer only at the end of the file.
    virtual domain::Result<std::size_t> read(std::uint64_t offset, std::span<char> buffer) = 0;
};

// Read-only access to files below a destination's bound root (feature-modules.md 10,
// FileAccess). A missing regular file is NOT_FOUND; an unknown id or an offline root is
// DESTINATION_UNAVAILABLE.
class PublishedFiles {
public:
    virtual ~PublishedFiles() = default;

    virtual domain::Result<std::unique_ptr<ReadableFile>> open_read(const std::string& destination_id,
                                                                    const std::string& relative_path) = 0;
};

} // namespace music_service::application::ports
