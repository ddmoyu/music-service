#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "domain/result.hpp"

namespace music_service::application::catalog {

// Successful catalog results by key, with a TTL, an entry budget and a byte budget
// (cache.catalog_*). Keys always contain the full AuthIdentityKey, so a result only
// serves the identity that produced it; work started under an old credential version
// writes to the old partition, which nobody reads any more and which ages out.
// Failures are never stored. Least recently used entries go first. Thread-safe.
template <class Value> class ResultCache {
public:
    using Clock = std::chrono::system_clock;

    struct Hit {
        std::shared_ptr<const Value> value;
        Clock::time_point created_at;
    };

    ResultCache(std::size_t max_entries, std::size_t max_bytes, std::chrono::seconds ttl)
        : max_entries_(max_entries), max_bytes_(max_bytes), ttl_(ttl) {}

    std::optional<Hit> get(const std::string& key, Clock::time_point now) {
        const std::lock_guard lock{mutex_};
        const auto it = index_.find(key);
        if (it == index_.end()) {
            return std::nullopt;
        }
        if (it->second->created_at + ttl_ <= now) {
            erase(it);
            return std::nullopt;
        }
        order_.splice(order_.begin(), order_, it->second);
        return Hit{it->second->value, it->second->created_at};
    }

    void put(const std::string& key, std::shared_ptr<const Value> value, std::size_t bytes, Clock::time_point now) {
        if (max_entries_ == 0 || bytes > max_bytes_) {
            return;
        }
        const std::lock_guard lock{mutex_};
        if (const auto it = index_.find(key); it != index_.end()) {
            erase(it);
        }
        order_.push_front(Item{key, std::move(value), bytes, now});
        index_[key] = order_.begin();
        bytes_ += bytes;
        while (order_.size() > max_entries_ || bytes_ > max_bytes_) {
            erase(index_.find(order_.back().key));
        }
    }

    [[nodiscard]] std::size_t size() const {
        const std::lock_guard lock{mutex_};
        return order_.size();
    }

private:
    struct Item {
        std::string key;
        std::shared_ptr<const Value> value;
        std::size_t bytes;
        Clock::time_point created_at;
    };
    using Iterator = typename std::list<Item>::iterator;

    void erase(typename std::map<std::string, Iterator>::iterator it) {
        bytes_ -= it->second->bytes;
        order_.erase(it->second);
        index_.erase(it);
    }

    std::size_t max_entries_;
    std::size_t max_bytes_;
    std::chrono::seconds ttl_;
    mutable std::mutex mutex_;
    std::list<Item> order_;
    std::map<std::string, Iterator> index_;
    std::size_t bytes_{0};
};

// Merges identical in-flight calls: the first caller runs the work, later callers with
// the same key wait for its result, each only until its own deadline. Keys contain the
// full AuthIdentityKey and the query, so different accounts never share a call.
// Thread-safe.
template <class Value> class SingleFlight {
public:
    using Result = domain::Result<std::shared_ptr<const Value>>;

    // run() is called at most once per key at a time; its result goes to every waiter.
    Result join(const std::string& key, std::chrono::steady_clock::time_point deadline,
                const std::function<Result()>& run) {
        std::shared_ptr<Flight> flight;
        bool leader = false;
        {
            const std::lock_guard lock{mutex_};
            auto& slot = flights_[key];
            if (!slot) {
                slot = std::make_shared<Flight>();
                leader = true;
            } else {
                ++merged_;
            }
            flight = slot;
        }
        if (leader) {
            auto result = run();
            {
                const std::lock_guard lock{mutex_};
                flights_.erase(key);
                flight->result = std::make_shared<Result>(std::move(result));
            }
            flight->done.notify_all();
            return *flight->result;
        }
        std::unique_lock lock{mutex_};
        if (!flight->done.wait_until(lock, deadline, [&] { return flight->result != nullptr; })) {
            return domain::Error{domain::ErrorCode::deadline_exceeded, "the request deadline passed", true, {}};
        }
        return *flight->result;
    }

    // Calls that found another in flight, for diagnostics and tests.
    [[nodiscard]] std::size_t joins() const {
        const std::lock_guard lock{mutex_};
        return merged_;
    }

private:
    struct Flight {
        std::condition_variable done;
        std::shared_ptr<Result> result;
    };

    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Flight>> flights_;
    std::size_t merged_{0};
};

} // namespace music_service::application::catalog
