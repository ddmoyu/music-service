#pragma once

#include "application/ports/runtime/operation_context.hpp"
#include "audio_processing/format/byte_reader.hpp"
#include "domain/result.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace music_service::audio_processing::format {

// Structure of an MP4/M4A audio file, without decoding: the top-level boxes must tile the
// file exactly (ftyp first, one moov, at least one mdat), and moov must hold a sound
// track. The length comes from mvhd.
struct Mp4Structure {
    std::string major_brand;              // "M4A ", "mp42", "isom", ...
    std::optional<std::string> codec;     // "aac", "alac", "flac", "opus" from the sample entry
    std::optional<std::int64_t> duration_ms;
    std::int64_t media_bytes{0}; // mdat payloads
};

// Fails with AUDIO_INVALID and details {"audio", reason}: "truncated" (a box runs past the
// end of the file), "damaged" (a malformed or missing required box, a moov larger than
// 16 MiB) or "no_audio_track". Checks cancellation and the deadline between boxes.
domain::Result<Mp4Structure> scan_mp4(ByteReader& reader, const application::ports::OperationContext& context);

} // namespace music_service::audio_processing::format
