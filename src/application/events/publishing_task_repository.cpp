#include "application/events/publishing_task_repository.hpp"

#include <set>
#include <utility>

namespace music_service::application::events {

TaskEvent event_of(const ports::TaskRecord& task) {
    return TaskEvent{task.owner_id, task.task_id, task.version, task.status, task.progress_json, task.updated_at};
}

PublishingTaskRepository::PublishingTaskRepository(std::shared_ptr<ports::TaskRepository> inner,
                                                   std::shared_ptr<TaskEventHub> hub)
    : inner_(std::move(inner)), hub_(std::move(hub)) {}

domain::Result<std::optional<ports::StoredResponse>>
PublishingTaskRepository::find_idempotent(const std::string& owner_id, const std::string& route,
                                          const std::string& idempotency_key) {
    return inner_->find_idempotent(owner_id, route, idempotency_key);
}

domain::Result<std::vector<ports::CompletedResult>>
PublishingTaskRepository::completed_results(const std::string& owner_id, const domain::DownloadFingerprint& fingerprint) {
    return inner_->completed_results(owner_id, fingerprint);
}

domain::Result<ports::CreateTaskOutcome> PublishingTaskRepository::create(const ports::CreateTaskCommand& command) {
    auto outcome = inner_->create(command);
    if (outcome && !outcome->replayed && !outcome->reused) {
        hub_->publish(event_of(outcome->task));
    }
    return outcome;
}

domain::Result<ports::BatchOutcome> PublishingTaskRepository::create_batch(const ports::CreateBatchCommand& command) {
    auto outcome = inner_->create_batch(command);
    if (outcome && !outcome->replayed) {
        std::set<std::string> published; // a duplicate item reuses the earlier one
        for (std::size_t i = 0; i < outcome->batch.items.size(); ++i) {
            const auto& task = outcome->batch.items[i];
            const bool reused = i < outcome->reused.size() && outcome->reused[i];
            if (!reused && published.insert(task.task_id).second) {
                hub_->publish(event_of(task));
            }
        }
    }
    return outcome;
}

domain::Result<ports::BatchRecord> PublishingTaskRepository::get_batch(const std::string& owner_id,
                                                                       const std::string& batch_id) {
    return inner_->get_batch(owner_id, batch_id);
}

domain::Result<ports::TaskRecord> PublishingTaskRepository::get(const std::string& owner_id,
                                                                const std::string& task_id) {
    return inner_->get(owner_id, task_id);
}

domain::Result<std::vector<ports::TaskRecord>> PublishingTaskRepository::list(const ports::TaskQuery& query) {
    return inner_->list(query);
}

domain::Result<domain::NormalizedDownloadIntent> PublishingTaskRepository::get_intent(const std::string& task_id) {
    return inner_->get_intent(task_id);
}

domain::Result<std::optional<ports::TaskRecord>>
PublishingTaskRepository::claim_next(std::chrono::system_clock::time_point now) {
    auto claimed = inner_->claim_next(now);
    if (claimed && *claimed) {
        hub_->publish(event_of(**claimed));
    }
    return claimed;
}

domain::Result<ports::TaskRecord> PublishingTaskRepository::transition(const ports::TransitionCommand& command) {
    auto task = inner_->transition(command);
    if (task) {
        hub_->publish(event_of(*task));
    }
    return task;
}

domain::Result<ports::TaskRecord> PublishingTaskRepository::request_cancel(const std::string& owner_id,
                                                                           const std::string& task_id) {
    auto task = inner_->request_cancel(owner_id, task_id);
    if (task) {
        hub_->publish(event_of(*task));
    }
    return task;
}

} // namespace music_service::application::events
