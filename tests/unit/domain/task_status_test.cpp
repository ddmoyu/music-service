#include "test_support.hpp"

#include "domain/tasks/task_status.hpp"

#include <array>

using namespace music_dl::domain;
using enum TaskStatus;

namespace {

constexpr std::array all{queued,     resolving,     probing,   downloading,
                         processing, finalizing,    waiting_retry, canceling,
                         completed,  failed,        canceled};

} // namespace

TEST_CASE("status names round-trip", "[domain][tasks]") {
    for (const auto status : all) {
        CHECK(music_dl::test::unwrap(parse_task_status(to_string(status))) == status);
    }
    CHECK_FALSE(parse_task_status("running"));
}

TEST_CASE("terminal states have no outgoing transitions", "[domain][tasks]") {
    for (const auto from : {completed, failed, canceled}) {
        CHECK(is_terminal(from));
        for (const auto to : all) {
            CHECK_FALSE(is_transition_allowed(from, to));
        }
    }
}

TEST_CASE("the happy path is allowed in order", "[domain][tasks]") {
    CHECK(is_transition_allowed(queued, resolving));
    CHECK(is_transition_allowed(resolving, probing));
    CHECK(is_transition_allowed(probing, downloading));
    CHECK(is_transition_allowed(resolving, downloading)); // probing may be skipped
    CHECK(is_transition_allowed(downloading, processing));
    CHECK(is_transition_allowed(processing, finalizing));
    CHECK(is_transition_allowed(finalizing, completed));
}

TEST_CASE("illegal transitions are rejected", "[domain][tasks]") {
    CHECK_FALSE(is_transition_allowed(queued, completed));
    CHECK_FALSE(is_transition_allowed(downloading, completed)); // must publish first
    CHECK_FALSE(is_transition_allowed(processing, waiting_retry));
    CHECK_FALSE(is_transition_allowed(finalizing, canceling)); // finalizing wins the race
    CHECK_FALSE(is_transition_allowed(canceling, failed));
    CHECK_FALSE(is_transition_allowed(waiting_retry, canceling)); // cancels immediately instead

    const auto result = check_transition(downloading, completed);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::invalid_transition);
    CHECK(check_transition(queued, resolving));
}

TEST_CASE("finalizing can fail or retry instead of hanging", "[domain][tasks]") {
    CHECK(is_transition_allowed(finalizing, failed));
    CHECK(is_transition_allowed(finalizing, waiting_retry));
}

TEST_CASE("waiting_retry resumes at a recorded stage", "[domain][tasks]") {
    CHECK(is_transition_allowed(waiting_retry, resolving));
    CHECK(is_transition_allowed(waiting_retry, probing));
    CHECK(is_transition_allowed(waiting_retry, downloading));
    CHECK_FALSE(is_transition_allowed(waiting_retry, processing));
}

TEST_CASE("cancel requests are decided by status", "[domain][tasks]") {
    CHECK(*decide_cancel(queued) == CancelAction::cancel_now);
    CHECK(*decide_cancel(waiting_retry) == CancelAction::cancel_now);
    for (const auto status : {resolving, probing, downloading, processing}) {
        CHECK(*decide_cancel(status) == CancelAction::enter_canceling);
        CHECK(is_running(status));
    }
    CHECK(*decide_cancel(canceling) == CancelAction::already);
    CHECK(*decide_cancel(canceled) == CancelAction::already);

    CHECK(decide_cancel(finalizing).error().code == ErrorCode::task_finalizing);
    CHECK(decide_cancel(completed).error().code == ErrorCode::task_terminal);
    CHECK(decide_cancel(failed).error().code == ErrorCode::task_terminal);
}
