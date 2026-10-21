#pragma once

#include "application/ports/processing/audio_processor.hpp"
#include "domain/assets/container.hpp"
#include "domain/result.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace music_service::audio_processing::tags {

// What a write left out; empty when every given item was written and read back.
struct TagWriteResult {
    std::vector<std::string> missing; // "title", "artist", "album", "lyrics", "cover"
};

// Writes the metadata into the file in place with TagLib: ID3v2.4 for MP3 (an ID3v1,
// APE or other tag already there is kept, not stripped), Vorbis comments and a FLAC
// picture for FLAC, iTunes atoms for MP4. Only the items metadata carries are touched;
// other frames and fields stay. Writing the same metadata again gives the same tags.
// Each item is read back after saving. Fails with METADATA_FAILED when the file cannot be
// opened or saved, CAPABILITY_UNSUPPORTED for another container.
domain::Result<TagWriteResult> write_tags(const std::filesystem::path& file, domain::Container container,
                                          const application::ports::TrackMetadata& metadata);

} // namespace music_service::audio_processing::tags
