#pragma once

#include "domain/result.hpp"

#include <functional>
#include <stop_token>

namespace music_service::application::ports {

// A bounded pool that runs work later on its own threads (runtime/executors).
// submit() never blocks: a full or stopping pool is SERVICE_BUSY at once. Work gets
// the pool's stop token and checks it at safe points.
class WorkExecutor {
public:
    using Work = std::function<void(std::stop_token)>;

    virtual ~WorkExecutor() = default;
    virtual domain::Result<void> submit(Work work) = 0;
};

} // namespace music_service::application::ports
