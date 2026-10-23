#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace music_service::application::ports {

// Shared across independently locked handle tables. Charges include payload,
// retained string/vector capacities and conservative container bookkeeping.
class MemoryBudget {
public:
    explicit MemoryBudget(std::size_t limit) : limit_(limit) {}
    bool acquire(std::size_t bytes) noexcept {
        auto used = used_.load();
        do {
            if (bytes > limit_ - used) return false;
        } while (!used_.compare_exchange_weak(used, used + bytes));
        return true;
    }
    void release(std::size_t bytes) noexcept { used_.fetch_sub(bytes); }
    std::size_t used() const noexcept { return used_.load(); }

    using Reclaimer = std::function<void(std::chrono::system_clock::time_point)>;
    void register_reclaimer(const void* owner, Reclaimer reclaim) {
        const std::lock_guard lock{reclaim_mutex_};
        reclaimers_.emplace(owner, std::move(reclaim));
    }
    void unregister_reclaimer(const void* owner) noexcept {
        const std::lock_guard lock{reclaim_mutex_};
        reclaimers_.erase(owner);
    }
    // Never call while holding a table lock. Registration/destruction synchronize
    // here, so callbacks cannot outlive their tables. Reclaimers never call back
    // into this registry, and only remove expired data, never another live handle.
    void reclaim(std::chrono::system_clock::time_point now) {
        const std::lock_guard lock{reclaim_mutex_};
        for (const auto& [owner, callback] : reclaimers_) {
            (void)owner;
            callback(now);
        }
    }

    class Reservation {
    public:
        Reservation() = default;
        Reservation(std::shared_ptr<MemoryBudget> budget, std::size_t bytes)
            : budget_(std::move(budget)), bytes_(bytes) {}
        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;
        Reservation(Reservation&& other) noexcept
            : budget_(std::move(other.budget_)), bytes_(std::exchange(other.bytes_, 0)) {}
        Reservation& operator=(Reservation&& other) noexcept {
            if (this != &other) {
                reset();
                budget_ = std::move(other.budget_);
                bytes_ = std::exchange(other.bytes_, 0);
            }
            return *this;
        }
        ~Reservation() { reset(); }
        bool resize(std::size_t bytes) noexcept {
            if (bytes > bytes_ && !budget_->acquire(bytes - bytes_)) return false;
            if (bytes < bytes_) budget_->release(bytes_ - bytes);
            bytes_ = bytes;
            return true;
        }
    private:
        void reset() noexcept { if (budget_) budget_->release(bytes_); }
        std::shared_ptr<MemoryBudget> budget_;
        std::size_t bytes_{0};
    };

    static std::optional<Reservation> reserve(const std::shared_ptr<MemoryBudget>& budget, std::size_t bytes) {
        if (!budget->acquire(bytes)) return std::nullopt;
        return Reservation{budget, bytes};
    }
private:
    std::size_t limit_;
    std::atomic<std::size_t> used_{0};
    std::mutex reclaim_mutex_;
    std::map<const void*, Reclaimer> reclaimers_;
};

} // namespace music_service::application::ports
