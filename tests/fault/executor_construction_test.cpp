// Replaces the global allocator to fail one chosen allocation on the calling thread,
// so it lives in its own executable.
#include "runtime/executors/bounded_executor.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <memory>
#include <new>
#include <thread>

namespace {

// Allocations left before the injected failure on this thread; negative is disarmed.
thread_local long fail_countdown = -1;

// Debug STL builds allocate 16-byte iterator-debugging proxies inside noexcept moves;
// failing those terminates whatever the code under test does, so only larger
// allocations (vector storage, stop state, thread state) are candidates.
constexpr std::size_t min_failing_size = 17;

} // namespace

void* operator new(std::size_t size) {
    if (size >= min_failing_size && fail_countdown >= 0 && fail_countdown-- == 0) {
        throw std::bad_alloc{};
    }
    if (void* p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc{};
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using music_service::runtime::BoundedExecutor;
using namespace std::chrono_literals;

TEST_CASE("an executor whose later thread fails to start still unwinds", "[runtime][executor][fault]") {
    // Fails the 1st, 2nd, ... allocation of the constructor in turn until it succeeds,
    // so every point between the reserve and the last thread is covered.
    bool constructed = false;
    int failures = 0;
    for (long k = 0; !constructed && k < 100; ++k) {
        std::promise<bool> result;
        auto finished = result.get_future();
        std::thread{[k, result = std::move(result)]() mutable {
            fail_countdown = k;
            bool ok = false;
            try {
                BoundedExecutor executor{"fault", 3, 4};
                ok = true;
            } catch (const std::bad_alloc&) {
            }
            fail_countdown = -1;
            result.set_value(ok);
        }}.detach();
        if (finished.wait_for(5s) != std::future_status::ready) {
            // A worker that never sees the stop request blocks join() forever.
            std::fprintf(stderr, "BoundedExecutor constructor hung after failing allocation %ld\n", k);
            std::_Exit(1);
        }
        if (finished.get()) {
            constructed = true;
        } else {
            ++failures;
        }
    }
    CHECK(constructed);
    CHECK(failures >= 3); // at least the reserve and the threads themselves
}

TEST_CASE("executor stop drains queued work without allocating or holding its lock", "[runtime][executor][fault]") {
    BoundedExecutor executor{"stop-fault", 1, 4};
    std::promise<void> started;
    auto running = started.get_future();
    std::atomic<bool> queued_ran{false};
    bool released = false;
    REQUIRE(executor.submit([&](std::stop_token stop) {
        started.set_value();
        while (!stop.stop_requested()) std::this_thread::sleep_for(1ms);
    }));
    REQUIRE(running.wait_for(2s) == std::future_status::ready);
    // Re-entering stats() detects cleanup accidentally moved under mutex_.
    auto owned = std::shared_ptr<int>{new int{0}, [&](int* value) {
        (void)executor.stats();
        released = true;
        delete value;
    }};
    REQUIRE(executor.submit([&, owned = std::move(owned)](std::stop_token) { queued_ran = true; }));

    bool allocated = false;
    fail_countdown = 0;
    try {
        executor.request_stop();
    } catch (const std::bad_alloc&) {
        allocated = true;
    }
    fail_countdown = -1;
    // Clean up even when the assertion is going to fail.
    executor.request_stop();
    executor.join();
    CHECK_FALSE(allocated);
    CHECK_FALSE(queued_ran.load());
    CHECK(released);
    CHECK(executor.stats().queued == 0);
}
