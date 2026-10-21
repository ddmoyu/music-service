#include "audio_processing/format/flac.hpp"

#include <chrono>
#include <string>
#include <utility>

namespace music_service::audio_processing::format {

namespace {

using domain::Error;
using domain::ErrorCode;

constexpr std::uint32_t streaminfo_type = 0;
constexpr std::uint32_t streaminfo_length = 34;
constexpr int max_blocks = 4096; // far more than any encoder writes

Error invalid(std::string reason, std::string message, bool retryable) {
    Error error{ErrorCode::audio_invalid, std::move(message), retryable, {}};
    error.details.push_back({"audio", std::move(reason)});
    return error;
}

unsigned byte_at(const std::string& bytes, std::size_t index) { return static_cast<unsigned char>(bytes[index]); }

std::uint32_t be24(const std::string& bytes, std::size_t at) {
    return (std::uint32_t{byte_at(bytes, at)} << 16) | (std::uint32_t{byte_at(bytes, at + 1)} << 8) |
           std::uint32_t{byte_at(bytes, at + 2)};
}

std::optional<Error> interrupted(const application::ports::OperationContext& context) {
    if (context.stop.stop_requested()) {
        return Error{ErrorCode::canceled, "audio validation was canceled", false, {}};
    }
    if (context.expired(std::chrono::steady_clock::now())) {
        return Error{ErrorCode::deadline_exceeded, "audio validation exceeded its deadline", true, {}};
    }
    return std::nullopt;
}

// The length of an ID3v2 tag at offset 0 (header, synchsafe size, optional footer), or 0.
domain::Result<std::int64_t> id3v2_prefix(ByteReader& reader) {
    auto head = reader.read(0, 10);
    if (!head) {
        return std::move(head).error();
    }
    if (head->size() < 10 || head->compare(0, 3, "ID3") != 0) {
        return std::int64_t{0};
    }
    const std::int64_t size = (std::int64_t{byte_at(*head, 6) & 0x7FU} << 21) |
                              (std::int64_t{byte_at(*head, 7) & 0x7FU} << 14) |
                              (std::int64_t{byte_at(*head, 8) & 0x7FU} << 7) | std::int64_t{byte_at(*head, 9) & 0x7FU};
    const bool footer = (byte_at(*head, 5) & 0x10U) != 0;
    return 10 + size + (footer ? 10 : 0);
}

} // namespace

domain::Result<FlacStructure> scan_flac(ByteReader& reader, const application::ports::OperationContext& context) {
    if (auto stopped = interrupted(context)) {
        return std::move(*stopped);
    }
    auto prefix = id3v2_prefix(reader);
    if (!prefix) {
        return std::move(prefix).error();
    }
    auto offset = prefix.value();
    if (offset + 4 > reader.size()) {
        return invalid("truncated", "the ID3v2 tag is cut off", true);
    }
    auto marker = reader.read(offset, 4);
    if (!marker) {
        return std::move(marker).error();
    }
    if (*marker != "fLaC") {
        return invalid("damaged", "the FLAC stream marker is missing", false);
    }
    offset += 4;

    FlacStructure flac;
    std::uint32_t min_frame_bytes = 0;
    std::uint32_t max_block_samples = 0;
    bool last = false;
    for (int block = 0; !last; ++block) {
        if (block == max_blocks) {
            return invalid("damaged", "the FLAC metadata block chain does not end", false);
        }
        if (auto stopped = interrupted(context)) {
            return std::move(*stopped);
        }
        auto header = reader.read(offset, 4);
        if (!header) {
            return std::move(header).error();
        }
        if (header->size() < 4) {
            return invalid("truncated", "a FLAC metadata block is cut off", true);
        }
        last = (byte_at(*header, 0) & 0x80U) != 0;
        const auto type = byte_at(*header, 0) & 0x7FU;
        const auto length = be24(*header, 1);
        if (type == 127) {
            return invalid("damaged", "a FLAC metadata block has an invalid type", false);
        }
        if ((block == 0) != (type == streaminfo_type)) {
            return invalid("damaged", "STREAMINFO is not the first FLAC metadata block", false);
        }
        if (offset + 4 + length > reader.size()) {
            return invalid("truncated", "a FLAC metadata block is cut off", true);
        }
        if (type == streaminfo_type) {
            if (length != streaminfo_length) {
                return invalid("damaged", "the FLAC STREAMINFO block has the wrong size", false);
            }
            auto info = reader.read(offset + 4, streaminfo_length);
            if (!info) {
                return std::move(info).error();
            }
            max_block_samples = (std::uint32_t{byte_at(*info, 2)} << 8) | std::uint32_t{byte_at(*info, 3)};
            min_frame_bytes = be24(*info, 4);
            // 20 bits sample rate, 3 bits channels - 1, 5 bits bits per sample - 1, 36 bits samples.
            const auto packed = (std::uint64_t{be24(*info, 10)} << 40) | (std::uint64_t{be24(*info, 13)} << 16) |
                                (std::uint64_t{byte_at(*info, 16)} << 8) | std::uint64_t{byte_at(*info, 17)};
            flac.sample_rate_hz = static_cast<std::int32_t>(packed >> 44);
            flac.channels = static_cast<std::int32_t>(((packed >> 41) & 0x7U) + 1);
            flac.bits_per_sample = static_cast<std::int32_t>(((packed >> 36) & 0x1FU) + 1);
            const auto samples = packed & 0xFFFFFFFFFULL;
            if (flac.sample_rate_hz == 0) {
                return invalid("damaged", "the FLAC sample rate is zero", false);
            }
            if (samples > 0) {
                flac.total_samples = samples;
                flac.duration_ms =
                    static_cast<std::int64_t>(samples * 1000 / static_cast<std::uint64_t>(flac.sample_rate_hz));
            }
        }
        offset += 4 + length;
    }

    auto sync = reader.read(offset, 2);
    if (!sync) {
        return std::move(sync).error();
    }
    if (sync->size() < 2 || byte_at(*sync, 0) != 0xFFU || (byte_at(*sync, 1) & 0xFEU) != 0xF8U) {
        return invalid("no_audio_frames", "no FLAC audio frame follows the metadata", sync->size() < 2);
    }
    flac.audio_offset = offset;
    auto end = reader.size();
    if (end - offset >= 128) {
        auto tail = reader.read(end - 128, 3);
        if (!tail) {
            return std::move(tail).error();
        }
        if (*tail == "TAG") {
            end -= 128;
        }
    }
    flac.audio_bytes = end - offset;
    // Every frame holds at least min_frame_bytes: far fewer bytes than the frames the
    // samples need is a cut file. Encoders that do not record sizes leave this unchecked.
    if (flac.total_samples && min_frame_bytes > 0 && max_block_samples > 0) {
        const auto frames = (*flac.total_samples + max_block_samples - 1) / max_block_samples;
        if (static_cast<std::uint64_t>(flac.audio_bytes) < frames * min_frame_bytes) {
            return invalid("truncated", "the FLAC audio is shorter than its frames need", true);
        }
    }
    return flac;
}

} // namespace music_service::audio_processing::format
