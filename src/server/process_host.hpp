#pragma once

#include "application/config/service_config.hpp"
#include "application/ports/providers/bundle.hpp"
#include "domain/result.hpp"
#include "infrastructure/observability/logger.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace music_service::server {

inline constexpr std::string_view service_version = MUSIC_SERVICE_VERSION;

// Published once every listener is bound; the desktop launcher reads it from stdout.
struct StartupReport {
    std::string instance_id;
    std::string api_base_url;
    std::optional<std::string> media_base_url;
    std::optional<std::string> events_base_url;
};

struct HostOptions {
    std::shared_ptr<infrastructure::observability::LogSink> log_sink; // stderr JSON lines when empty
    // The platform packages to register; the build's built-in manifest when empty.
    std::optional<std::vector<application::ports::ProviderBundle>> bundles;
};

// The serving process (configuration-and-deployment.md, 启动与退出). start() runs the
// startup order: instance lock -> database and migrations -> identities and secrets
// -> destination bindings -> providers and transport -> listeners -> ready. Any
// failure before ready undoes what was opened and returns the error; nothing is
// served half-initialized. stop() reverses it: readiness off, listeners stopped,
// services released, the database closed last, then the lock.
class ProcessHost {
public:
    static domain::Result<std::unique_ptr<ProcessHost>> start(const application::config::ServiceConfig& config,
                                                              HostOptions options = {});
    ~ProcessHost();
    ProcessHost(const ProcessHost&) = delete;
    ProcessHost& operator=(const ProcessHost&) = delete;

    [[nodiscard]] const StartupReport& report() const noexcept;

    // Blocks until should_stop() returns true (polled), then stops.
    void run_until(const std::function<bool()>& should_stop);
    // Idempotent; safe from any thread except a request handler.
    void stop();

private:
    struct State;
    explicit ProcessHost(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

} // namespace music_service::server
