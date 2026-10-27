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

// An intent left behind with the task it belongs to (finalizing, same execution).
struct PendingPublication {
    PublicationIntent intent;
    TaskRecord task;
};

// The exact asset whose publication path was checked absent outside the transaction.
// Releasing its name must preserve its identity/history and make it permanently missing.
struct MissingPathAsset {
    std::string file_id;
    std::uint64_t version{0};
};

// Publication steps that must commit together with task state (data-models.md,
// 文件与数据库的协调). Each write checks the task's execution id and version.
class PublicationRepository {
public:
    virtual ~PublicationRepository() = default;

    // processing -> finalizing, plus the intent and its path reservation. A target path
    // already used by an asset or another intent (compared case-insensitively) is
    // CONFLICT. A checked missing asset can release its name in the same transaction;
    // its version and path must still match. Failure rolls back both changes.
    virtual domain::Result<TaskRecord> begin(const PublicationIntent& intent,
                                             std::uint64_t expected_version,
                                             const std::optional<MissingPathAsset>& missing = {}) = 0;

    virtual domain::Result<void> set_stage(const std::string& task_id,
                                           const std::string& execution_id,
                                           const std::string& stage) = 0;

    // Moves the reservation to another path when an unknown file occupies the reserved
    // one, so it is never overwritten. CONFLICT when the new path is taken; the intent
    // keeps its old path then.
    virtual domain::Result<void> relocate(const std::string& task_id, const std::string& execution_id,
                                          const std::string& reserved_path,
                                          const std::optional<MissingPathAsset>& missing = {}) = 0;

    // Internal publication lookup, including missing assets which still hold their
    // original name. The caller checks the recorded path (including its original case),
    // never just the requested spelling. Bundle assets hold the bundle directory name.
    virtual domain::Result<std::optional<FileAssetRecord>>
    asset_at_path(const std::string& destination_id, const std::string& reserved_path) = 0;

    // finalizing -> completed: inserts the asset and the task-file relation, stores the
    // result (a JSON object, completed with the new "file_id") and removes the intent.
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

    // Intents left behind by a crash, oldest first, for startup recovery.
    virtual domain::Result<std::vector<PendingPublication>> pending() = 0;

    // Tasks of any owner in a status only startup recovery resolves once their execution
    // is gone (finalizing without an intent, canceling).
    virtual domain::Result<std::vector<TaskRecord>> in_status(domain::TaskStatus status) = 0;

    virtual domain::Result<FileAssetRecord> get_asset(const std::string& owner_id,
                                                      const std::string& file_id) = 0;
    virtual domain::Result<FileAssetRecord> mark_missing(const std::string& file_id) = 0;
};

} // namespace music_service::application::ports
