#pragma once

#include "application/ports/processing/audio_processor.hpp"
#include "application/ports/processing/decryptor.hpp"
#include "application/ports/storage/secret_store.hpp"

namespace music_service::audio_processing {

// 文件密钥仅存内存；旧 MG3D 无外部密钥时，尝试有界的 WAV 零填充序列恢复。
class MiguZ3dDecryptor final : public application::ports::AudioDecryptor {
  public:
    explicit MiguZ3dDecryptor(std::shared_ptr<const application::ports::SecretBytes> key = {})
        : key_(std::move(key)) {}
    [[nodiscard]] std::string id() const override { return "migu.z3d"; }
    domain::Result<void> decrypt(const std::filesystem::path& input,
                                 const std::filesystem::path& output,
                                 const application::ports::OperationContext& context) override;

  private:
    std::shared_ptr<const application::ports::SecretBytes> key_;
};

class MiguZ3dProcessor final : public application::ports::AudioProcessor {
  public:
    [[nodiscard]] application::ports::ProcessingPlan plan() const override;
    domain::Result<application::ports::ProcessingReport>
    process(const application::ports::ProcessingRequest& request,
            const application::ports::OperationContext& context) override;
};

} // namespace music_service::audio_processing
