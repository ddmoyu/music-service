#include "../domain/test_support.hpp"

#include "application/events/publishing_task_repository.hpp"
#include "application/events/task_event_hub.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <future>
#include <thread>

using namespace music_service;
namespace events = application::events;
namespace ports = application::ports;
using namespace std::chrono_literals;
using events::CloseReason;
using events::TaskEvent;
using events::TaskEventHub;

namespace {

application::config::EventLimits limits(std::uint32_t global = 4, std::uint32_t per_principal = 2,
                                        std::uint32_t max_events = 4, std::uint64_t max_bytes = 1024,
                                        std::uint64_t max_event = 256) {
    return {global, per_principal, max_events, max_bytes, max_event};
}

std::string encode(const TaskEvent& event) {
    return event.task_id + "@" + std::to_string(event.version) + ":" + std::string{domain::to_string(event.status)};
}

TaskEvent event(std::string owner, std::string task, std::uint64_t version,
                domain::TaskStatus status = domain::TaskStatus::queued) {
    return TaskEvent{std::move(owner), std::move(task), version, status, std::nullopt, {}};
}

auto soon() { return std::chrono::steady_clock::now() + 50ms; }

std::shared_ptr<events::Subscription> subscribe(TaskEventHub& hub, const std::string& owner) {
    return test::unwrap(hub.subscribe(owner));
}

} // namespace

TEST_CASE("events reach only the owner's subscriptions, in commit order", "[events][hub]") {
    TaskEventHub hub{limits(), encode};
    auto alice = subscribe(hub, "prn_alice");
    auto alice_second = subscribe(hub, "prn_alice");
    auto bob = subscribe(hub, "prn_bob");

    hub.publish(event("prn_alice", "tsk_1", 1));
    hub.publish(event("prn_alice", "tsk_2", 1));
    hub.publish(event("prn_carol", "tsk_3", 1)); // nobody listens: nothing is kept

    CHECK(alice->wait(soon()).message == "tsk_1@1:queued");
    CHECK(alice->wait(soon()).message == "tsk_2@1:queued");
    CHECK(alice_second->wait(soon()).message == "tsk_1@1:queued"); // every connection of the owner
    const auto nothing = bob->wait(soon());
    CHECK_FALSE(nothing.message);
    CHECK_FALSE(nothing.closed);
}

TEST_CASE("subscriptions are limited per principal and globally", "[events][hub]") {
    TaskEventHub hub{limits(3, 2), encode};
    auto a1 = subscribe(hub, "prn_a");
    auto a2 = subscribe(hub, "prn_a");
    auto third = hub.subscribe("prn_a");
    REQUIRE_FALSE(third);
    CHECK(third.error().code == domain::ErrorCode::caller_limit_exceeded);

    auto b1 = subscribe(hub, "prn_b");
    auto global = hub.subscribe("prn_c");
    REQUIRE_FALSE(global);
    CHECK(global.error().code == domain::ErrorCode::caller_limit_exceeded);
    CHECK(hub.subscriptions() == 3);

    a1.reset(); // a closed connection frees its slot at once
    CHECK(hub.subscriptions() == 2);
    CHECK(hub.subscribe("prn_c"));
}

TEST_CASE("an unsent version is replaced by a newer one and never by an older one", "[events][hub]") {
    TaskEventHub hub{limits(), encode};
    auto sub = subscribe(hub, "prn_a");

    hub.publish(event("prn_a", "tsk_1", 1));
    hub.publish(event("prn_a", "tsk_2", 1));
    hub.publish(event("prn_a", "tsk_1", 2, domain::TaskStatus::downloading));
    hub.publish(event("prn_a", "tsk_1", 3, domain::TaskStatus::canceled));
    hub.publish(event("prn_a", "tsk_1", 2, domain::TaskStatus::downloading)); // late, older

    CHECK(sub->wait(soon()).message == "tsk_2@1:queued");
    CHECK(sub->wait(soon()).message == "tsk_1@3:canceled"); // the terminal version survives
    CHECK_FALSE(sub->wait(soon()).message);
}

TEST_CASE("a slow subscriber is closed for resync instead of growing or blocking", "[events][hub]") {
    TaskEventHub hub{limits(4, 2, 3), encode};
    auto slow = subscribe(hub, "prn_a");
    auto other = subscribe(hub, "prn_a");

    const auto started = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; ++i) {
        hub.publish(event("prn_a", "tsk_" + std::to_string(i), 1));
        if (i < 3) {
            CHECK(other->wait(soon()).message); // this one keeps up
        }
    }
    CHECK(std::chrono::steady_clock::now() - started < 2s); // publishing never waited
    const auto closed = slow->wait(soon());
    CHECK_FALSE(closed.message); // what it held is dropped, the client must resync
    CHECK(closed.closed == CloseReason::resync_required);
    CHECK(slow->wait(soon()).closed == CloseReason::resync_required);
    CHECK(other->wait(soon()).closed == CloseReason::resync_required); // it fell behind later
}

TEST_CASE("byte limits close the subscription too", "[events][hub]") {
    SECTION("queued bytes") {
        TaskEventHub hub{limits(4, 2, 100, 40), encode};
        auto sub = subscribe(hub, "prn_a");
        hub.publish(event("prn_a", "tsk_aaaaaaaaaa", 1)); // 22 bytes
        hub.publish(event("prn_a", "tsk_bbbbbbbbbb", 1)); // 44 > 40
        CHECK(sub->wait(soon()).closed == CloseReason::resync_required);
    }
    SECTION("one oversized event") {
        TaskEventHub hub{limits(4, 2, 100, 1024, 10), encode};
        auto sub = subscribe(hub, "prn_a");
        hub.publish(event("prn_a", "tsk_too_long", 1));
        CHECK(sub->wait(soon()).closed == CloseReason::resync_required);
    }
}

TEST_CASE("closing the hub wakes waiting streams and refuses new ones", "[events][hub]") {
    TaskEventHub hub{limits(), encode};
    auto sub = subscribe(hub, "prn_a");
    auto waiting = std::async(std::launch::async, [&] { return sub->wait(std::chrono::steady_clock::now() + 30s); });
    std::this_thread::sleep_for(50ms);
    hub.close_all(CloseReason::shutdown);
    REQUIRE(waiting.wait_for(5s) == std::future_status::ready);
    CHECK(waiting.get().closed == CloseReason::shutdown);
    auto late = hub.subscribe("prn_a");
    REQUIRE_FALSE(late);
    CHECK(late.error().code == domain::ErrorCode::service_busy);
}

namespace {

ports::CreateTaskCommand create_command() {
    domain::DownloadIntentDraft draft{"prn_a",
                                      test::track("kuwo", "MUSIC_1"),
                                      {test::anonymous("prn_a", "kuwo")},
                                      {},
                                      domain::MetadataPolicy::best_effort,
                                      {"dst_default", "d1", "n1", "m1"}};
    return ports::CreateTaskCommand{test::unwrap(domain::NormalizedDownloadIntent::normalize(std::move(draft))),
                                    "key-0123456789abcdef", "hash"};
}

// Commits nothing; returns scripted outcomes.
class ScriptedTasks final : public ports::TaskRepository {
public:
    ports::TaskRecord task(std::string id, std::uint64_t version, domain::TaskStatus status) const {
        ports::TaskRecord record;
        record.task_id = std::move(id);
        record.owner_id = "prn_a";
        record.version = version;
        record.status = status;
        return record;
    }

    domain::Result<ports::CreateTaskOutcome> next_create{domain::Error{domain::ErrorCode::internal, "unset", false, {}}};
    domain::Result<ports::BatchOutcome> next_batch{domain::Error{domain::ErrorCode::internal, "unset", false, {}}};
    domain::Result<ports::TaskRecord> next_task{domain::Error{domain::ErrorCode::internal, "unset", false, {}}};

    domain::Result<std::optional<ports::StoredResponse>> find_idempotent(const std::string&, const std::string&,
                                                                         const std::string&) override {
        return std::optional<ports::StoredResponse>{};
    }
    domain::Result<std::vector<ports::CompletedResult>> completed_results(const std::string&,
                                                                          const domain::DownloadFingerprint&) override {
        return std::vector<ports::CompletedResult>{};
    }
    domain::Result<ports::CreateTaskOutcome> create(const ports::CreateTaskCommand&) override { return next_create; }
    domain::Result<ports::BatchOutcome> create_batch(const ports::CreateBatchCommand&) override { return next_batch; }
    domain::Result<ports::BatchRecord> get_batch(const std::string&, const std::string&) override {
        return domain::Error{domain::ErrorCode::not_found, "none", false, {}};
    }
    domain::Result<ports::TaskRecord> get(const std::string&, const std::string&) override { return next_task; }
    domain::Result<std::vector<ports::TaskRecord>> list(const ports::TaskQuery&) override {
        return std::vector<ports::TaskRecord>{};
    }
    domain::Result<domain::NormalizedDownloadIntent> get_intent(const std::string&) override {
        return domain::Error{domain::ErrorCode::not_found, "none", false, {}};
    }
    domain::Result<std::optional<ports::TaskRecord>> claim_next(std::chrono::system_clock::time_point) override {
        if (!next_task) {
            return next_task.error();
        }
        return std::optional<ports::TaskRecord>{next_task.value()};
    }
    domain::Result<std::vector<ports::RunnableTask>> runnable(std::chrono::system_clock::time_point, std::uint32_t,
                                                             std::uint32_t) override {
        return std::vector<ports::RunnableTask>{};
    }
    domain::Result<std::optional<ports::TaskRecord>> claim(const std::string&,
                                                           std::chrono::system_clock::time_point) override {
        return claim_next({});
    }
    domain::Result<ports::TaskRecord> begin_attempt(const ports::BeginAttemptCommand&) override { return next_task; }
    domain::Result<ports::TaskRecord> defer(const ports::DeferCommand&) override { return next_task; }
    domain::Result<ports::TaskRecord> checkpoint(const ports::CheckpointCommand&) override { return next_task; }
    domain::Result<std::vector<ports::TaskRecord>> requeue_interrupted(std::chrono::system_clock::time_point) override {
        return std::vector<ports::TaskRecord>{};
    }
    domain::Result<ports::TaskRecord> transition(const ports::TransitionCommand&) override { return next_task; }
    domain::Result<ports::TaskRecord> request_cancel(const std::string&, const std::string&) override {
        return next_task;
    }
};

} // namespace

TEST_CASE("only committed new versions are published", "[events][repository]") {
    auto hub = std::make_shared<TaskEventHub>(limits(4, 2, 16, 4096), encode);
    auto inner = std::make_shared<ScriptedTasks>();
    events::PublishingTaskRepository tasks{inner, hub};
    auto sub = subscribe(*hub, "prn_a");
    const auto queued = domain::TaskStatus::queued;

    inner->next_create = ports::CreateTaskOutcome{inner->task("tsk_new", 1, queued), false, false};
    CHECK(tasks.create(create_command()));
    CHECK(sub->wait(soon()).message == "tsk_new@1:queued");

    // A replay or a reused task commits no new version.
    inner->next_create = ports::CreateTaskOutcome{inner->task("tsk_new", 1, queued), false, true};
    CHECK(tasks.create(create_command()));
    inner->next_create = ports::CreateTaskOutcome{inner->task("tsk_old", 4, queued), true, false};
    CHECK(tasks.create(create_command()));
    inner->next_create = domain::Error{domain::ErrorCode::queue_limit_exceeded, "full", true, {}};
    CHECK_FALSE(tasks.create(create_command()));
    CHECK_FALSE(sub->wait(soon()).message);

    // Batches publish each created task once.
    ports::BatchOutcome batch;
    batch.batch.items = {inner->task("tsk_b1", 1, queued), inner->task("tsk_b1", 1, queued),
                         inner->task("tsk_old", 4, queued)};
    batch.reused = {false, true, true};
    inner->next_batch = batch;
    CHECK(tasks.create_batch({}));
    CHECK(sub->wait(soon()).message == "tsk_b1@1:queued");
    CHECK_FALSE(sub->wait(soon()).message);
    batch.replayed = true;
    inner->next_batch = batch;
    CHECK(tasks.create_batch({}));
    CHECK_FALSE(sub->wait(soon()).message);

    // Cancellation, claims and runner transitions publish what they committed.
    inner->next_task = inner->task("tsk_new", 2, domain::TaskStatus::canceled);
    CHECK(tasks.request_cancel("prn_a", "tsk_new"));
    CHECK(sub->wait(soon()).message == "tsk_new@2:canceled");
    inner->next_task = inner->task("tsk_b1", 2, domain::TaskStatus::resolving);
    CHECK(tasks.claim_next({}));
    CHECK(sub->wait(soon()).message == "tsk_b1@2:resolving");
    inner->next_task = inner->task("tsk_b1", 3, domain::TaskStatus::downloading);
    CHECK(tasks.transition({}));
    CHECK(sub->wait(soon()).message == "tsk_b1@3:downloading");
    inner->next_task = domain::Error{domain::ErrorCode::version_conflict, "stale", false, {}};
    CHECK_FALSE(tasks.transition({}));
    CHECK(tasks.get("prn_a", "tsk_b1").error().code == domain::ErrorCode::version_conflict); // reads pass through
    CHECK_FALSE(sub->wait(soon()).message);
}
