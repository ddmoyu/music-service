#pragma once

#include "application/events/task_event_hub.hpp"
#include "application/ports/storage/publication_repository.hpp"

#include <memory>

namespace music_service::application::events {

// Publishes the task versions publication writes commit (finalizing, completed and the
// failed or waiting_retry of an abandoned publication), like PublishingTaskRepository.
class PublishingPublicationRepository final : public ports::PublicationRepository {
public:
    PublishingPublicationRepository(std::shared_ptr<ports::PublicationRepository> inner,
                                    std::shared_ptr<TaskEventHub> hub);

    domain::Result<ports::TaskRecord> begin(const ports::PublicationIntent& intent,
                                            std::uint64_t expected_version,
                                            const std::optional<ports::MissingPathAsset>& missing = {}) override;
    domain::Result<void> set_stage(const std::string& task_id, const std::string& execution_id,
                                   const std::string& stage) override;
    domain::Result<void> relocate(const std::string& task_id, const std::string& execution_id,
                                  const std::string& reserved_path,
                                  const std::optional<ports::MissingPathAsset>& missing = {}) override;
    domain::Result<std::optional<ports::FileAssetRecord>>
    asset_at_path(const std::string& destination_id, const std::string& reserved_path) override;
    domain::Result<ports::CompletedPublication> complete(const std::string& task_id, const std::string& execution_id,
                                                         std::uint64_t expected_version,
                                                         const ports::NewFileAsset& asset,
                                                         const std::string& result_json) override;
    domain::Result<ports::TaskRecord> abandon(const std::string& task_id, const std::string& execution_id,
                                              std::uint64_t expected_version, domain::TaskStatus to,
                                              const std::string& error_json) override;
    domain::Result<std::vector<ports::PendingPublication>> pending() override;
    domain::Result<std::vector<ports::TaskRecord>> in_status(domain::TaskStatus status) override;
    domain::Result<ports::FileAssetRecord> get_asset(const std::string& owner_id, const std::string& file_id) override;
    domain::Result<ports::FileAssetRecord> mark_missing(const std::string& file_id) override;

private:
    std::shared_ptr<ports::PublicationRepository> inner_;
    std::shared_ptr<TaskEventHub> hub_;
};

} // namespace music_service::application::events
