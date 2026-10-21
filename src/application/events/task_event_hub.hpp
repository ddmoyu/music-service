#pragma once

#include "application/config/service_config.hpp"
#include "domain/result.hpp"
#include "domain/tasks/task_status.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <string>

namespace music_service::application::events {

// A committed public task version (api-contract.md 9.4, task.updated). Only versions the
// repository has committed are published; the event is a projection, not the task.
struct TaskEvent {
    std::string owner_id;
    std::string task_id;
    std::uint64_t version{0};
    domain::TaskStatus status{domain::TaskStatus::queued};
    std::optional<std::string> progress_json; // as committed; null while unknown
    std::chrono::system_clock::time_point updated_at;
};

// Turns an event into one complete SSE message. Supplied by the protocol adapter, so the
// hub counts exactly the bytes a subscriber will be sent.
using EventEncoder = std::function<std::string(const TaskEvent&)>;

enum class CloseReason {
    resync_required, // the subscriber fell behind its bounds or an event was too large
    revoked,         // the token or principal can no longer authenticate
    expired,         // the connection reached its maximum lifetime
    shutdown,        // the service is stopping
};

namespace detail {
struct HubRegistry;
} // namespace detail

// One SSE connection's queue. Destroying it unregisters it and frees its slot.
class Subscription {
public:
    ~Subscription();
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;

    struct Next {
        std::optional<std::string> message;   // the next encoded event
        std::optional<CloseReason> closed;    // the stream must end
    };
    // Waits until a message is queued, the subscription is closed or until passes; both
    // fields empty means the time passed. A closed subscription drops what it still holds.
    Next wait(std::chrono::steady_clock::time_point until);
    void close(CloseReason reason);

    [[nodiscard]] const std::string& owner_id() const noexcept { return owner_id_; }

private:
    friend class TaskEventHub;
    struct Pending {
        std::string task_id;
        std::uint64_t version{0};
        std::string message;
    };

    Subscription(std::shared_ptr<detail::HubRegistry> registry, std::string owner_id,
                 const config::EventLimits& limits);
    // Never blocks on the consumer: an event that does not fit closes the subscription.
    void push(const TaskEvent& event, const std::string& message);
    void close_locked(CloseReason reason);

    std::shared_ptr<detail::HubRegistry> registry_;
    std::string owner_id_;
    config::EventLimits limits_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Pending> queue_;
    std::uint64_t queued_bytes_{0};
    std::optional<CloseReason> closed_;
};

// Fans committed task versions out to the subscriptions of their owner (runtime-and-
// tasks.md, SSE 事件传输). Bounded per subscription in events, bytes and event size; an
// unsent version of a task is replaced by a newer one, nothing is dropped silently:
// overflow closes the subscription so the client resynchronizes over HTTP. Publishing
// never waits for a subscriber.
class TaskEventHub {
public:
    TaskEventHub(config::EventLimits limits, EventEncoder encoder);
    ~TaskEventHub();
    TaskEventHub(const TaskEventHub&) = delete;
    TaskEventHub& operator=(const TaskEventHub&) = delete;

    // Registers a subscription for the owner's tasks. Global and per-principal limits
    // are CALLER_LIMIT_EXCEEDED; a closed hub is SERVICE_BUSY.
    domain::Result<std::shared_ptr<Subscription>> subscribe(const std::string& owner_id);

    // Delivers to every subscription of event.owner_id registered at this moment.
    void publish(const TaskEvent& event);

    // Closes every subscription and refuses new ones.
    void close_all(CloseReason reason);

    [[nodiscard]] std::size_t subscriptions() const;

private:
    config::EventLimits limits_;
    EventEncoder encoder_;
    std::shared_ptr<detail::HubRegistry> registry_;
};

} // namespace music_service::application::events
