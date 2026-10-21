#pragma once

#include <string>

namespace music_service::application::ports {

// The download scheduler as commands see it (internal-contracts.md 4, DownloadQueuePort
// and TaskControlPort). Both calls are hints: the persisted task state decides, and a
// periodic scan covers a lost wake-up.
class DownloadQueue {
public:
    virtual ~DownloadQueue() = default;

    // New or newly due work was committed.
    virtual void wake() = 0;

    // A cancel was committed (canceling): signal the running execution of this task to
    // stop at once. Not the cancellation itself; the runner finishes it.
    virtual void request_stop(const std::string& task_id) = 0;
};

} // namespace music_service::application::ports
