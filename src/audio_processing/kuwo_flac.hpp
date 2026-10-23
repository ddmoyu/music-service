#pragma once

#include "application/ports/processing/audio_processor.hpp"

namespace music_service::audio_processing {

// 酷我黑胶是原生 FLAC；仅在完整帧及 PCM 校验通过后清理已识别尾部，保留原下载文件。
class KuwoFlacProcessor final : public application::ports::AudioProcessor {
public:
    [[nodiscard]] application::ports::ProcessingPlan plan() const override;
    domain::Result<application::ports::ProcessingReport> process(
        const application::ports::ProcessingRequest& request,
        const application::ports::OperationContext& context) override;
};

} // namespace music_service::audio_processing
