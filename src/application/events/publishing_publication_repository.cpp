#include "application/events/publishing_publication_repository.hpp"

#include "application/events/publishing_task_repository.hpp"

#include <utility>

namespace music_service::application::events {

PublishingPublicationRepository::PublishingPublicationRepository(std::shared_ptr<ports::PublicationRepository> inner,
                                                                 std::shared_ptr<TaskEventHub> hub)
    : inner_(std::move(inner)), hub_(std::move(hub)) {}

domain::Result<ports::TaskRecord> PublishingPublicationRepository::begin(const ports::PublicationIntent& intent,
                                                                         std::uint64_t expected_version) {
    auto task = inner_->begin(intent, expected_version);
    if (task) {
        hub_->publish(event_of(task.value()));
    }
    return task;
}

domain::Result<void> PublishingPublicationRepository::set_stage(const std::string& task_id,
                                                                const std::string& execution_id,
                                                                const std::string& stage) {
    return inner_->set_stage(task_id, execution_id, stage);
}

domain::Result<void> PublishingPublicationRepository::relocate(const std::string& task_id,
                                                               const std::string& execution_id,
                                                               const std::string& reserved_path) {
    return inner_->relocate(task_id, execution_id, reserved_path);
}

domain::Result<ports::CompletedPublication>
PublishingPublicationRepository::complete(const std::string& task_id, const std::string& execution_id,
                                          std::uint64_t expected_version, const ports::NewFileAsset& asset,
                                          const std::string& result_json) {
    auto done = inner_->complete(task_id, execution_id, expected_version, asset, result_json);
    if (done) {
        hub_->publish(event_of(done->task));
    }
    return done;
}

domain::Result<ports::TaskRecord> PublishingPublicationRepository::abandon(const std::string& task_id,
                                                                           const std::string& execution_id,
                                                                           std::uint64_t expected_version,
                                                                           domain::TaskStatus to,
                                                                           const std::string& error_json) {
    auto task = inner_->abandon(task_id, execution_id, expected_version, to, error_json);
    if (task) {
        hub_->publish(event_of(task.value()));
    }
    return task;
}

domain::Result<std::vector<ports::PendingPublication>> PublishingPublicationRepository::pending() {
    return inner_->pending();
}

domain::Result<std::vector<ports::TaskRecord>> PublishingPublicationRepository::in_status(domain::TaskStatus status) {
    return inner_->in_status(status);
}

domain::Result<ports::FileAssetRecord> PublishingPublicationRepository::get_asset(const std::string& owner_id,
                                                                                  const std::string& file_id) {
    return inner_->get_asset(owner_id, file_id);
}

domain::Result<ports::FileAssetRecord> PublishingPublicationRepository::mark_missing(const std::string& file_id) {
    return inner_->mark_missing(file_id);
}

} // namespace music_service::application::events
