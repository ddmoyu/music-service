#pragma once

#include "application/events/task_event_hub.hpp"
#include "application/ports/storage/task_repository.hpp"

#include <memory>

namespace music_service::application::events {

TaskEvent event_of(const ports::TaskRecord& task);

// Publishes every task version a write commits, after the commit returns (feature-
// modules.md 12): creation, batch items, cancellation, claims and runner transitions.
// Replays, reuses and failed writes commit no new version and publish nothing. A
// publication can only close subscriptions; it never undoes the commit.
class PublishingTaskRepository final : public ports::TaskRepository {
public:
    PublishingTaskRepository(std::shared_ptr<ports::TaskRepository> inner, std::shared_ptr<TaskEventHub> hub);

    domain::Result<std::optional<ports::StoredResponse>>
    find_idempotent(const std::string& owner_id, const std::string& route, const std::string& idempotency_key) override;
    domain::Result<std::vector<ports::CompletedResult>>
    completed_results(const std::string& owner_id, const domain::DownloadFingerprint& fingerprint) override;
    domain::Result<ports::CreateTaskOutcome> create(const ports::CreateTaskCommand& command) override;
    domain::Result<ports::BatchOutcome> create_batch(const ports::CreateBatchCommand& command) override;
    domain::Result<ports::BatchRecord> get_batch(const std::string& owner_id, const std::string& batch_id) override;
    domain::Result<ports::TaskRecord> get(const std::string& owner_id, const std::string& task_id) override;
    domain::Result<std::vector<ports::TaskRecord>> list(const ports::TaskQuery& query) override;
    domain::Result<domain::NormalizedDownloadIntent> get_intent(const std::string& task_id) override;
    domain::Result<std::optional<ports::TaskRecord>> claim_next(std::chrono::system_clock::time_point now) override;
    domain::Result<ports::TaskRecord> transition(const ports::TransitionCommand& command) override;
    domain::Result<ports::TaskRecord> request_cancel(const std::string& owner_id, const std::string& task_id) override;

private:
    std::shared_ptr<ports::TaskRepository> inner_;
    std::shared_ptr<TaskEventHub> hub_;
};

} // namespace music_service::application::events
