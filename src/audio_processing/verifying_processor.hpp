#pragma once

#include "application/ports/processing/audio_processor.hpp"

#include <memory>
#include <string>
#include <vector>

namespace music_service::audio_processing {

// Baseline processor "audio.verify" v2: identifies the container from the bytes,
// validates the structure of MP3, FLAC and MP4/M4A and keeps the original encoding. Other
// containers, HTML/JSON bodies and cut-off files fail with AUDIO_INVALID. Unless
// metadata_policy is skip, the trusted metadata is written into a tagged copy beside the
// input (tags/tag_writer), whose audio payload must measure the same as the input's; that
// copy is the output. A required tag that cannot be written is METADATA_FAILED; with
// best_effort the untagged input is the output and the report warns.
class VerifyingProcessor final : public application::ports::AudioProcessor {
public:
    [[nodiscard]] application::ports::ProcessingPlan plan() const override;
    domain::Result<application::ports::ProcessingReport>
    process(const application::ports::ProcessingRequest& request,
            const application::ports::OperationContext& context) override;
};

// The registered processors, looked up by the id and version a resolved resource names.
class ProcessorCatalog {
public:
    // Duplicate id and version pairs are rejected.
    static domain::Result<ProcessorCatalog>
    create(std::vector<std::shared_ptr<application::ports::AudioProcessor>> processors);
    // The processors built into this service.
    static ProcessorCatalog builtin();

    // An unknown plan is CAPABILITY_UNSUPPORTED: the file cannot be processed as required.
    [[nodiscard]] domain::Result<std::shared_ptr<application::ports::AudioProcessor>>
    find(const application::ports::ProcessingPlan& plan) const;

private:
    std::vector<std::shared_ptr<application::ports::AudioProcessor>> processors_;
};

} // namespace music_service::audio_processing
