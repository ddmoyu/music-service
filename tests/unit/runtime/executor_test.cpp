#include "runtime/cancellation/deadline_timer.hpp"
#include "runtime/executors/bounded_executor.hpp"
#include "runtime/shutdown/shutdown_sequence.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

using namespace music_service::runtime;
using namespace std::chrono_literals;

namespace {

// Blocks work until released, so tests control what is running.
struct Gate {
    std::promise<void> opened;
    std::shared_future<void> wait = opened.get_future().share();
    void open() { opened.set_value(); }
};

} // namespace

TEST_CASE("executor runs work on its threads", "[runtime][executor]") {
    BoundedExecutor executor{"test", 2, 8};
    std::atomic<int> done{0};
    for (int i = 0; i < 5; ++i) {
        REQUIRE(executor.submit([&](std::stop_token) { ++done; }));
    }
    REQUIRE(executor.wait_idle(2s));
    CHECK(done == 5);
    CHECK(executor.stats().completed == 5);
}

TEST_CASE("a full queue rejects instead of blocking", "[runtime][executor]") {
    BoundedExecutor executor{"full", 1, 1};
    Gate gate;
    std::promise<void> started;
    REQUIRE(executor.submit([&](std::stop_token) {
        started.set_value();
        gate.wait.wait();
    }));
    started.get_future().wait(); // the worker is busy
    REQUIRE(executor.submit([](std::stop_token) {}));  // fills the queue
    const auto rejected = executor.submit([](std::stop_token) {});
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == music_service::domain::ErrorCode::service_busy);
    CHECK(rejected.error().retryable);
    CHECK(executor.stats().rejected == 1);
    gate.open();
    CHECK(executor.wait_idle(2s));
}

TEST_CASE("stop signals running work and drops queued work", "[runtime][executor]") {
    BoundedExecutor executor{"stop", 1, 4};
    std::atomic<bool> saw_stop{false};
    std::atomic<bool> queued_ran{false};
    std::promise<void> started;
    REQUIRE(executor.submit([&](std::stop_token stop) {
        started.set_value();
        while (!stop.stop_requested()) {
            std::this_thread::sleep_for(1ms);
        }
        saw_stop = true; // e.g. save a checkpoint
    }));
    REQUIRE(executor.submit([&](std::stop_token) { queued_ran = true; }));
    started.get_future().wait();

    executor.stop_accepting();
    CHECK_FALSE(executor.submit([](std::stop_token) {}));
    CHECK_FALSE(executor.wait_idle(20ms)); // running work ignores the soft phase
    executor.request_stop();
    executor.join();
    CHECK(saw_stop);
    CHECK_FALSE(queued_ran);
}

TEST_CASE("shutdown sequence drains within the soft budget", "[runtime][shutdown]") {
    BoundedExecutor quick{"quick", 1, 4};
    BoundedExecutor slow{"slow", 1, 4};
    ShutdownSequence shutdown;
    shutdown.add(quick);
    shutdown.add(slow);

    std::atomic<bool> quick_done{false};
    std::atomic<bool> slow_stopped{false};
    REQUIRE(quick.submit([&](std::stop_token) {
        std::this_thread::sleep_for(5ms);
        quick_done = true;
    }));
    std::promise<void> started;
    REQUIRE(slow.submit([&](std::stop_token stop) {
        started.set_value();
        while (!stop.stop_requested()) {
            std::this_thread::sleep_for(1ms);
        }
        slow_stopped = true;
    }));
    started.get_future().wait();

    const auto report = shutdown.run(50ms);
    CHECK(quick_done);
    CHECK(slow_stopped);
    CHECK_FALSE(report.drained_within_budget);
    CHECK(report.still_busy == std::vector<std::string>{"slow"});
    CHECK(shutdown.stopping());
    CHECK(shutdown.token().stop_requested());
}

TEST_CASE("deadline timer fires in order and can cancel", "[runtime][timer]") {
    DeadlineTimer timer;
    std::mutex mutex;
    std::vector<int> fired;
    std::promise<void> last;
    const auto now = DeadlineTimer::Clock::now();

    timer.schedule(now + 30ms, [&] {
        const std::lock_guard lock{mutex};
        fired.push_back(2);
        last.set_value();
    });
    timer.schedule(now + 10ms, [&] {
        const std::lock_guard lock{mutex};
        fired.push_back(1);
    });
    const auto cancelled = timer.schedule(now + 20ms, [&] {
        const std::lock_guard lock{mutex};
        fired.push_back(99);
    });
    CHECK(timer.cancel(cancelled));
    CHECK_FALSE(timer.cancel(cancelled));

    REQUIRE(last.get_future().wait_for(2s) == std::future_status::ready);
    const std::lock_guard lock{mutex};
    CHECK(fired == std::vector<int>{1, 2});
    CHECK(timer.pending() == 0);
}

TEST_CASE("deadline timer can cancel an operation at its deadline", "[runtime][timer]") {
    DeadlineTimer timer;
    std::stop_source operation;
    timer.schedule(DeadlineTimer::Clock::now() + 5ms, [&] { operation.request_stop(); });
    const auto give_up = std::chrono::steady_clock::now() + 2s;
    while (!operation.stop_requested() && std::chrono::steady_clock::now() < give_up) {
        std::this_thread::sleep_for(1ms);
    }
    CHECK(operation.stop_requested());
}
