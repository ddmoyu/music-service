#include "../domain/test_support.hpp"

#include "infrastructure/clock.hpp"
#include "runtime/admission/admission_ledger.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

using namespace music_service;
using application::ports::AdmissionRequest;
using application::ports::Deferred;
using application::ports::TrafficClass;
using application::ports::UpstreamLease;
using runtime::AdmissionLedger;
using runtime::AdmissionLimits;

namespace {

struct Fixture {
    std::shared_ptr<infrastructure::ManualClock> clock = std::make_shared<infrastructure::ManualClock>();

    AdmissionLimits limits() const {
        AdmissionLimits l;
        l.total_connections = 4;
        l.per_principal = 3;
        l.per_origin = 2;
        l.per_source[TrafficClass::short_request] = 2;
        l.per_source[TrafficClass::download] = 1;
        return l;
    }

    AdmissionRequest request(std::string_view source, std::string origin,
                             std::string principal = "prn_a",
                             TrafficClass traffic = TrafficClass::short_request) const {
        AdmissionRequest r{std::move(principal),
                           test::source(source),
                           "search_tracks",
                           traffic,
                           std::nullopt,
                           std::move(origin),
                           "",
                           clock->steady_now() + std::chrono::seconds{10}};
        return r;
    }
};

UpstreamLease granted(domain::Result<application::ports::AdmissionOutcome> outcome) {
    REQUIRE(outcome);
    REQUIRE(std::holds_alternative<UpstreamLease>(*outcome));
    return std::get<UpstreamLease>(std::move(outcome).value());
}

Deferred deferred(domain::Result<application::ports::AdmissionOutcome> outcome) {
    REQUIRE(outcome);
    REQUIRE(std::holds_alternative<Deferred>(*outcome));
    return std::get<Deferred>(*outcome);
}

} // namespace

// A43: a full source defers without blocking, another source proceeds.
TEST_CASE("a full source defers while other sources proceed", "[runtime][admission]") {
    Fixture f;
    AdmissionLedger ledger{f.limits(), f.clock};

    auto a = granted(ledger.try_acquire(f.request("kuwo", "http://search.kuwo.cn")));
    auto b = granted(ledger.try_acquire(f.request("kuwo", "http://m.kuwo.cn")));
    const auto blocked = deferred(ledger.try_acquire(f.request("kuwo", "http://wapi.kuwo.cn")));
    CHECK(blocked.blocked_dimension == "source");
    CHECK(blocked.retry_at > f.clock->steady_now());
    CHECK(ledger.readiness(f.request("kuwo", "http://wapi.kuwo.cn")).has_value());

    auto other = granted(ledger.try_acquire(f.request("qq", "https://u.y.qq.com")));
    // prn_a now holds its per-principal limit of 3; another principal is still ready.
    CHECK(ledger.readiness(f.request("migu", "https://m.music.migu.cn")).has_value());
    CHECK_FALSE(ledger.readiness(f.request("migu", "https://m.music.migu.cn", "prn_b")).has_value());

    a.release();
    CHECK_FALSE(ledger.readiness(f.request("kuwo", "http://wapi.kuwo.cn")).has_value());
    CHECK(ledger.usage().total == 2);
}

TEST_CASE("each dimension limits independently", "[runtime][admission]") {
    Fixture f;
    AdmissionLedger ledger{f.limits(), f.clock};

    SECTION("origin") {
        auto a = granted(ledger.try_acquire(f.request("kuwo", "http://cdn.example")));
        auto b = granted(ledger.try_acquire(f.request("qq", "http://cdn.example")));
        CHECK(deferred(ledger.try_acquire(f.request("migu", "http://cdn.example"))).blocked_dimension == "origin");
    }
    SECTION("principal") {
        auto a = granted(ledger.try_acquire(f.request("kuwo", "http://a.example")));
        auto b = granted(ledger.try_acquire(f.request("qq", "http://b.example")));
        auto c = granted(ledger.try_acquire(f.request("migu", "http://c.example")));
        CHECK(deferred(ledger.try_acquire(f.request("joox", "http://d.example"))).blocked_dimension == "principal");
        // Another principal is not limited by the first one's usage.
        auto d = granted(ledger.try_acquire(f.request("joox", "http://d.example", "prn_b")));
        CHECK(deferred(ledger.try_acquire(f.request("bilibili", "http://e.example", "prn_b"))).blocked_dimension == "total");
    }
    SECTION("traffic class") {
        auto download = granted(ledger.try_acquire(f.request("kuwo", "http://cdn1.example", "prn_a", TrafficClass::download)));
        CHECK(deferred(ledger.try_acquire(f.request("kuwo", "http://cdn2.example", "prn_a", TrafficClass::download)))
                  .blocked_dimension == "source");
        // Short requests to the same source have their own allowance.
        auto search = granted(ledger.try_acquire(f.request("kuwo", "http://search.kuwo.cn")));
    }
}

TEST_CASE("a refused request reserves nothing", "[runtime][admission]") {
    Fixture f;
    AdmissionLedger ledger{f.limits(), f.clock};
    auto a = granted(ledger.try_acquire(f.request("kuwo", "http://cdn.example")));
    auto b = granted(ledger.try_acquire(f.request("qq", "http://cdn.example")));
    const auto before = ledger.usage();

    // Source and principal have room, origin does not: no partial reservation remains.
    (void)deferred(ledger.try_acquire(f.request("migu", "http://cdn.example")));
    const auto after = ledger.usage();
    CHECK(after.total == before.total);
    CHECK(after.principals == before.principals);
    CHECK(after.sources == before.sources);
    CHECK(after.origins == before.origins);
}

TEST_CASE("leases return quota exactly once", "[runtime][admission]") {
    Fixture f;
    AdmissionLedger ledger{f.limits(), f.clock};
    {
        auto lease = granted(ledger.try_acquire(f.request("kuwo", "http://search.kuwo.cn")));
        auto moved = std::move(lease);
        moved.release();
        moved.release();
        CHECK(ledger.usage().total == 0);
    }
    CHECK(ledger.usage().total == 0);

    auto request = f.request("kuwo", "http://search.kuwo.cn");
    request.connections = 2;
    {
        auto lease = granted(ledger.try_acquire(request));
        CHECK(ledger.usage().total == 2);
    } // destructor releases
    CHECK(ledger.usage().total == 0);
    CHECK(ledger.usage().sources.empty());
}

TEST_CASE("leases may outlive the ledger", "[runtime][admission]") {
    Fixture f;
    auto ledger = std::make_unique<AdmissionLedger>(f.limits(), f.clock);
    auto lease = granted(ledger->try_acquire(f.request("kuwo", "http://search.kuwo.cn")));
    ledger.reset();
    lease.release(); // must not touch freed memory
    CHECK_FALSE(lease.active());
}

TEST_CASE("Retry-After cools down the source or its quota group", "[runtime][admission]") {
    Fixture f;
    AdmissionLedger ledger{f.limits(), f.clock};
    const auto now = f.clock->steady_now();

    ledger.feedback(f.request("kuwo", "http://search.kuwo.cn"), now + std::chrono::seconds{30});
    const auto cooling = deferred(ledger.try_acquire(f.request("kuwo", "http://m.kuwo.cn")));
    CHECK(cooling.blocked_dimension == "cooldown");
    CHECK(cooling.retry_at == now + std::chrono::seconds{30});
    // A shorter later value never shortens the cooldown.
    ledger.feedback(f.request("kuwo", "http://m.kuwo.cn"), now + std::chrono::seconds{5});
    CHECK(deferred(ledger.try_acquire(f.request("kuwo", "http://m.kuwo.cn"))).retry_at ==
          now + std::chrono::seconds{30});
    auto other = granted(ledger.try_acquire(f.request("qq", "https://u.y.qq.com")));

    f.clock->advance(std::chrono::seconds{31});
    auto resumed = granted(ledger.try_acquire(f.request("kuwo", "http://m.kuwo.cn")));
    CHECK(ledger.usage().cooldowns.empty());

    // A45 groundwork: two sources sharing an application quota group cool down together.
    auto spotify = f.request("spotify", "https://api.spotify.com");
    spotify.rate_limit_group = "spotify_app";
    auto spotify_other = f.request("spotify_alt", "https://api2.spotify.com");
    spotify_other.rate_limit_group = "spotify_app";
    ledger.feedback(spotify, f.clock->steady_now() + std::chrono::seconds{10});
    CHECK(deferred(ledger.try_acquire(spotify_other)).blocked_dimension == "cooldown");

    // Retry-After is capped.
    ledger.feedback(f.request("migu", "https://m.music.migu.cn"),
                    f.clock->steady_now() + std::chrono::hours{24});
    CHECK(deferred(ledger.try_acquire(f.request("migu", "https://m.music.migu.cn"))).retry_at <=
          f.clock->steady_now() + f.limits().max_cooldown);
}

TEST_CASE("invalid and expired requests are errors, not deferrals", "[runtime][admission]") {
    Fixture f;
    AdmissionLedger ledger{f.limits(), f.clock};

    auto expired = f.request("kuwo", "http://search.kuwo.cn");
    expired.deadline = f.clock->steady_now();
    CHECK(ledger.try_acquire(expired).error().code == domain::ErrorCode::deadline_exceeded);

    auto helper = f.request("youtube", "https://youtube.com");
    helper.helper_processes = 1;
    CHECK(ledger.try_acquire(helper).error().code == domain::ErrorCode::capability_unsupported);

    auto empty = f.request("kuwo", "");
    CHECK(ledger.try_acquire(empty).error().code == domain::ErrorCode::validation_failed);
}
