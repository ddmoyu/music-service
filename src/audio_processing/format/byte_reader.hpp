#pragma once

#include "domain/result.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace music_service::audio_processing::format {

// Random-access reads through one bounded window, so a file is never loaded whole.
class ByteReader {
public:
    static constexpr std::size_t window_bytes = 64 * 1024;

    static domain::Result<ByteReader> open(const std::filesystem::path& path);

    [[nodiscard]] std::int64_t size() const noexcept { return size_; }

    // Up to count bytes at offset; fewer only at the end of the file. A read failure is
    // FILE_SYSTEM_ERROR.
    domain::Result<std::string> read(std::int64_t offset, std::size_t count);

private:
    ByteReader(std::ifstream stream, std::int64_t size);

    std::ifstream stream_;
    std::int64_t size_{0};
    std::vector<char> window_;
    std::int64_t window_offset_{0};
    std::size_t window_size_{0};
};

} // namespace music_service::audio_processing::format
