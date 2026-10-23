#pragma once

#include "application/ports/runtime/operation_context.hpp"
#include "audio_processing/format/byte_reader.hpp"
#include "domain/result.hpp"

#include <cstdint>
#include <array>
#include <optional>

namespace music_service::audio_processing::format {

// Structure of a native FLAC file, without decoding: an optional ID3v2 tag in front, the
// "fLaC" marker, the metadata block chain (STREAMINFO first) and the first audio frame's
// sync code. Lengths come from STREAMINFO; audio frames are not walked one by one.
struct FlacStructure {
    std::int32_t sample_rate_hz{0};
    std::int32_t channels{0};
    std::int32_t bits_per_sample{0};
    std::optional<std::uint64_t> total_samples; // 0 in STREAMINFO means unknown
    std::optional<std::int64_t> duration_ms;
    std::int64_t audio_offset{0}; // the first audio frame
    std::int64_t audio_bytes{0};  // from there to the end, without a trailing ID3v1 tag
    std::uint32_t min_block_samples{0};
    std::uint32_t max_block_samples{0};
    std::array<unsigned char, 16> pcm_md5{};
};

// Fails with AUDIO_INVALID and details {"audio", reason}: "truncated" (a cut metadata
// block, or far fewer audio bytes than STREAMINFO's frame sizes allow), "damaged" (no
// STREAMINFO, a malformed block chain) or "no_audio_frames" (no frame sync after the
// metadata). Checks cancellation and the deadline between metadata blocks.
domain::Result<FlacStructure> scan_flac(ByteReader& reader, const application::ports::OperationContext& context);

} // namespace music_service::audio_processing::format
