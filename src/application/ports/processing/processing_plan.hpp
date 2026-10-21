#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace music_service::application::ports {

// The processor a resolved resource needs (internal-contracts.md 3.3): AudioProcessor
// implementations are selected by id and version, never by platform name.
struct ProcessingPlan {
    std::string processor_id{"audio.verify"};
    std::uint32_t processor_version{2};
    // A required decryption step by AudioDecryptor id (e.g. a platform's file cipher);
    // none for plain files. An unknown id is never skipped: the file is not published.
    std::optional<std::string> decryption{};

    friend bool operator==(const ProcessingPlan&, const ProcessingPlan&) = default;
};

} // namespace music_service::application::ports
