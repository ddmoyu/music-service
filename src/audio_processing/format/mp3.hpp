#pragma once

#include "application/ports/runtime/operation_context.hpp"
#include "audio_processing/format/byte_reader.hpp"
#include "domain/result.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace music_service::audio_processing::format {

// Structure of an MPEG audio file (MP3 and the other layers), read frame by frame
// without decoding. Tags (ID3v2 in front; ID3v1, ID3v2 with a footer, APEv2 and
// Lyrics3v2 at the end) are skipped; the first audio frame must start a chain of
// consistent frames.
struct Mp3Structure {
    std::string codec; // "mpeg1-layer3", "mpeg2-layer3", "mpeg2.5-layer3", ...
    std::int32_t sample_rate_hz{0};
    std::int32_t channels{0};
    std::uint64_t frames{0};      // audio frames, without a Xing/Info/VBRI header frame
    std::int64_t audio_bytes{0};  // bytes of those frames
    std::int64_t duration_ms{0};  // frames x samples per frame / sample rate
    std::int64_t bitrate_bps{0};  // average
    bool variable_bitrate{false}; // frames with different bitrates
    std::int64_t skipped_bytes{0}; // damaged bytes skipped between or after frames
    std::optional<std::uint64_t> declared_frames; // from a Xing/Info or VBRI header
};

// Fails with AUDIO_INVALID and details {"audio", reason} for: "no_audio_frames",
// "truncated" (a cut tag, first or final frame, or fewer frames than the header
// declares) and "damaged" (too many bytes that are not frames). Checks cancellation and
// the deadline on entry and every few hundred frames, tags or kilobytes searched.
domain::Result<Mp3Structure> scan_mp3(ByteReader& reader, const application::ports::OperationContext& context);

} // namespace music_service::audio_processing::format
