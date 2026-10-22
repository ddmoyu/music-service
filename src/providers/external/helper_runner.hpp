#pragma once

#include "application/config/service_config.hpp"
#include "application/ports/providers/external_helper.hpp"
#include "application/ports/runtime/admission.hpp"
#include "application/ports/runtime/clock.hpp"
#include "infrastructure/observability/logger.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace music_service::providers::external {

struct HelperRunnerOptions {
    std::map<std::string, application::config::ExternalAdapterConfig> adapters;
    // Parent of the per-call working directories (data_dir/work/helpers).
    std::filesystem::path work_root;
    // upstream.max_response_bytes: the most a helper may write to stdout per call.
    std::uint64_t max_response_bytes{2 * application::config::mib};
};

// Runs configured helper programs, one process per call (T26). Each call is admitted
// first: one helper process plus the adapter's declared connection upper bound,
// against the adapter's source, never blocking. The process gets a fresh working
// directory, a fixed argument list and a minimal environment; requests reach it only
// as protocol data. The call returns once the whole process tree has exited: after
// its reply, on cancellation (cancel message, stdin closed, SIGTERM on POSIX, then a
// kill after stop_grace) or on timeout, crash or a protocol violation. Only then is
// the lease released, so the quota never comes back while a helper still runs.
// There is no queue: a full ledger answers Deferred. Thread-safe.
class HelperRunner final : public application::ports::ExternalHelper {
public:
    HelperRunner(HelperRunnerOptions options, std::shared_ptr<application::ports::UpstreamAdmissionPort> admission,
                 std::shared_ptr<const application::ports::Clock> clock,
                 std::optional<infrastructure::observability::Logger> log = std::nullopt);

    domain::Result<application::ports::HelperOutcome<application::ports::HelperAudioLocation>>
    extract_audio(const application::ports::HelperCall& call, const application::ports::HelperExtractRequest& request,
                  const application::ports::OperationContext& context) override;

    // Helper processes started since construction, for tests and status.
    [[nodiscard]] std::uint64_t started() const noexcept { return started_; }

private:
    struct Session;

    HelperRunnerOptions options_;
    std::shared_ptr<application::ports::UpstreamAdmissionPort> admission_;
    std::shared_ptr<const application::ports::Clock> clock_;
    std::optional<infrastructure::observability::Logger> log_;
    std::atomic<std::uint64_t> started_{0};
    std::atomic<std::uint64_t> next_id_{0};
};

} // namespace music_service::providers::external
