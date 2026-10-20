#pragma once

#include "domain/result.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace music_service::application::catalog {

// Short-lived server handles (cursors, assets, contexts) with an owner, an expiry and
// bounded capacity: an instance limit and a per-owner share of it (handles in
// configuration). Expired entries are dropped first; when no room is left a new
// handle is SERVICE_BUSY, never an eviction of someone else's live handle.
// Lookups by another owner are NOT_FOUND; expired handles are RESOURCE_EXPIRED (or
// the error passed in, such as CURSOR_EXPIRED). Thread-safe.
template <class Value> class HandleTable {
public:
    using Clock = std::chrono::system_clock;

    HandleTable(std::size_t capacity, std::size_t per_owner, std::function<std::string()> new_id)
        : capacity_(capacity), per_owner_(per_owner), new_id_(std::move(new_id)) {}

    domain::Result<std::string> put(const std::string& owner, Value value, Clock::time_point expires_at,
                                    Clock::time_point now) {
        const std::lock_guard lock{mutex_};
        purge(now);
        if (entries_.size() >= capacity_ || owned_[owner] >= per_owner_) {
            return domain::Error{domain::ErrorCode::service_busy, "too many live handles", true, {}};
        }
        auto id = new_id_();
        entries_.emplace(id, Entry{owner, expires_at, std::move(value)});
        ++owned_[owner];
        return id;
    }

    // The value and its expiry, or NOT_FOUND / the expired error.
    domain::Result<std::pair<Value, Clock::time_point>>
    get(const std::string& owner, const std::string& id, Clock::time_point now,
        domain::ErrorCode expired = domain::ErrorCode::resource_expired) const {
        const std::lock_guard lock{mutex_};
        const auto it = entries_.find(id);
        if (it == entries_.end() || it->second.owner != owner) {
            return domain::Error{domain::ErrorCode::not_found, "no such handle", false, {}};
        }
        if (it->second.expires_at <= now) {
            return domain::Error{expired, "the handle has expired", false, {}};
        }
        return std::pair{it->second.value, it->second.expires_at};
    }

    [[nodiscard]] std::size_t size() const {
        const std::lock_guard lock{mutex_};
        return entries_.size();
    }

private:
    struct Entry {
        std::string owner;
        Clock::time_point expires_at;
        Value value;
    };

    void purge(Clock::time_point now) {
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->second.expires_at <= now) {
                if (--owned_[it->second.owner] == 0) {
                    owned_.erase(it->second.owner);
                }
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::size_t capacity_;
    std::size_t per_owner_;
    std::function<std::string()> new_id_;
    mutable std::mutex mutex_;
    std::map<std::string, Entry> entries_;
    std::map<std::string, std::size_t> owned_;
};

} // namespace music_service::application::catalog
