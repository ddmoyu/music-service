#pragma once

#include "application/ports/storage/task_repository.hpp"

#include "domain/result.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace music_service::application::ports {

// Durable record that a task is publishing a verified work file to a reserved target
// (internal-contracts.md, 文件发布与崩溃矩阵). Paths are UTF-8 and relative: the
// workspace file to its destination root, the target to the same root.
struct PublicationIntent {
    std::string task_id;
    std::string execution_id;
    std::string destination_id;
    std::string workspace_file;
    std::string reserved_path;
    std::string evidence_json; // size, hash and other proof of the produced bytes
    std::string stage;         // reserved | moved
};

enum class FileAssetStatus { available, missing };

struct NewFileAsset {
    std::string owner_id;
    std::string destination_id;
    std::string relative_path;
    std::uint64_t size_bytes{0};
    std::optional<std::string> container;
    std::string etag;
    std::string properties_json; // actual track, quality, metadata status
};

struct FileAssetRecord {
    std::string id;
    std::string owner_id;
    std::string destination_id;
    std::string relative_path;
    std::uint64_t size_bytes{0};
    std::optional<std::string> container;
    std::string etag;
    FileAssetStatus status{FileAssetStatus::available};
    std::string properties_json;
    std::uint64_t version{1};
    std::optional<std::string> created_by_task;
    std::chrono::system_clock::time_point created_at;
};

struct CompletedPublication {
    TaskRecord task;
    FileAssetRecord asset;
};

// Publication steps that must commit together with task state (data-models.md,
// 文件与数据库的协调). Each write checks the task's execution id and version.
class PublicationRepository {
public:
    virtual ~PublicationRepository() = default;

    // processing -> finalizing, plus the intent and its path reservation. A target path
    // already used by an asset or another intent (compared case-insensitively) is
    // CONFLICT; the task is unchanged then.
    virtual domain::Result<TaskRecord> begin(const PublicationIntent& intent,
                                             std::uint64_t expected_version) = 0;

    virtual domain::Result<void> set_stage(const std::string& task_id,
                                           const std::string& execution_id,
                                           const std::string& stage) = 0;

    // finalizing -> completed: inserts the asset and the task-file relation, stores the
    // result and removes the intent.
    virtual domain::Result<CompletedPublication> complete(const std::string& task_id,
                                                          const std::string& execution_id,
                                                          std::uint64_t expected_version,
                                                          const NewFileAsset& asset,
                                                          const std::string& result_json) = 0;

    // finalizing -> failed or waiting_retry, releasing the reservation.
    virtual domain::Result<TaskRecord> abandon(const std::string& task_id,
                                               const std::string& execution_id,
                                               std::uint64_t expected_version,
                                               domain::TaskStatus to,
                                               const std::string& error_json) = 0;

    // Intents left behind by a crash, for startup recovery.
    virtual domain::Result<std::vector<PublicationIntent>> pending() = 0;

    virtual domain::Result<FileAssetRecord> get_asset(const std::string& owner_id,
                                                      const std::string& file_id) = 0;
    virtual domain::Result<FileAssetRecord> mark_missing(const std::string& file_id) = 0;
};

} // namespace music_service::application::ports
