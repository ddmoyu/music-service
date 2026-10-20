#include "server/process_host.hpp"

#include "api/httplib/control_api.hpp"
#include "application/accounts/credential_service.hpp"
#include "application/accounts/identity_admin.hpp"
#include "infrastructure/auth/access_tokens.hpp"
#include "infrastructure/filesystem/paths.hpp"
#include "infrastructure/secrets/file_secret_store.hpp"
#include "infrastructure/transport/httplib_transport.hpp"
#include "providers/registry/builtin_bundles.hpp"
#include "providers/registry/provider_registry.hpp"
#include "runtime/admission/admission_ledger.hpp"
#include "server/instance.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>
#include <utility>

namespace music_service::server {

namespace {

namespace cfg = application::config;
namespace observability = infrastructure::observability;
using namespace std::chrono_literals;

std::string base_url(const cfg::ListenerConfig& listener, std::uint16_t port,
                     const std::optional<std::string>& configured) {
    if (configured) {
        return *configured;
    }
    const bool ipv6 = listener.host.find(':') != std::string::npos;
    return "http://" + (ipv6 ? "[" + listener.host + "]" : listener.host) + ":" + std::to_string(port);
}

runtime::AdmissionLimits admission_limits(const cfg::ServiceConfig& config) {
    runtime::AdmissionLimits limits;
    limits.total_connections = config.upstream.max_total_connections;
    limits.per_principal = config.upstream.per_principal;
    limits.per_origin = config.upstream.per_origin;
    limits.per_source = {
        {application::ports::TrafficClass::short_request, config.upstream.per_source_short},
        {application::ports::TrafficClass::auth, config.upstream.per_source_short},
        {application::ports::TrafficClass::download, config.downloads.per_source},
        {application::ports::TrafficClass::media, config.media_limits.upstream},
    };
    return limits;
}

} // namespace

struct ProcessHost::State {
    // Declared in startup order; stop() releases them in reverse, the instance (and
    // with it the database and the lock) last.
    std::unique_ptr<Instance> instance;
    observability::Logger log;
    std::shared_ptr<infrastructure::secrets::FileSecretStore> secrets;
    std::shared_ptr<infrastructure::auth::MbedTlsTokenCodec> codec;
    std::shared_ptr<application::accounts::AccessTokenAuthenticator> authenticator;
    std::shared_ptr<application::accounts::CredentialService> credentials;
    std::vector<DestinationState> destinations;
    std::shared_ptr<const providers::ProviderRegistry> registry;
    std::shared_ptr<runtime::AdmissionLedger> admission;
    std::shared_ptr<infrastructure::transport::HttplibTransport> transport;
    std::unique_ptr<api::ControlApi> control;

    StartupReport report;
    std::chrono::system_clock::time_point started_at;
    std::uint32_t control_workers{0};
    std::atomic<bool> ready{false};
    std::atomic<bool> stopping{false};
    std::once_flag stopped;

    explicit State(observability::Logger logger) : log(std::move(logger)) {}

    api::AdminStatus status() const {
        api::AdminStatus s;
        s.state = stopping ? "stopping" : ready ? "ready" : "starting";
        s.instance_id = report.instance_id;
        s.version = std::string{service_version};
        s.started_at = started_at;
        s.storage_available = instance && instance->database != nullptr;
        s.providers_registered = registry ? registry->size() : 0;
        s.destinations = destinations.size();
        s.control_workers = control_workers;
        if (admission) {
            const auto usage = admission->usage();
            s.upstream_connections_in_use = usage.total;
            s.upstream_cooldowns = usage.cooldowns.size();
        }
        return s;
    }

    std::vector<api::DestinationView> destinations_for(const std::string& principal) const {
        std::vector<api::DestinationView> views;
        for (const auto& d : destinations) {
            const auto& allowed = d.config.principals;
            if (std::find(allowed.begin(), allowed.end(), principal) == allowed.end()) {
                continue;
            }
            std::error_code ec;
            views.push_back({d.config.id, d.config.name, std::filesystem::is_directory(d.root, ec),
                             d.config.accept_new_tasks});
        }
        return views;
    }

    void shutdown() {
        std::call_once(stopped, [this] {
            stopping = true;
            ready = false;
            log.info("shutdown.started");
            if (control) {
                control->stop(); // no new requests; those in flight finish
                control.reset();
            }
            log.info("shutdown.listeners_stopped");
            transport.reset();
            admission.reset();
            registry.reset();
            credentials.reset();
            authenticator.reset();
            codec.reset();
            secrets.reset();
            log.info("shutdown.services_released");
            if (instance) {
                instance->identities.reset();
                instance->credentials.reset();
                instance->destinations.reset();
                instance->database.reset();
                log.info("shutdown.database_closed");
                instance->lock.release();
                log.info("shutdown.lock_released");
                instance.reset();
            }
        });
    }
};

ProcessHost::ProcessHost(std::unique_ptr<State> state) : state_(std::move(state)) {}

ProcessHost::~ProcessHost() { stop(); }

const StartupReport& ProcessHost::report() const noexcept { return state_->report; }

void ProcessHost::stop() {
    if (state_) {
        state_->shutdown();
    }
}

void ProcessHost::run_until(const std::function<bool()>& should_stop) {
    while (!should_stop()) {
        std::this_thread::sleep_for(100ms);
    }
    stop();
}

domain::Result<std::unique_ptr<ProcessHost>> ProcessHost::start(const cfg::ServiceConfig& config,
                                                                HostOptions options) {
    auto clock = std::make_shared<infrastructure::SystemClock>();
    auto sink = options.log_sink ? options.log_sink : std::make_shared<observability::JsonLineSink>(std::cerr);
    auto level = observability::parse_log_level(config.logging.level);
    auto state = std::make_unique<State>(
        observability::Logger{sink, level ? level.value() : observability::LogLevel::info, clock});
    // On any early return the partially built state is torn down in reverse order.
    struct Undo {
        State* state;
        ~Undo() {
            if (state) {
                state->shutdown();
            }
        }
    } undo{state.get()};
    const auto fail = [&](domain::Error error, std::string_view step) {
        state->log.error("startup.failed", {{"step", std::string{step}},
                                            {"code", std::string{domain::to_string(error.code)}},
                                            {"message", error.message}});
        return error;
    };

    state->started_at = clock->now();
    auto instance = open_instance(config);
    if (!instance) {
        return fail(std::move(instance).error(), "instance");
    }
    state->instance = std::move(instance).value();
    auto& inst = *state->instance;
    state->report.instance_id = inst.ids->next("inst_");
    state->log.info("startup.database_ready",
                    {{"data_dir", infrastructure::filesystem::path_to_utf8(inst.layout.root)}});

    state->secrets = std::make_shared<infrastructure::secrets::FileSecretStore>(inst.layout.credentials);
    state->codec = std::make_shared<infrastructure::auth::MbedTlsTokenCodec>();
    state->authenticator =
        std::make_shared<application::accounts::AccessTokenAuthenticator>(inst.identities, state->codec);

    auto destinations = register_destinations(inst, config);
    if (!destinations) {
        return fail(std::move(destinations).error(), "destinations");
    }
    state->destinations = std::move(destinations).value();
    for (const auto& d : state->destinations) {
        if (!d.available) {
            state->log.warn("startup.destination_unavailable", {{"destination_id", d.config.id}});
        }
    }

    auto registry = providers::ProviderRegistry::build(
        options.bundles ? std::move(*options.bundles) : providers::builtin_bundles(), config.providers);
    if (!registry) {
        return fail(std::move(registry).error(), "providers");
    }
    state->registry = std::move(registry).value();
    state->credentials = std::make_shared<application::accounts::CredentialService>(
        inst.credentials, state->secrets, state->registry, clock);
    // Before any request can create a credential: reclaim secrets no row references.
    auto reclaimed = state->credentials->reclaim_orphan_secrets();
    if (!reclaimed) {
        return fail(std::move(reclaimed).error(), "secrets");
    }
    if (reclaimed.value() > 0) {
        state->log.info("startup.orphan_secrets_removed", {{"count", static_cast<std::uint64_t>(reclaimed.value())}});
    }

    state->admission = std::make_shared<runtime::AdmissionLedger>(admission_limits(config), clock);
    infrastructure::transport::HttplibTransportOptions transport_options;
    transport_options.connect_timeout = config.upstream.connect_timeout;
    transport_options.max_response_bytes = static_cast<std::size_t>(config.upstream.max_response_bytes);
    transport_options.redirect_max = config.upstream.redirect_max;
    auto transport = infrastructure::transport::HttplibTransport::create(transport_options);
    if (!transport) {
        return fail(std::move(transport).error(), "transport");
    }
    state->transport = std::move(transport).value();

    api::ControlApiOptions api_options{config.control, config.http, config.cors, config.catalog,
                                       config.download_limits, config.media_limits, {}};
    api_options.info.version = std::string{service_version};
    api_options.info.instance_id = state->report.instance_id;
    // Nothing that later tasks deliver is declared: no search, downloads, media or SSE yet.
    State* raw = state.get();
    api::ControlApiServices services{
        state->authenticator,
        state->credentials,
        state->registry,
        [raw](const std::string& principal) { return raw->destinations_for(principal); },
        [raw] { return raw->ready.load(); },
        [raw] { return raw->status(); },
        [ids = inst.ids] { return ids->next("req_"); },
        [raw](const std::string& request_id, const std::string& method, const std::string& path, int status,
              std::chrono::milliseconds elapsed) {
            raw->log.info("http.request", {{"request_id", request_id},
                                           {"method", method},
                                           {"path", path},
                                           {"status", status},
                                           {"elapsed_ms", static_cast<std::int64_t>(elapsed.count())}});
        }};
    state->control = std::make_unique<api::ControlApi>(std::move(api_options), std::move(services));
    state->control_workers = config.control.workers;
    auto port = state->control->bind();
    if (!port) {
        return fail(std::move(port).error(), "control_listener");
    }
    state->control->start();
    state->report.api_base_url = base_url(config.control, port.value(), config.public_urls.api_base);

    state->ready = true;
    state->log.info("startup.ready", {{"instance_id", state->report.instance_id},
                                      {"api_base_url", state->report.api_base_url}});
    undo.state = nullptr;
    return std::unique_ptr<ProcessHost>{new ProcessHost(std::move(state))};
}

} // namespace music_service::server
