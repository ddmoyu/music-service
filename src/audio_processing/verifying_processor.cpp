#include "audio_processing/verifying_processor.hpp"
#include "audio_processing/ncm.hpp"
#include "audio_processing/kuwo_flac.hpp"
#include "audio_processing/migu_z3d.hpp"

#include "audio_processing/format/byte_reader.hpp"
#include "audio_processing/format/flac.hpp"
#include "audio_processing/format/mp3.hpp"
#include "audio_processing/format/mp4.hpp"
#include "audio_processing/format/wav.hpp"
#include "audio_processing/tags/tag_writer.hpp"
#include "domain/assets/container.hpp"

#include <mbedtls/sha256.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace music_service::audio_processing {

namespace ports = application::ports;
using domain::Error;
using domain::ErrorCode;

namespace {

Error invalid(std::string reason, std::string message, bool retryable) {
    Error error{ErrorCode::audio_invalid, std::move(message), retryable, {}};
    error.details.push_back({"audio", std::move(reason)});
    return error;
}

// Declared lengths are whole seconds and encoders add padding: a few seconds or 3%.
std::int64_t duration_tolerance_ms(std::int64_t declared_ms) {
    return std::max<std::int64_t>(3000, declared_ms * 3 / 100);
}

// SHA-256 of every byte through the reader that the checks then use, so the report names
// the bytes that were checked. Cancellation and the deadline are checked between windows.
domain::Result<std::string> sha256_of(format::ByteReader& reader, const ports::OperationContext& context) {
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    std::optional<Error> failure;
    for (std::int64_t offset = 0; offset < reader.size();) {
        if (context.stop.stop_requested()) {
            failure = Error{ErrorCode::canceled, "audio validation was canceled", false, {}};
            break;
        }
        if (context.expired(std::chrono::steady_clock::now())) {
            failure = Error{ErrorCode::deadline_exceeded, "audio validation exceeded its deadline", true, {}};
            break;
        }
        auto bytes = reader.read(offset, format::ByteReader::window_bytes);
        if (!bytes || bytes->empty()) {
            failure = bytes ? Error{ErrorCode::file_system, "the working file changed while it was read", true, {}}
                            : std::move(bytes).error();
            break;
        }
        mbedtls_sha256_update(&sha, reinterpret_cast<const unsigned char*>(bytes->data()), bytes->size());
        offset += static_cast<std::int64_t>(bytes->size());
    }
    std::array<unsigned char, 32> digest{};
    mbedtls_sha256_finish(&sha, digest.data());
    mbedtls_sha256_free(&sha);
    if (failure) {
        return std::move(*failure);
    }
    static constexpr char digits[] = "0123456789abcdef";
    std::string hex;
    for (const auto byte : digest) {
        hex += digits[byte >> 4U];
        hex += digits[byte & 0x0FU];
    }
    return hex;
}

} // namespace

ports::ProcessingPlan VerifyingProcessor::plan() const { return ports::ProcessingPlan{}; }

namespace {

// What the container's structure says, before any tag is written.
struct Measured {
    std::optional<std::int32_t> bits_per_sample;
    std::optional<std::string> codec;
    std::optional<std::int32_t> sample_rate_hz;
    std::optional<std::int32_t> channels;
    std::optional<std::int64_t> bitrate_bps;
    bool variable_bitrate{false};
    std::optional<std::int64_t> duration_ms;
    std::int64_t audio_bytes{0};
    std::uint64_t frames{0};
    std::int64_t skipped_bytes{0};
    std::optional<std::string> payload_sha256;

    // Tags may move the audio but never change it: the same frames, bytes and length.
    [[nodiscard]] bool same_payload(const Measured& other) const {
        return frames == other.frames && audio_bytes == other.audio_bytes && duration_ms == other.duration_ms &&
               payload_sha256 == other.payload_sha256;
    }
};

std::optional<std::int64_t> average_bitrate(std::int64_t bytes, std::optional<std::int64_t> duration_ms) {
    if (!duration_ms || *duration_ms <= 0) {
        return std::nullopt;
    }
    return bytes * 8 * 1000 / *duration_ms;
}

domain::Result<Measured> measure(format::ByteReader& reader, domain::Container container,
                                 const ports::OperationContext& context) {
    Measured m;
    switch (container) {
    case domain::Container::wav: {
        auto wav = format::scan_wav(reader, context);
        if (!wav) return std::move(wav).error();
        m.codec = wav->codec;
        m.bits_per_sample = wav->bits_per_sample;
        m.sample_rate_hz = wav->sample_rate_hz;
        m.channels = wav->channels;
        m.bitrate_bps = wav->bitrate_bps;
        m.duration_ms = wav->duration_ms;
        m.audio_bytes = wav->audio_bytes;
        m.frames = wav->samples;
        m.payload_sha256 = std::move(wav->payload_sha256);
        return m;
    }
    case domain::Container::mp3: {
        auto mp3 = format::scan_mp3(reader, context);
        if (!mp3) {
            return std::move(mp3).error();
        }
        m.codec = mp3->codec;
        m.sample_rate_hz = mp3->sample_rate_hz;
        m.channels = mp3->channels;
        m.bitrate_bps = mp3->bitrate_bps;
        m.variable_bitrate = mp3->variable_bitrate;
        m.duration_ms = mp3->duration_ms;
        m.audio_bytes = mp3->audio_bytes;
        m.frames = mp3->frames;
        m.skipped_bytes = mp3->skipped_bytes;
        return m;
    }
    case domain::Container::flac: {
        auto flac = format::scan_flac(reader, context);
        if (!flac) {
            return std::move(flac).error();
        }
        m.codec = "flac";
        m.bits_per_sample = flac->bits_per_sample;
        m.sample_rate_hz = flac->sample_rate_hz;
        m.channels = flac->channels;
        m.duration_ms = flac->duration_ms;
        m.audio_bytes = flac->audio_bytes;
        m.frames = flac->total_samples.value_or(0); // samples: the payload's own count
        m.bitrate_bps = average_bitrate(flac->audio_bytes, flac->duration_ms);
        m.variable_bitrate = true; // lossless frames vary in size
        return m;
    }
    case domain::Container::mp4: {
        auto mp4 = format::scan_mp4(reader, context);
        if (!mp4) {
            return std::move(mp4).error();
        }
        m.codec = mp4->codec;
        m.duration_ms = mp4->duration_ms;
        m.audio_bytes = mp4->media_bytes;
        m.bitrate_bps = average_bitrate(mp4->media_bytes, mp4->duration_ms);
        m.variable_bitrate = true; // not measured per frame
        return m;
    }
    default:
        return invalid("unsupported_container:" + std::string{domain::to_string(container)},
                       "this container cannot be validated yet", false);
    }
}

bool has_items(const ports::TrackMetadata& metadata) {
    const bool artists = std::any_of(metadata.artists.begin(), metadata.artists.end(),
                                     [](const std::string& artist) { return !artist.empty(); });
    return !metadata.title.empty() || artists || (metadata.album && !metadata.album->empty()) ||
           (metadata.lyrics && !metadata.lyrics->empty()) || (metadata.cover && !metadata.cover->data.empty()) ||
           (metadata.track_number && *metadata.track_number > 0);
}

Error metadata_failed(std::string reason, std::string message) {
    Error error{ErrorCode::metadata_failed, std::move(message), false, {}};
    error.details.push_back({"tags", std::move(reason)});
    return error;
}

// The tagged copy beside the input: the input stays as received, so processing again
// after a crash or retry starts from the same bytes.
std::filesystem::path tagged_path(const std::filesystem::path& input, domain::Container container) {
    return input.parent_path() / ("tagged." + std::string{domain::to_string(container)});
}

struct Tagged {
    std::filesystem::path path;
    std::vector<std::string> missing;
};

// Copies, tags and re-measures; the copy is removed again on any failure.
domain::Result<Tagged> tag_copy(const ports::ProcessingRequest& request, domain::Container container,
                                const Measured& before, const ports::OperationContext& context) {
    const auto output = tagged_path(request.input, container);
    std::error_code ec;
    std::filesystem::copy_file(request.input, output, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        return metadata_failed("copy", "the working file could not be copied for tagging");
    }
    const auto discard = [&output] {
        std::error_code ignored;
        std::filesystem::remove(output, ignored);
    };
    auto written = tags::write_tags(output, container, *request.metadata);
    if (!written) {
        discard();
        return std::move(written).error();
    }
    auto reader = format::ByteReader::open(output);
    if (!reader) {
        discard();
        return std::move(reader).error();
    }
    auto after = measure(*reader, container, context);
    if (!after) {
        discard();
        if (after.error().code == ErrorCode::canceled || after.error().code == ErrorCode::deadline_exceeded) {
            return std::move(after).error();
        }
        return metadata_failed("payload_changed", "the tagged file no longer validates");
    }
    if (!before.same_payload(*after)) {
        discard();
        return metadata_failed("payload_changed", "writing tags changed the audio payload");
    }
    return Tagged{output, std::move(written.value().missing)};
}

} // namespace

domain::Result<ports::ProcessingReport> VerifyingProcessor::process(const ports::ProcessingRequest& request,
                                                                    const ports::OperationContext& context) {
    auto reader = format::ByteReader::open(request.input);
    if (!reader) {
        return std::move(reader).error();
    }
    const auto size = reader->size();
    auto head = reader->read(0, domain::sniff_bytes);
    if (!head) {
        return std::move(head).error();
    }

    // A download may contain NCM even when the provider declares MP3/FLAC. The native
    // processor checks the encrypted size and decrypts in this same processing worker;
    // only its plaintext is sent back through verification (and required-tag checks).
    if (head->starts_with("CTENFDAM")) {
        return NcmProcessor{}.process(request, context);
    }
    const bool wants_tags = request.metadata_policy != domain::MetadataPolicy::skip;
    const bool can_tag = request.metadata && has_items(*request.metadata);
    if (request.metadata_policy == domain::MetadataPolicy::required && !can_tag) {
        return metadata_failed("no_metadata", "no trusted metadata is available to write the required tags");
    }

    // An upstream error page or JSON body is not audio and may be transient.
    const auto sniffed = domain::sniff_content(*head);
    switch (sniffed.kind) {
    case domain::ContentKind::audio:
        break;
    case domain::ContentKind::empty:
        return invalid("empty", "the downloaded file is empty", true);
    case domain::ContentKind::markup:
    case domain::ContentKind::json:
    case domain::ContentKind::text:
        return invalid("not_audio:" + std::string{domain::to_string(sniffed.kind)},
                       "the upstream returned a text body instead of audio", true);
    case domain::ContentKind::unknown:
        return invalid("unrecognized", "the downloaded file is not a recognized audio container", false);
    }
    const auto container = *sniffed.container;
    if (container != domain::Container::mp3 && container != domain::Container::flac &&
        container != domain::Container::mp4 && container != domain::Container::wav) {
        return invalid("unsupported_container:" + std::string{domain::to_string(container)},
                       "this container cannot be validated yet", false);
    }
    // The transfer announced a total: fewer bytes is a cut download, more is another file.
    if (request.expected.size_bytes && size != *request.expected.size_bytes) {
        return size < *request.expected.size_bytes
                   ? invalid("truncated", "the file is shorter than the transfer announced", true)
                   : invalid("size_mismatch", "the file is longer than the transfer announced", true);
    }

    // Hashed before the checks: bytes changed later no longer match at publication.
    auto digest = sha256_of(*reader, context);
    if (!digest) {
        return std::move(digest).error();
    }
    auto structure = measure(*reader, container, context);
    if (!structure) {
        return std::move(structure).error();
    }

    ports::ProcessingReport report;
    const auto plan_value = plan();
    report.processor_id = plan_value.processor_id;
    report.processor_version = plan_value.processor_version;
    report.output = request.input; // the original encoding is kept
    auto& facts = report.facts;
    facts.container = std::string{domain::to_string(container)};
    facts.codec = structure->codec;
    facts.sample_rate_hz = structure->sample_rate_hz;
    facts.channels = structure->channels;
    facts.bits_per_sample = structure->bits_per_sample;
    facts.bitrate_bps = structure->bitrate_bps;
    facts.variable_bitrate = structure->variable_bitrate;
    facts.duration_ms = structure->duration_ms;
    facts.size_bytes = size;
    facts.audio_bytes = structure->audio_bytes;
    facts.frames = structure->frames;
    facts.sha256 = std::move(digest).value();

    const auto warn = [&](std::string_view code) { report.warnings.emplace_back(code); };
    if (structure->skipped_bytes > 0) {
        warn(ports::processing_warning::damaged_data_skipped);
    }
    const auto& expected = request.expected;
    if (expected.container && *expected.container != facts.container) {
        warn(ports::processing_warning::container_mismatch);
    }
    if (expected.bitrate_bps && *expected.bitrate_bps > 0 && facts.bitrate_bps && !facts.variable_bitrate &&
        std::llabs(*facts.bitrate_bps - *expected.bitrate_bps) * 10 > *expected.bitrate_bps) {
        warn(ports::processing_warning::bitrate_mismatch);
    }

    // The measured length against the declared one. A clearly shorter file is a clip, a
    // matching one is full-length evidence; a known preview is never upgraded. Without a
    // measured length the completeness stays as declared.
    report.completeness = expected.completeness;
    if (expected.duration_ms && *expected.duration_ms > 0 && structure->duration_ms) {
        const auto declared = *expected.duration_ms;
        const auto measured = *structure->duration_ms;
        const auto tolerance = duration_tolerance_ms(declared);
        if (measured + tolerance < declared) {
            report.completeness = domain::Completeness::preview;
            warn(ports::processing_warning::duration_shorter);
        } else if (measured > declared + tolerance) {
            warn(ports::processing_warning::duration_mismatch);
        } else if (report.completeness != domain::Completeness::preview) {
            report.completeness = domain::Completeness::full;
        }
    }

    report.stages.push_back({"decrypt", ports::StageOutcome::skipped});
    report.stages.push_back({"validate", ports::StageOutcome::done});
    const auto fields = [&](std::string_view status, const std::vector<std::string>& missing = {}) {
        if (!request.metadata) return;
        const auto& m = *request.metadata;
        const std::pair<std::string_view, bool> present[] = {
            {"title", !m.title.empty()}, {"artist", !m.artists.empty()}, {"album", m.album && !m.album->empty()},
            {"cover", m.cover && !m.cover->data.empty()}, {"lyrics", m.lyrics && !m.lyrics->empty()},
            {"track_number", m.track_number && *m.track_number > 0},
            {"track_total", m.track_number && *m.track_number > 0 && m.track_total && *m.track_total >= *m.track_number}};
        for (const auto& [name, exists] : present) {
            if (exists) {
                const auto key = name == "track_total" ? std::string_view{"track_number"} : name;
                const bool failed = std::find(missing.begin(), missing.end(), key) != missing.end();
                report.metadata_fields.emplace_back(name, failed ? "write_failed" : status);
            }
        }
    };
    if (!wants_tags) {
        fields("disabled");
        report.stages.push_back({"tags", ports::StageOutcome::skipped});
        return report;
    }
    if (!can_tag) {
        fields("write_failed");
        report.stages.push_back({"tags", ports::StageOutcome::skipped});
        warn(ports::processing_warning::metadata_not_written);
        return report;
    }
    const bool required = request.metadata_policy == domain::MetadataPolicy::required;
    auto tagged = tag_copy(request, container, *structure, context);
    if (!tagged) {
        const auto code = tagged.error().code;
        if (required || code == ErrorCode::canceled || code == ErrorCode::deadline_exceeded) {
            return std::move(tagged).error();
        }
        report.stages.push_back({"tags", ports::StageOutcome::failed});
        fields("write_failed");
        warn(ports::processing_warning::metadata_not_written);
        return report; // the validated input is published untagged
    }
    if (!tagged->missing.empty()) {
        if (required) {
            std::error_code ignored;
            std::filesystem::remove(tagged->path, ignored);
            std::string items;
            for (const auto& item : tagged->missing) {
                items += (items.empty() ? "" : ",") + item;
            }
            return metadata_failed("unsupported:" + items, "the container cannot hold every required tag");
        }
        warn(ports::processing_warning::metadata_partial);
    }
    // The published bytes are the tagged copy: hash and size are its own.
    auto output = format::ByteReader::open(tagged->path);
    if (!output) {
        return std::move(output).error();
    }
    auto output_digest = sha256_of(*output, context);
    if (!output_digest) {
        return std::move(output_digest).error();
    }
    report.output = tagged->path;
    facts.size_bytes = output->size();
    facts.sha256 = std::move(output_digest).value();
    report.stages.push_back({"tags", ports::StageOutcome::done});
    fields("written", tagged->missing);
    return report;
}

domain::Result<ProcessorCatalog>
ProcessorCatalog::create(std::vector<std::shared_ptr<ports::AudioProcessor>> processors) {
    for (std::size_t i = 0; i < processors.size(); ++i) {
        if (!processors[i]) {
            return Error{ErrorCode::internal, "null audio processor", false, {}};
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (processors[j]->plan() == processors[i]->plan()) {
                return Error{ErrorCode::internal, "duplicate audio processor " + processors[i]->plan().processor_id,
                             false, {}};
            }
        }
    }
    ProcessorCatalog catalog;
    catalog.processors_ = std::move(processors);
    return catalog;
}

ProcessorCatalog ProcessorCatalog::builtin() {
    ProcessorCatalog catalog;
    catalog.processors_.push_back(std::make_shared<VerifyingProcessor>());
    catalog.processors_.push_back(std::make_shared<NcmProcessor>());
    catalog.processors_.push_back(std::make_shared<KuwoFlacProcessor>());
    catalog.processors_.push_back(std::make_shared<MiguZ3dProcessor>());
    return catalog;
}

domain::Result<std::shared_ptr<ports::AudioProcessor>> ProcessorCatalog::find(const ports::ProcessingPlan& plan) const {
    for (const auto& processor : processors_) {
        if (processor->plan() == plan) {
            return processor;
        }
    }
    Error error{ErrorCode::capability_unsupported, "the required audio processor is not available", false, {}};
    error.details.push_back({"processor", plan.processor_id + "@" + std::to_string(plan.processor_version)});
    return error;
}

} // namespace music_service::audio_processing
