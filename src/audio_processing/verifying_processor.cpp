#include "audio_processing/verifying_processor.hpp"

#include "audio_processing/format/byte_reader.hpp"
#include "audio_processing/format/mp3.hpp"
#include "domain/assets/container.hpp"

#include <mbedtls/sha256.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <utility>

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

domain::Result<ports::ProcessingReport> VerifyingProcessor::process(const ports::ProcessingRequest& request,
                                                                    const ports::OperationContext& context) {
    if (request.metadata_policy == domain::MetadataPolicy::required) {
        Error error{ErrorCode::capability_unsupported, "required metadata cannot be written yet", false, {}};
        error.details.push_back({"metadata_policy", "tags are not written by this service version"});
        return error;
    }
    auto reader = format::ByteReader::open(request.input);
    if (!reader) {
        return std::move(reader).error();
    }
    const auto size = reader->size();
    auto head = reader->read(0, domain::sniff_bytes);
    if (!head) {
        return std::move(head).error();
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
    if (*sniffed.container != domain::Container::mp3) {
        return invalid("unsupported_container:" + std::string{domain::to_string(*sniffed.container)},
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
    auto structure = format::scan_mp3(*reader, context);
    if (!structure) {
        return std::move(structure).error();
    }

    ports::ProcessingReport report;
    const auto plan_value = plan();
    report.processor_id = plan_value.processor_id;
    report.processor_version = plan_value.processor_version;
    report.output = request.input; // the original encoding is kept
    auto& facts = report.facts;
    facts.container = std::string{domain::to_string(*sniffed.container)};
    facts.codec = structure->codec;
    facts.sample_rate_hz = structure->sample_rate_hz;
    facts.channels = structure->channels;
    facts.bitrate_bps = structure->bitrate_bps;
    facts.variable_bitrate = structure->variable_bitrate;
    facts.duration_ms = structure->duration_ms;
    facts.size_bytes = size;
    facts.audio_bytes = structure->audio_bytes;
    facts.frames = structure->frames;
    facts.sha256 = std::move(digest).value(); // the output is the input, unchanged

    const auto warn = [&](std::string_view code) { report.warnings.emplace_back(code); };
    if (structure->skipped_bytes > 0) {
        warn(ports::processing_warning::damaged_data_skipped);
    }
    const auto& expected = request.expected;
    if (expected.container && *expected.container != facts.container) {
        warn(ports::processing_warning::container_mismatch);
    }
    if (expected.bitrate_bps && *expected.bitrate_bps > 0 && !facts.variable_bitrate &&
        std::llabs(*facts.bitrate_bps - *expected.bitrate_bps) * 10 > *expected.bitrate_bps) {
        warn(ports::processing_warning::bitrate_mismatch);
    }

    // The measured length against the declared one. A clearly shorter file is a clip, a
    // matching one is full-length evidence; a known preview is never upgraded.
    report.completeness = expected.completeness;
    if (expected.duration_ms && *expected.duration_ms > 0) {
        const auto declared = *expected.duration_ms;
        const auto tolerance = duration_tolerance_ms(declared);
        if (structure->duration_ms + tolerance < declared) {
            report.completeness = domain::Completeness::preview;
            warn(ports::processing_warning::duration_shorter);
        } else if (structure->duration_ms > declared + tolerance) {
            warn(ports::processing_warning::duration_mismatch);
        } else if (report.completeness != domain::Completeness::preview) {
            report.completeness = domain::Completeness::full;
        }
    }

    report.stages.push_back({"decrypt", ports::StageOutcome::skipped});
    report.stages.push_back({"validate", ports::StageOutcome::done});
    report.stages.push_back({"tags", ports::StageOutcome::skipped});
    if (request.metadata_policy == domain::MetadataPolicy::best_effort) {
        warn(ports::processing_warning::metadata_not_written);
    }
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
