#pragma once

#include "domain/result.hpp"

#include <chrono>
#include <string>
#include <vector>

namespace music_service::application::ports {

// Immutable binding of a destination id to the canonical root it was first registered
// with (configuration-and-deployment.md, 目标目录绑定与变更).
struct DestinationBinding {
    std::string id;
    std::string canonical_root; // UTF-8
    std::chrono::system_clock::time_point registered_at;
};

class DestinationRepository {
public:
    virtual ~DestinationRepository() = default;

    // Registers a new id, or confirms an existing one with the same root. A different
    // root for a known id is DESTINATION_ROOT_CHANGED; nothing is modified.
    virtual domain::Result<DestinationBinding> bind(const std::string& id,
                                                    const std::string& canonical_root) = 0;
    virtual domain::Result<DestinationBinding> get(const std::string& id) = 0;
    virtual domain::Result<std::vector<DestinationBinding>> list() = 0;
    // Whether tasks, file assets or publication intents still reference the id, so a
    // removed configuration entry can be rejected with DESTINATION_IN_USE.
    virtual domain::Result<bool> in_use(const std::string& id) = 0;
};

} // namespace music_service::application::ports
