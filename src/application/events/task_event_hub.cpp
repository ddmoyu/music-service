#include "application/events/task_event_hub.hpp"

#include <algorithm>
#include <map>
#include <utility>
#include <vector>

namespace music_service::application::events {

namespace detail {

// Registered subscriptions by owner. Lock order: registry, then a subscription.
struct HubRegistry {
    std::mutex mutex;
    std::map<std::string, std::vector<Subscription*>, std::less<>> by_owner;
    std::size_t total{0};
    bool closed{false};
};

} // namespace detail

using domain::Error;
using domain::ErrorCode;

Subscription::Subscription(std::shared_ptr<detail::HubRegistry> registry, std::string owner_id,
                           const config::EventLimits& limits)
    : registry_(std::move(registry)), owner_id_(std::move(owner_id)), limits_(limits) {}

Subscription::~Subscription() {
    // Once removed under the registry lock, no publisher can still reach this object.
    const std::lock_guard lock{registry_->mutex};
    auto it = registry_->by_owner.find(owner_id_);
    if (it == registry_->by_owner.end()) {
        return;
    }
    auto& list = it->second;
    const auto found = std::find(list.begin(), list.end(), this);
    if (found != list.end()) {
        list.erase(found);
        --registry_->total;
    }
    if (list.empty()) {
        registry_->by_owner.erase(it);
    }
}

Subscription::Next Subscription::wait(std::chrono::steady_clock::time_point until) {
    std::unique_lock lock{mutex_};
    ready_.wait_until(lock, until, [this] { return closed_.has_value() || !queue_.empty(); });
    if (closed_) {
        return {std::nullopt, closed_};
    }
    if (queue_.empty()) {
        return {};
    }
    auto pending = std::move(queue_.front());
    queue_.pop_front();
    queued_bytes_ -= pending.message.size();
    return {std::move(pending.message), std::nullopt};
}

void Subscription::close(CloseReason reason) {
    const std::lock_guard lock{mutex_};
    close_locked(reason);
}

void Subscription::close_locked(CloseReason reason) {
    if (!closed_) {
        closed_ = reason;
        queue_.clear();
        queued_bytes_ = 0;
    }
    ready_.notify_all();
}

void Subscription::push(const TaskEvent& event, const std::string& message) {
    const std::lock_guard lock{mutex_};
    if (closed_) {
        return;
    }
    if (message.size() > limits_.max_event_bytes) {
        close_locked(CloseReason::resync_required);
        return;
    }
    // An unsent version of the same task is superseded; an older one is not news.
    const auto same = std::find_if(queue_.begin(), queue_.end(),
                                   [&](const Pending& pending) { return pending.task_id == event.task_id; });
    if (same != queue_.end()) {
        if (same->version >= event.version) {
            return;
        }
        queued_bytes_ -= same->message.size();
        queue_.erase(same);
    }
    if (queue_.size() + 1 > limits_.max_queued_events || queued_bytes_ + message.size() > limits_.max_queued_bytes) {
        close_locked(CloseReason::resync_required);
        return;
    }
    queue_.push_back({event.task_id, event.version, message});
    queued_bytes_ += message.size();
    ready_.notify_one();
}

TaskEventHub::TaskEventHub(config::EventLimits limits, EventEncoder encoder)
    : limits_(limits), encoder_(std::move(encoder)), registry_(std::make_shared<detail::HubRegistry>()) {}

TaskEventHub::~TaskEventHub() { close_all(CloseReason::shutdown); }

domain::Result<std::shared_ptr<Subscription>> TaskEventHub::subscribe(const std::string& owner_id) {
    const std::lock_guard lock{registry_->mutex};
    if (registry_->closed) {
        return Error{ErrorCode::service_busy, "the event stream is stopping", true, {}};
    }
    auto& list = registry_->by_owner[owner_id];
    if (registry_->total >= limits_.global_subscriptions || list.size() >= limits_.per_principal_subscriptions) {
        if (list.empty()) {
            registry_->by_owner.erase(owner_id);
        }
        Error error{ErrorCode::caller_limit_exceeded, "too many event subscriptions", true, {}};
        error.details.push_back({"subscriptions", registry_->total >= limits_.global_subscriptions
                                                      ? "the service subscription limit is reached"
                                                      : "the principal subscription limit is reached"});
        return error;
    }
    std::shared_ptr<Subscription> subscription{new Subscription(registry_, owner_id, limits_)};
    list.push_back(subscription.get());
    ++registry_->total;
    return subscription;
}

void TaskEventHub::publish(const TaskEvent& event) {
    const std::lock_guard lock{registry_->mutex};
    const auto it = registry_->by_owner.find(event.owner_id);
    if (it == registry_->by_owner.end() || it->second.empty()) {
        return;
    }
    const auto message = encoder_(event);
    for (auto* subscription : it->second) {
        subscription->push(event, message);
    }
}

void TaskEventHub::close_all(CloseReason reason) {
    const std::lock_guard lock{registry_->mutex};
    registry_->closed = true;
    for (auto& [owner, list] : registry_->by_owner) {
        for (auto* subscription : list) {
            subscription->close(reason);
        }
    }
}

std::size_t TaskEventHub::subscriptions() const {
    const std::lock_guard lock{registry_->mutex};
    return registry_->total;
}

} // namespace music_service::application::events
