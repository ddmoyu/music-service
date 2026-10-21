// Media tickets (api-contract.md 10.3, A23 and A32 ticket parts): bound to one target,
// valid for any number of requests until they expire, stored as hashes, bounded in
// number, and an expired ticket is told apart from an unknown one.

#include "../domain/test_support.hpp"

#include "application/media/media_tickets.hpp"
#include "infrastructure/clock.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>

using namespace music_service;
using namespace std::chrono_literals;
namespace media = application::media;
namespace ports = application::ports;
using domain::ErrorCode;

namespace {

// Deterministic tokens; the hash is a visible transformation so tests can tell them apart.
class CountingCodec final : public ports::AccessTokenCodec {
public:
    domain::Result<ports::SecretBytes> generate() override {
        return ports::SecretBytes{"mst_random" + std::to_string(++count_)};
    }
    [[nodiscard]] std::string hash(std::string_view token) const override { return "h:" + std::string{token}; }

private:
    std::atomic<int> count_{0};
};

media::TicketGrant grant(const infrastructure::ManualClock& clock, std::string owner = "prn_a",
                         std::chrono::seconds ttl = 60s) {
    return {std::move(owner), "tok_1", media::TargetKind::file, "fil_1", clock.now() + ttl};
}

} // namespace

TEST_CASE("a ticket maps to its target until it expires", "[media][tickets]") {
    auto clock = std::make_shared<infrastructure::ManualClock>();
    media::MediaTicketService tickets{std::make_shared<CountingCodec>(), clock, 10, 5};

    const auto issued = test::unwrap(tickets.issue(grant(*clock)));
    CHECK(issued.token.rfind("mtk_", 0) == 0); // never usable as an access token
    CHECK(issued.expires_at == clock->now() + 60s);

    // Any number of requests while it is valid.
    for (int i = 0; i < 3; ++i) {
        const auto found = test::unwrap(tickets.authorize(issued.token));
        CHECK(found.owner_id == "prn_a");
        CHECK(found.token_id == "tok_1");
        CHECK(found.kind == media::TargetKind::file);
        CHECK(found.target_id == "fil_1");
    }
    CHECK(tickets.authorize("mtk_other").error().code == ErrorCode::not_found);
    CHECK(tickets.authorize("mst_random1").error().code == ErrorCode::not_found);

    // Expired: told apart from unknown, also after it was purged by a later issue.
    clock->set_wall(clock->now() + 61s);
    CHECK(tickets.authorize(issued.token).error().code == ErrorCode::media_ticket_expired);
    REQUIRE(tickets.issue(grant(*clock)));
    CHECK(tickets.size() == 1);
    CHECK(tickets.authorize(issued.token).error().code == ErrorCode::media_ticket_expired);
}

TEST_CASE("tickets are bounded per owner and in total", "[media][tickets]") {
    auto clock = std::make_shared<infrastructure::ManualClock>();
    media::MediaTicketService tickets{std::make_shared<CountingCodec>(), clock, 3, 2};

    REQUIRE(tickets.issue(grant(*clock, "prn_a")));
    REQUIRE(tickets.issue(grant(*clock, "prn_a")));
    CHECK(tickets.issue(grant(*clock, "prn_a")).error().code == ErrorCode::service_busy);
    REQUIRE(tickets.issue(grant(*clock, "prn_b")));
    CHECK(tickets.issue(grant(*clock, "prn_c")).error().code == ErrorCode::service_busy);

    // Expired tickets make room; live ones of others are never evicted.
    clock->set_wall(clock->now() + 61s);
    REQUIRE(tickets.issue(grant(*clock, "prn_c", 120s)));
    CHECK(tickets.size() == 1);
}
