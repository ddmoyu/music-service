#pragma once

#include "application/accounts/credential_service.hpp"
#include "application/ports/providers/directory.hpp"
#include "application/ports/runtime/download_queue.hpp"
#include "application/ports/storage/task_repository.hpp"
#include "domain/result.hpp"
#include "domain/tasks/download_intent.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace music_service::application::tasks {

// A configured destination as the service may use it (configuration-and-deployment.md,
// 目标目录绑定与变更). Only the server knows the actual directory.
struct DestinationRule {
    std::string id;
    std::string canonical_root; // UTF-8, as bound
    std::vector<std::string> principals;
    bool accept_new_tasks{true};
};

// One requested download, as parsed from the API.
struct DownloadRequest {
    domain::TrackRef track;
    domain::ResourceRequirement requirement;
    bool automatic_fallback{false};
    bool match_selection{false};
    std::optional<std::string> credential_id;
    std::optional<std::string> destination_id;
    domain::MetadataPolicy metadata_policy{domain::MetadataPolicy::best_effort};
    bool force{false}; // duplicate_policy=force
};

// A task with the request side of its persisted intent, for the public snapshot.
struct TaskView {
    ports::TaskRecord task;
    domain::NormalizedDownloadIntent intent;
};

struct CreateResult {
    TaskView view;
    bool reused{false};
    bool replayed{false};
    int status_code{202}; // 202 new task, 200 reused (or the replayed first status)
};

struct BatchResult {
    std::string batch_id;
    std::vector<TaskView> items; // input order
    std::vector<bool> reused;
    std::uint32_t created_count{0};
    std::uint32_t reused_count{0};
    bool replayed{false};
    int status_code{202};
};

struct TaskFilter {
    std::optional<domain::TaskStatus> status;
    std::optional<std::string> source;
    std::optional<std::string> batch_id;
    std::uint32_t limit{50};
    std::optional<std::string> cursor;
};

struct TaskPage {
    std::vector<TaskView> items;
    std::optional<std::string> next_cursor;
};

struct CancelResult {
    TaskView view;
    bool asynchronous{false}; // 202: the running task stops at its next safe point
};

struct BatchCancelResult {
    std::vector<TaskView> accepted; // canceled now or canceling
    struct Refused {
        TaskView view;
        domain::Error reason;
    };
    std::vector<Refused> refused; // finalizing or already finished
    bool asynchronous{false};
};

// Download commands and queries (application/tasks). Intents are prepared outside the
// write transaction: idempotent replays are answered first, then the account binding
// is fixed, the destination checked and defaults filled; the repository re-checks
// idempotency, active duplicates, quotas and any completed result in one transaction.
// Matching and automatic fallback are not available yet (CAPABILITY_UNSUPPORTED). After a
// commit the download queue is woken, and a cancel of a running task signals its runner.
class DownloadService {
public:
    static constexpr std::string_view create_route = "/api/v1/downloads";
    static constexpr std::string_view batch_route = "/api/v1/download-batches";
    static constexpr std::uint32_t max_page = 100;

    DownloadService(std::shared_ptr<ports::TaskRepository> tasks, std::shared_ptr<const ports::ProviderDirectory> providers,
                    std::shared_ptr<accounts::CredentialService> credentials, std::vector<DestinationRule> destinations,
                    std::function<bool(const DestinationRule&)> destination_available,
                    std::function<bool(const std::string& destination_id, const std::string& relative_path,
                                       std::uint64_t size_bytes)>
                        file_intact,
                    std::uint32_t max_batch_items, std::shared_ptr<ports::DownloadQueue> queue = {});

    // Idempotency-Key: 16 to 128 printable ASCII characters without spaces.
    static domain::Result<void> check_idempotency_key(std::string_view key);

    domain::Result<CreateResult> create(const std::string& owner_id, const DownloadRequest& request,
                                        const std::string& idempotency_key, const std::string& request_hash);
    domain::Result<BatchResult> create_batch(const std::string& owner_id, const std::vector<DownloadRequest>& items,
                                             const std::string& idempotency_key, const std::string& request_hash);
    // Keeps the persisted intent, bindings and destination; re-checks today's
    // capability, destination and that each bound account can still be leased.
    domain::Result<CreateResult> retry(const std::string& owner_id, const std::string& task_id,
                                       const std::string& idempotency_key, const std::string& request_hash);

    domain::Result<TaskView> get(const std::string& owner_id, const std::string& task_id);
    domain::Result<TaskPage> list(const std::string& owner_id, const TaskFilter& filter);
    domain::Result<CancelResult> cancel(const std::string& owner_id, const std::string& task_id);

    struct BatchView {
        ports::BatchRecord batch;
        std::vector<TaskView> items;
    };
    domain::Result<BatchView> get_batch(const std::string& owner_id, const std::string& batch_id);
    domain::Result<BatchCancelResult> cancel_batch(const std::string& owner_id, const std::string& batch_id);

private:
    domain::Result<const DestinationRule*> destination_for(const std::string& owner_id,
                                                           const std::optional<std::string>& destination_id) const;
    domain::Result<domain::NormalizedDownloadIntent> prepare(const std::string& owner_id, const DownloadRequest& request);
    std::optional<ports::ReuseCandidate> reusable(const domain::NormalizedDownloadIntent& intent);
    domain::Result<TaskView> view_of(ports::TaskRecord task);
    void wake() const;

    std::shared_ptr<ports::TaskRepository> tasks_;
    std::shared_ptr<const ports::ProviderDirectory> providers_;
    std::shared_ptr<accounts::CredentialService> credentials_;
    std::vector<DestinationRule> destinations_;
    std::function<bool(const DestinationRule&)> destination_available_;
    std::function<bool(const std::string&, const std::string&, std::uint64_t)> file_intact_;
    std::uint32_t max_batch_items_;
    std::shared_ptr<ports::DownloadQueue> queue_; // empty: tasks only queue
};

} // namespace music_service::application::tasks
