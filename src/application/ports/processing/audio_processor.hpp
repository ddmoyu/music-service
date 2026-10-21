#pragma once

#include "application/ports/processing/processing_plan.hpp"
#include "application/ports/runtime/operation_context.hpp"
#include "domain/assets/quality.hpp"
#include "domain/result.hpp"
#include "domain/tasks/download_intent.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace music_service::application::ports {

// What resolve and the transfer claimed about the file. Every field is optional evidence
// to compare the bytes with; none of it is trusted on its own.
struct AudioExpectation {
    std::optional<std::string> container;    // declared by the provider ("mp3", ...)
    std::optional<std::int64_t> bitrate_bps; // declared bitrate
    std::optional<std::int64_t> duration_ms; // declared length of the song
    std::optional<std::int64_t> size_bytes;  // total size the transfer announced
    domain::Completeness completeness{domain::Completeness::unknown};
};

struct ProcessingRequest {
    // The transfer-complete working file. Validation reads it and never modifies it, so a
    // retry can process the same file again (runtime-and-tasks.md, processing recovery).
    std::filesystem::path input;
    AudioExpectation expected;
    domain::MetadataPolicy metadata_policy{domain::MetadataPolicy::best_effort};
};

// Facts measured from the bytes. Unknown values stay empty, never estimated.
struct AudioFacts {
    std::string container;           // "mp3", ...
    std::optional<std::string> codec; // e.g. "mpeg1-layer3"
    std::optional<std::int32_t> sample_rate_hz;
    std::optional<std::int32_t> channels;
    std::optional<std::int64_t> bitrate_bps; // average over the audio frames
    bool variable_bitrate{false};
    std::optional<std::int64_t> duration_ms;
    std::int64_t size_bytes{0};  // the whole file
    std::int64_t audio_bytes{0}; // audio frames, without tags
    std::uint64_t frames{0};
    // SHA-256 (lowercase hex) of the output bytes the checks were made on; publication
    // refuses a file whose bytes no longer match it.
    std::string sha256;
};

enum class StageOutcome { done, skipped, failed };

struct StageReport {
    std::string stage; // "decrypt", "validate", "tags" (data-models.md, stage)
    StageOutcome outcome{StageOutcome::skipped};
};

// Warning codes a processor may report next to a successful result.
namespace processing_warning {
inline constexpr std::string_view container_mismatch = "CONTAINER_MISMATCH";
inline constexpr std::string_view bitrate_mismatch = "BITRATE_MISMATCH";
inline constexpr std::string_view duration_mismatch = "DURATION_MISMATCH";
inline constexpr std::string_view duration_shorter = "DURATION_SHORTER_THAN_TRACK";
inline constexpr std::string_view damaged_data_skipped = "DAMAGED_DATA_SKIPPED";
inline constexpr std::string_view metadata_not_written = "METADATA_NOT_WRITTEN";
} // namespace processing_warning

struct ProcessingReport {
    std::string processor_id;
    std::uint32_t processor_version{0};
    std::filesystem::path output; // the file to publish; the input when left unchanged
    AudioFacts facts;
    // The input expectation updated by the measured length: a clearly shorter file is a
    // preview, a matching length is full-length evidence, otherwise unchanged.
    domain::Completeness completeness{domain::Completeness::unknown};
    std::vector<std::string> warnings;
    std::vector<StageReport> stages;
};

// Required decryption and validation, optional tags (feature-modules.md 10). A failed
// required step returns AUDIO_INVALID with details {"audio", reason}; such a file must not
// be published. Cancellation and the deadline are checked while reading.
class AudioProcessor {
public:
    virtual ~AudioProcessor() = default;
    [[nodiscard]] virtual ProcessingPlan plan() const = 0;
    virtual domain::Result<ProcessingReport> process(const ProcessingRequest& request,
                                                     const OperationContext& context) = 0;
};

} // namespace music_service::application::ports
