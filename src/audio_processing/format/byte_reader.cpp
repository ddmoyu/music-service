#include "audio_processing/format/byte_reader.hpp"

#include <algorithm>
#include <system_error>
#include <utility>

namespace music_service::audio_processing::format {

namespace {

domain::Error io_error(std::string message) {
    return domain::Error{domain::ErrorCode::file_system, std::move(message), true, {}};
}

} // namespace

ByteReader::ByteReader(std::ifstream stream, std::int64_t size)
    : stream_(std::move(stream)), size_(size), window_(window_bytes) {}

domain::Result<ByteReader> ByteReader::open(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return io_error("the working file cannot be read");
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return io_error("the working file cannot be opened");
    }
    return ByteReader{std::move(stream), static_cast<std::int64_t>(size)};
}

domain::Result<std::string> ByteReader::read(std::int64_t offset, std::size_t count) {
    if (offset < 0 || offset >= size_) {
        return std::string{};
    }
    count = static_cast<std::size_t>(std::min<std::int64_t>(static_cast<std::int64_t>(count), size_ - offset));
    const auto window_end = window_offset_ + static_cast<std::int64_t>(window_size_);
    if (offset < window_offset_ || offset + static_cast<std::int64_t>(count) > window_end) {
        if (count > window_bytes) {
            return io_error("read exceeds the window");
        }
        stream_.clear();
        stream_.seekg(offset);
        const auto wanted = static_cast<std::size_t>(std::min<std::int64_t>(window_bytes, size_ - offset));
        stream_.read(window_.data(), static_cast<std::streamsize>(wanted));
        if (static_cast<std::size_t>(stream_.gcount()) != wanted) {
            return io_error("the working file changed or cannot be read");
        }
        window_offset_ = offset;
        window_size_ = wanted;
    }
    const auto start = static_cast<std::size_t>(offset - window_offset_);
    return std::string{window_.data() + start, count};
}

} // namespace music_service::audio_processing::format
