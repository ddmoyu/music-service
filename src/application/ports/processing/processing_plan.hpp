#pragma once

#include <cstdint>
#include <string>

namespace music_service::application::ports {

// The processor a resolved resource needs (internal-contracts.md 3.3): AudioProcessor
// implementations are selected by id and version, never by platform name.
struct ProcessingPlan {
    std::string processor_id{"audio.verify"};
    std::uint32_t processor_version{1};

    friend bool operator==(const ProcessingPlan&, const ProcessingPlan&) = default;
};

} // namespace music_service::application::ports
