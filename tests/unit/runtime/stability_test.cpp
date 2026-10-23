#include "application/catalog/handle_table.hpp"
#include "application/catalog/result_cache.hpp"
#include "application/ports/runtime/scope_exit.hpp"
#include "infrastructure/clock.hpp"
#include "infrastructure/observability/logger.hpp"

#include <catch2/catch_test_macros.hpp>
#include <future>
#include <stdexcept>
#include <thread>

using namespace music_service;
using namespace std::chrono_literals;
namespace cat = application::catalog;
namespace obs = infrastructure::observability;

TEST_CASE("singleflight publishes exceptions to waiters and permits a new leader", "[stability][catalog]") {
    cat::SingleFlight<int> flight;
    std::promise<void> started, release;
    auto gate = release.get_future().share();
    auto first = std::async(std::launch::async, [&] {
        try {
            (void)flight.join("k", std::chrono::steady_clock::now() + 5s, [&]() -> cat::SingleFlight<int>::Result {
                started.set_value();
                gate.wait();
                throw std::runtime_error("injected");
            });
        } catch (const std::runtime_error&) { return true; }
        return false;
    });
    started.get_future().wait();
    auto second = std::async(std::launch::async, [&] {
        try {
            (void)flight.join("k", std::chrono::steady_clock::now() + 5s, []() -> cat::SingleFlight<int>::Result {
                return std::make_shared<const int>(-1);
            });
        } catch (const std::runtime_error&) { return true; }
        return false;
    });
    const auto until = std::chrono::steady_clock::now() + 2s;
    while (flight.joins() == 0 && std::chrono::steady_clock::now() < until) std::this_thread::yield();
    const auto joined = flight.joins();
    release.set_value();
    CHECK(joined == 1);
    CHECK(first.get());
    CHECK(second.get());
    auto retry = flight.join("k", std::chrono::steady_clock::now() + 1s, []() -> cat::SingleFlight<int>::Result {
        return std::make_shared<const int>(42);
    });
    REQUIRE(retry);
    CHECK(**retry == 42);
}

TEST_CASE("handle tables share bytes and reject oversized updates without changing values", "[stability][catalog]") {
    auto budget = std::make_shared<application::ports::MemoryBudget>(3000);
    unsigned next = 0;
    auto ids = [&] { return "id_" + std::to_string(++next); };
    auto weight = [](const std::string& value) { return sizeof(value) + value.capacity() + 1; };
    const auto now = std::chrono::system_clock::now();
    {
        cat::HandleTable<std::string> one{20, 10, ids, budget, weight}, two{20, 10, ids, budget, weight};
        auto id = one.put("owner", std::string(1800, 'a'), now + 1s, now);
        REQUIRE(id);
        auto second = two.put("another", std::string(1200, 'b'), now + 1s, now);
        REQUIRE_FALSE(second);
        CHECK(second.error().code == domain::ErrorCode::service_busy);
        const auto used = budget->used();
        auto changed = one.update("owner", *id, now, [](auto& value) { value.resize(5000, 'c'); });
        REQUIRE_FALSE(changed);
        CHECK(budget->used() == used);
        CHECK(one.get("owner", *id, now)->first == std::string(1800, 'a'));
        CHECK(one.get("foreign", *id, now).error().code == domain::ErrorCode::not_found);
        one.collect_expired(now + 2s);
        CHECK(budget->used() < used);
        REQUIRE(two.put("another", std::string(1200, 'b'), now + 3s, now + 2s));
        CHECK(one.get("owner", *id, now + 2s).error().code == domain::ErrorCode::resource_expired);
    }
    CHECK(budget->used() == 0);
}

TEST_CASE("expired handles in one table cannot starve another table's byte budget", "[stability][catalog]") {
    auto budget = std::make_shared<application::ports::MemoryBudget>(3000);
    unsigned next = 0;
    auto ids = [&] { return "id_" + std::to_string(++next); };
    auto weight = [](const std::string& value) { return sizeof(value) + value.capacity() + 1; };
    const auto now = std::chrono::system_clock::now();
    cat::HandleTable<std::string> cursor{20, 10, ids, budget, weight}, asset{20, 10, ids, budget, weight};
    REQUIRE(cursor.put("owner", std::string(2400, 'c'), now + 1s, now));
    CHECK_FALSE(asset.put("owner", std::string(2400, 'a'), now + 3s, now));
    // No lookup or insertion in the cursor table is needed to reclaim its expiry.
    REQUIRE(asset.put("owner", std::string(2400, 'a'), now + 3s, now + 2s));
    CHECK(budget->used() <= 3000);
    CHECK(cursor.size() == 0);
}

TEST_CASE("blocked logging has bounded memory and bounded shutdown", "[stability][logger]") {
    class BlockedSink final : public obs::LogSink {
    public:
        std::promise<void> entered, release, exited;
        std::shared_future<void> gate = release.get_future().share();
        void write(const obs::LogRecord&) override { entered.set_value(); gate.wait(); exited.set_value(); }
    };
    auto target = std::make_shared<BlockedSink>();
    const application::ports::ScopeExit release{[&]() noexcept { try { target->release.set_value(); } catch (...) {} }};
    obs::AsyncLogSink sink{target, 2, 4096, 20ms};
    const obs::LogRecord record{{}, obs::LogLevel::info, "test", {}};
    sink.write(record);
    REQUIRE(target->entered.get_future().wait_for(2s) == std::future_status::ready);
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; ++i) sink.write(record);
    CHECK(std::chrono::steady_clock::now() - start < 500ms);
    CHECK(sink.stats().queued == 2);
    CHECK(sink.stats().bytes <= 4096);
    CHECK(sink.stats().dropped == 998);
    sink.close();
    CHECK(std::chrono::steady_clock::now() - start < 1s);
    CHECK(sink.stats().queued == 0);
    target->release.set_value();
    CHECK(target->exited.get_future().wait_for(2s) == std::future_status::ready);
}

TEST_CASE("log output exceptions never escape the business caller", "[stability][logger]") {
    class FailingSink final : public obs::LogSink {
        void write(const obs::LogRecord&) override { throw std::runtime_error("broken output"); }
    };
    auto target = std::make_shared<FailingSink>();
    obs::Logger log{target, obs::LogLevel::info, std::make_shared<infrastructure::SystemClock>()};
    REQUIRE_NOTHROW(log.info("operation.completed"));
    obs::AsyncLogSink sink{target};
    sink.write({{}, obs::LogLevel::info, "failed", {}});
    sink.close();
    CHECK(sink.stats().failures == 1);
}
