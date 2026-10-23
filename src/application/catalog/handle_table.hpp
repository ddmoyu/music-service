#pragma once

#include "application/ports/runtime/memory_budget.hpp"
#include "domain/result.hpp"

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace music_service::application::catalog {

// Live handles and bounded expiry tombstones share the process byte budget.
// Values are copied for updates so a failed budget check cannot mutate a handle.
template <class Value> class HandleTable {
public:
    using Clock = std::chrono::system_clock;
    using Weight = std::function<std::size_t(const Value&)>;
    using Budget = ports::MemoryBudget;

    HandleTable(std::size_t capacity, std::size_t per_owner, std::function<std::string()> new_id,
                std::shared_ptr<Budget> budget = {}, Weight weight = {})
        : capacity_(capacity), per_owner_(per_owner), new_id_(std::move(new_id)),
          budget_(budget ? std::move(budget) : std::make_shared<Budget>(32 * 1024 * 1024)),
          weight_(weight ? std::move(weight) : Weight{[](const Value&) { return sizeof(Value); }}) {
        budget_->register_reclaimer(this, [this](Clock::time_point now) {
            const std::lock_guard lock{mutex_};
            purge(now);
            while (!expired_order_.empty()) forget_oldest();
        });
    }
    ~HandleTable() { budget_->unregister_reclaimer(this); }

    domain::Result<std::string> put(const std::string& owner, Value value, Clock::time_point expires_at,
                                    Clock::time_point now) {
        std::string id;
        { const std::lock_guard lock{id_mutex_}; id = new_id_(); }
        return put_key(std::move(id), owner, std::move(value), expires_at, now);
    }

    // Caller-supplied keys are only for hashes of unguessable bearer tickets.
    domain::Result<std::string> put_key(std::string id, const std::string& owner, Value value,
                                        Clock::time_point expires_at, Clock::time_point now) {
        for (unsigned attempt = 0; attempt < 2; ++attempt) {
            std::unique_lock lock{mutex_};
            purge(now);
            const auto owned = owned_.find(owner);
            if (entries_.size() >= capacity_ || (owned != owned_.end() && owned->second >= per_owner_)) return full();
            if (entries_.contains(id) || expired_.contains(id)) {
                return domain::Error{domain::ErrorCode::internal, "handle identifier collision", false, {}};
            }
            const auto bytes = weight_(value) + overhead(id, owner);
            auto memory = Budget::reserve(budget_, bytes);
            while (!memory && !expired_order_.empty()) {
                forget_oldest();
                memory = Budget::reserve(budget_, bytes);
            }
            if (!memory) {
                lock.unlock();
                if (attempt == 0) { budget_->reclaim(now); continue; }
                return full();
            }
            auto [entry, inserted] = entries_.emplace(id, Entry{owner, expires_at, std::move(value), std::move(*memory)});
            (void)inserted;
            try { ++owned_[owner]; } catch (...) { entries_.erase(entry); throw; }
            return id;
        }
        return full();
    }

    domain::Result<std::pair<Value, Clock::time_point>>
    get(const std::string& owner, const std::string& id, Clock::time_point now,
        domain::ErrorCode expired = domain::ErrorCode::resource_expired) const {
        return lookup(&owner, id, now, expired);
    }

    // Only a bearer-token service may expose a lookup without an owner check.
    domain::Result<std::pair<Value, Clock::time_point>>
    get_by_key(const std::string& id, Clock::time_point now, domain::ErrorCode expired) const {
        return lookup(nullptr, id, now, expired);
    }

    template <class Change> domain::Result<Value>
    update(const std::string& owner, const std::string& id, Clock::time_point now, Change change) {
        const std::lock_guard lock{mutex_};
        const auto it = entries_.find(id);
        if (it == entries_.end() || it->second.owner != owner) return missing();
        if (it->second.expires_at <= now) return expired_error(domain::ErrorCode::resource_expired);
        auto value = it->second.value;
        change(value);
        if (!it->second.memory.resize(weight_(value) + overhead(id, owner))) return full();
        static_assert(std::is_nothrow_move_assignable_v<Value>);
        it->second.value = std::move(value);
        return it->second.value;
    }

    void expire(const std::string& owner, const std::string& id) {
        const std::lock_guard lock{mutex_};
        const auto it = entries_.find(id);
        if (it != entries_.end() && it->second.owner == owner) it->second.expires_at = Clock::time_point{};
    }

    std::size_t size() const { const std::lock_guard lock{mutex_}; return entries_.size(); }
    void collect_expired(Clock::time_point now) { const std::lock_guard lock{mutex_}; purge(now); }

private:
    struct Entry {
        std::string owner;
        Clock::time_point expires_at;
        Value value;
        Budget::Reservation memory;
    };
    struct Tombstone { std::string owner; Budget::Reservation memory; };

    static std::size_t overhead(const std::string& id, const std::string& owner) {
        // Map nodes, owner counter and duplicated key in the expiry queue.
        return 256 + 2 * (id.capacity() + owner.capacity() + 2);
    }
    static domain::Error full() { return {domain::ErrorCode::service_busy, "handle count or byte budget is full", true, {}}; }
    static domain::Error missing() { return {domain::ErrorCode::not_found, "no such handle", false, {}}; }
    static domain::Error expired_error(domain::ErrorCode code) { return {code, "the handle has expired", false, {}}; }

    domain::Result<std::pair<Value, Clock::time_point>>
    lookup(const std::string* owner, const std::string& id, Clock::time_point now, domain::ErrorCode expired) const {
        const std::lock_guard lock{mutex_};
        const auto it = entries_.find(id);
        if (it == entries_.end()) {
            const auto gone = expired_.find(id);
            if (gone != expired_.end() && (!owner || gone->second.owner == *owner)) return expired_error(expired);
            return missing();
        }
        if (owner && it->second.owner != *owner) return missing();
        if (it->second.expires_at <= now) return expired_error(expired);
        return std::pair{it->second.value, it->second.expires_at};
    }

    void forget_oldest() {
        expired_.erase(expired_order_.front());
        expired_order_.pop_front();
    }

    void purge(Clock::time_point now) {
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->second.expires_at > now) { ++it; continue; }
            expired_order_.push_back(it->first);
            try {
                auto [gone, added] = expired_.emplace(it->first, Tombstone{it->second.owner, {}});
                (void)added;
                gone->second.memory = std::move(it->second.memory);
                (void)gone->second.memory.resize(overhead(it->first, it->second.owner));
            } catch (...) { expired_order_.pop_back(); throw; }
            const auto owned = owned_.find(it->second.owner);
            if (--owned->second == 0) owned_.erase(owned);
            it = entries_.erase(it);
            while (expired_order_.size() > capacity_) forget_oldest();
        }
    }

    std::size_t capacity_;
    std::size_t per_owner_;
    std::function<std::string()> new_id_;
    std::shared_ptr<Budget> budget_;
    Weight weight_;
    mutable std::mutex mutex_;
    std::mutex id_mutex_;
    std::map<std::string, Entry> entries_;
    std::map<std::string, std::size_t> owned_;
    std::map<std::string, Tombstone> expired_;
    std::deque<std::string> expired_order_;
};

} // namespace music_service::application::catalog
