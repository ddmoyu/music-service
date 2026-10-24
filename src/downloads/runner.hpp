#pragma once

#include "application/accounts/credential_service.hpp"
#include "application/ports/processing/audio_processor.hpp"
#include "application/ports/providers/directory.hpp"
#include "application/ports/runtime/clock.hpp"
#include "application/ports/runtime/executor.hpp"
#include "application/ports/storage/destination_files.hpp"
#include "application/ports/storage/task_repository.hpp"
#include "application/ports/storage/work_files.hpp"
#include "application/ports/transport/upstream_http.hpp"
#include "domain/result.hpp"
#include "downloads/finalizer.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace music_service::downloads {

struct RunnerOptions {
    std::uint32_t max_attempts{3};      // downloads.max_attempts, the first included
    std::uint32_t transport_retries{2}; // downloads.transport_retries, within one attempt
    std::uint64_t max_file_bytes{2ULL * 1024 * 1024 * 1024};
    std::chrono::milliseconds initial_backoff{1000};   // retry.initial_backoff
    std::uint32_t backoff_multiplier{2};               // retry.multiplier
    // Upper bound on an upstream Retry-After that moves the next attempt later; the
    // admission ledger caps its cooldowns the same way.
    std::chrono::seconds max_retry_after{300};
    std::chrono::seconds attempt_budget{600};          // download_timeouts.attempt
    std::chrono::seconds absolute_budget{1800};        // download_timeouts.absolute
    std::chrono::milliseconds progress_interval{1000}; // event_stream.progress_interval
    // When an execution that could not be admitted is claimed again; the scheduler also
    // skips sources the admission ledger reports as blocked.
    std::chrono::milliseconds admission_retry{1000};
    // Bounded retries of a storage write the execution must not lose (it keeps the task).
    std::uint32_t storage_retries{3};
};

// Why an execution was told to stop.
enum class StopReason { none, canceled, shutdown };

// The control a scheduler keeps for one running execution.
struct ExecutionControl {
    std::stop_source stop;
    std::atomic<StopReason> reason{StopReason::none};

    // The first reason wins: a cancel is not turned into a shutdown pause, or back.
    void request(StopReason why) {
        auto expected = StopReason::none;
        reason.compare_exchange_strong(expected, why);
        stop.request_stop();
    }
};

// How an execution ended, for logs and tests.
enum class RunOutcome {
    completed,
    failed,
    canceled,
    waiting_admission, // paused without counting an attempt (admission, shutdown)
    waiting_retry,     // the attempt failed and another one is planned
    left,              // another execution or a publication left for recovery owns it now
};

std::string_view to_string(RunOutcome outcome) noexcept;

struct RunReport {
    RunOutcome outcome{RunOutcome::left};
    std::string detail; // error code or reason, no secrets
    std::chrono::nanoseconds elapsed{}; // one claimed execution, including retries and processing
};

struct RunnerServices {
    std::shared_ptr<ports::TaskRepository> tasks;
    std::shared_ptr<const ports::ProviderDirectory> providers;
    std::shared_ptr<application::accounts::CredentialService> credentials;
    std::shared_ptr<ports::UpstreamHttp> upstream;
    std::shared_ptr<ports::WorkFiles> work_files;
    std::shared_ptr<ports::DestinationFiles> files;
    std::shared_ptr<Finalizer> finalizer;
    // The processor a resolved resource names (CAPABILITY_UNSUPPORTED when unknown).
    std::function<domain::Result<std::shared_ptr<ports::AudioProcessor>>(const ports::ProcessingPlan&)> processors;
    // Bounded processing pool; processing runs on the runner's thread when empty.
    std::shared_ptr<ports::WorkExecutor> processing;
    std::shared_ptr<const ports::Clock> clock;
    // Display text (title, artist) -> one safe path component.
    std::function<std::string(std::string_view)> file_name;
};

// Runs one claimed execution of a download (runtime-and-tasks.md, 执行阶段) until the task
// is durably completed, failed, canceled or waiting again; nothing is left to a thread
// that sleeps:
//   - uses the task's fixed account binding, leased at its latest valid version, never the
//     owner's current default (A39);
//   - begins a logical attempt only once its work really starts: claims after an admission
//     pause continue the open attempt, and the first attempt fixes the absolute deadline,
//     which pauses and restarts never extend (A26, A44);
//   - every upstream request is admitted on its own; a request that cannot be admitted
//     ends the I/O, then the task is saved as waiting_retry (admission) with its resume
//     stage and evidence before the slot is released (A43);
//   - streams the audio into the task's work file, resuming only with decide_resume's
//     evidence, checkpointing progress at most every progress_interval (unknown totals
//     stay null, A25) and flushing the bytes before a checkpoint counts them;
//   - 429/5xx/expired addresses end the attempt as upstream_retry while attempts and
//     the deadline allow, transport interruptions are retried within the attempt;
//   - processing and publication through the AudioProcessor and the Finalizer;
//   - a cancel stops the I/O, removes the work directory and finishes as canceled; a
//     shutdown saves the checkpoint and leaves the task due again (A27).
// Every write carries the execution id and version, so a late write of an older execution
// is refused (A22).
class DownloadRunner {
public:
    DownloadRunner(RunnerOptions options, RunnerServices services);

    RunReport run(const ports::TaskRecord& claimed, ExecutionControl& control);

private:
    RunnerOptions options_;
    RunnerServices services_;
};

} // namespace music_service::downloads
