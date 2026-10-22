#pragma once

#include "application/ports/processing/audio_processor.hpp"
#include "application/ports/processing/decryptor.hpp"

namespace music_service::audio_processing {

// Bounded native decoder. The caller owns a private workspace; input is never modified.
class NcmDecryptor final : public application::ports::AudioDecryptor {
public:
    [[nodiscard]] std::string id() const override { return "netease.ncm"; }
    domain::Result<void> decrypt(const std::filesystem::path& input, const std::filesystem::path& output,
                                 const application::ports::OperationContext& context) override;
    domain::Result<application::ports::TrackMetadata>
    decode(const std::filesystem::path& input, const std::filesystem::path& output,
           const application::ports::OperationContext& context, std::uint64_t max_bytes = 2ULL * 1024 * 1024 * 1024);
};

class NcmProcessor final : public application::ports::AudioProcessor {
public:
    [[nodiscard]] application::ports::ProcessingPlan plan() const override;
    domain::Result<application::ports::ProcessingReport>
    process(const application::ports::ProcessingRequest& request,
            const application::ports::OperationContext& context) override;
};

} // namespace music_service::audio_processing
