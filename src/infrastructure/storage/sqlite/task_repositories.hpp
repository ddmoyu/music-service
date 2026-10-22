#pragma once

#include "application/ports/storage/maintenance.hpp"
#include "application/ports/storage/publication_repository.hpp"
#include "application/ports/storage/task_repository.hpp"
#include "infrastructure/storage/sqlite/account_repositories.hpp"

#include <chrono>
#include <cstdint>

namespace music_service::infrastructure::storage::sqlite {

struct TaskRepositoryOptions {
    std::uint32_t per_principal_active{500};
    std::uint32_t global_active{2000};
    std::chrono::hours idempotency_retention{24};
};

class SqliteTaskRepository final : public application::ports::TaskRepository {
public:
    SqliteTaskRepository(RepositoryContext context, TaskRepositoryOptions options)
        : ctx_(std::move(context)), options_(options) {}

    domain::Result<std::optional<application::ports::StoredResponse>>
    find_idempotent(const std::string& owner_id, const std::string& route,
                    const std::string& idempotency_key) override;
    domain::Result<std::vector<application::ports::CompletedResult>>
    completed_results(const std::string& owner_id, const domain::DownloadFingerprint& fingerprint) override;
    domain::Result<application::ports::CreateTaskOutcome>
    create(const application::ports::CreateTaskCommand& command) override;
    domain::Result<application::ports::BatchOutcome>
    create_batch(const application::ports::CreateBatchCommand& command) override;
    domain::Result<application::ports::BatchRecord> get_batch(const std::string& owner_id,
                                                              const std::string& batch_id) override;
    domain::Result<std::vector<application::ports::TaskRecord>>
    list(const application::ports::TaskQuery& query) override;
    domain::Result<application::ports::TaskRecord> get(const std::string& owner_id,
                                                       const std::string& task_id) override;
    domain::Result<domain::NormalizedDownloadIntent> get_intent(const std::string& task_id) override;
    domain::Result<std::optional<application::ports::TaskRecord>>
    claim_next(std::chrono::system_clock::time_point now) override;
    domain::Result<std::vector<application::ports::RunnableTask>>
    runnable(std::chrono::system_clock::time_point now, std::uint32_t per_owner, std::uint32_t limit) override;
    domain::Result<std::optional<application::ports::TaskRecord>>
    claim(const std::string& task_id, std::chrono::system_clock::time_point now) override;
    domain::Result<application::ports::TaskRecord>
    begin_attempt(const application::ports::BeginAttemptCommand& command) override;
    domain::Result<application::ports::TaskRecord> defer(const application::ports::DeferCommand& command) override;
    domain::Result<application::ports::TaskRecord>
    checkpoint(const application::ports::CheckpointCommand& command) override;
    domain::Result<application::ports::TaskRecord>
    transition(const application::ports::TransitionCommand& command) override;
    domain::Result<std::vector<application::ports::TaskRecord>>
    requeue_interrupted(std::chrono::system_clock::time_point now) override;
    domain::Result<application::ports::TaskRecord> request_cancel(const std::string& owner_id,
                                                                  const std::string& task_id) override;

private:
    RepositoryContext ctx_;
    TaskRepositoryOptions options_;
};

class SqlitePublicationRepository final : public application::ports::PublicationRepository {
public:
    explicit SqlitePublicationRepository(RepositoryContext context) : ctx_(std::move(context)) {}

    domain::Result<application::ports::TaskRecord>
    begin(const application::ports::PublicationIntent& intent,
          std::uint64_t expected_version) override;
    domain::Result<void> set_stage(const std::string& task_id, const std::string& execution_id,
                                   const std::string& stage) override;
    domain::Result<application::ports::CompletedPublication>
    complete(const std::string& task_id, const std::string& execution_id,
             std::uint64_t expected_version, const application::ports::NewFileAsset& asset,
             const std::string& result_json) override;
    domain::Result<application::ports::TaskRecord>
    abandon(const std::string& task_id, const std::string& execution_id,
            std::uint64_t expected_version, domain::TaskStatus to,
            const std::string& error_json) override;
    domain::Result<void> relocate(const std::string& task_id, const std::string& execution_id,
                                  const std::string& reserved_path) override;
    domain::Result<std::vector<application::ports::PendingPublication>> pending() override;
    domain::Result<std::vector<application::ports::TaskRecord>> in_status(domain::TaskStatus status) override;
    domain::Result<application::ports::FileAssetRecord> get_asset(const std::string& owner_id,
                                                                  const std::string& file_id) override;
    domain::Result<application::ports::FileAssetRecord> mark_missing(const std::string& file_id) override;

private:
    RepositoryContext ctx_;
};

class SqliteMaintenanceStore final : public application::ports::MaintenanceStore {
public:
    explicit SqliteMaintenanceStore(RepositoryContext context) : ctx_(std::move(context)) {}

    domain::Result<std::vector<application::ports::WorkspaceTask>>
    workspace_tasks(const std::vector<std::string>& task_ids) override;
    domain::Result<std::size_t> purge_expired_idempotency(std::chrono::system_clock::time_point now,
                                                          std::size_t limit) override;

private:
    RepositoryContext ctx_;
};

} // namespace music_service::infrastructure::storage::sqlite
