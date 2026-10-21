#pragma once

#include "application/ports/runtime/operation_context.hpp"
#include "domain/result.hpp"

#include <filesystem>
#include <string>

namespace music_service::application::ports {

// A platform's required file decryption (feature-modules.md 10). It reads the received
// file and writes the plain audio to output; the processor validates that output as if
// it had been downloaded. Implementations arrive with the platforms that need them
// (N08, N09); none are built in yet, so a plan naming one is CAPABILITY_UNSUPPORTED.
// A failure is AUDIO_INVALID with details {"audio", "decrypt:<reason>"}.
class AudioDecryptor {
public:
    virtual ~AudioDecryptor() = default;
    [[nodiscard]] virtual std::string id() const = 0;
    virtual domain::Result<void> decrypt(const std::filesystem::path& input, const std::filesystem::path& output,
                                         const OperationContext& context) = 0;
};

} // namespace music_service::application::ports
