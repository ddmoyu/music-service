#include "server/process_host.hpp"

#include "api/httplib/control_api.hpp"
#include "application/accounts/credential_service.hpp"
#include "application/accounts/identity_admin.hpp"
#include "application/catalog/asset_service.hpp"
#include "application/catalog/catalog_service.hpp"
#include "application/events/publishing_publication_repository.hpp"
#include "application/events/publishing_task_repository.hpp"
#include "application/events/task_event_hub.hpp"
#include "application/tasks/download_service.hpp"
#include "audio_processing/verifying_processor.hpp"
#include "downloads/recovery.hpp"
#include "downloads/runner.hpp"
#include "downloads/scheduler.hpp"
#include "application/transport/upstream_client.hpp"
#include "event_server/httplib/event_server.hpp"
#include "infrastructure/auth/access_tokens.hpp"
#include "infrastructure/filesystem/destination_files.hpp"
#include "infrastructure/filesystem/paths.hpp"
#include "infrastructure/secrets/file_secret_store.hpp"
#include "infrastructure/transport/httplib_transport.hpp"
#include "providers/registry/builtin_bundles.hpp"
#include "providers/registry/provider_registry.hpp"
#include "runtime/admission/admission_ledger.hpp"
#include "runtime/executors/bounded_executor.hpp"
#include "server/instance.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
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

// admin/download-policy on the scheduler.
class SchedulerPolicy final : public api::DownloadPolicy {
public:
    explicit SchedulerPolicy(std::shared_ptr<downloads::DownloadScheduler> scheduler)
        : scheduler_(std::move(scheduler)) {}
    [[nodiscard]] bool paused() const override { return scheduler_->paused(); }
    void set_paused(bool paused) override { scheduler_->set_paused(paused); }

private:
    std::shared_ptr<downloads::DownloadScheduler> scheduler_;
};

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
    std::shared_ptr<infrastructure::filesystem::LocalDestinationFiles> files;
    std::shared_ptr<const providers::ProviderRegistry> registry;
    std::shared_ptr<runtime::AdmissionLedger> admission;
    std::shared_ptr<infrastructure::transport::HttplibTransport> transport;
    std::shared_ptr<application::transport::UpstreamClient> upstream;
    std::shared_ptr<runtime::BoundedExecutor> catalog_executor;
    std::shared_ptr<application::catalog::CatalogService> catalog;
    std::shared_ptr<application::catalog::AssetService> assets;
    std::shared_ptr<application::events::TaskEventHub> events_hub;
    std::shared_ptr<application::tasks::DownloadService> downloads;
    std::shared_ptr<runtime::BoundedExecutor> processing_executor;
    std::shared_ptr<runtime::BoundedExecutor> download_executor;
    std::shared_ptr<downloads::DownloadScheduler> scheduler;
    std::unique_ptr<event_server::EventServer> events;
    std::unique_ptr<api::ControlApi> control;

    StartupReport report;
    std::chrono::system_clock::time_point started_at;
    std::uint32_t control_workers{0};
    std::chrono::seconds soft_shutdown{15};
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
        if (scheduler) {
            const auto d = scheduler->stats();
            s.download_workers = d.workers;
            s.downloads_running = d.running;
            s.downloads_paused = d.paused;
            s.downloads_claimed = d.claimed;
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
            if (scheduler) {
                scheduler->stop_claiming(); // no new executions from here on
            }
            if (events) {
                events->stop(); // every stream ends; clients resynchronize after restart
                events.reset();
            }
            if (control) {
                control->stop(); // no new requests; those in flight finish
                control.reset();
            }
            log.info("shutdown.listeners_stopped");
            if (scheduler) {
                // Running downloads may finish within the soft budget; the others save their
                // checkpoint and wait to be claimed again after the restart (service_shutdown).
                if (!scheduler->wait_idle(soft_shutdown)) {
                    log.info("shutdown.downloads_paused",
                             {{"running", static_cast<std::uint64_t>(scheduler->stats().running)}});
                    scheduler->stop_running();
                }
                download_executor->stop_accepting();
                download_executor->request_stop();
                download_executor->join();
                processing_executor->stop_accepting();
                processing_executor->request_stop();
                processing_executor->join();
                scheduler->join();
                log.info("shutdown.downloads_stopped");
            }
            if (catalog_executor) {
                // Platform calls still running finish or see the stop signal before the
                // services they use are released.
                catalog_executor->stop_accepting();
                catalog_executor->request_stop();
                catalog_executor->join();
            }
            downloads.reset();
            scheduler.reset();
            download_executor.reset();
            processing_executor.reset();
            events_hub.reset();
            assets.reset();
            catalog.reset();
            catalog_executor.reset();
            registry.reset(); // capability objects hold the upstream client
            upstream.reset();
            transport.reset();
            admission.reset();
            credentials.reset();
            files.reset();
            authenticator.reset();
            codec.reset();
            secrets.reset();
            log.info("shutdown.services_released");
            if (instance) {
                instance->identities.reset();
                instance->credentials.reset();
                instance->destinations.reset();
                instance->publications.reset();
                instance->tasks.reset();
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
    std::shared_ptr<observability::LogSink> sink = options.log_sink;
    if (!sink) {
        if (config.logging.format == "text") {
            sink = std::make_shared<observability::TextSink>(std::cerr);
        } else {
            sink = std::make_shared<observability::JsonLineSink>(std::cerr);
        }
    }
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
    std::map<std::string, std::filesystem::path> roots;
    for (const auto& d : state->destinations) {
        roots.emplace(d.config.id, d.root);
    }
    state->files = std::make_shared<infrastructure::filesystem::LocalDestinationFiles>(std::move(roots));

    // Publications and cancels interrupted by the last exit are settled before anything
    // can claim or query them (configuration-and-deployment.md, 启动与退出).
    auto recovered = downloads::Recovery(inst.publications, inst.tasks, state->files,
                                         downloads::RecoveryOptions{config.downloads.max_attempts})
                         .run();
    if (!recovered) {
        return fail(std::move(recovered).error(), "recovery");
    }
    for (const auto& task : recovered->tasks) {
        const bool attention =
            task.action == downloads::RecoveryAction::failed || task.action == downloads::RecoveryAction::deferred;
        state->log.log(attention ? observability::LogLevel::warn : observability::LogLevel::info,
                       "startup.recovery_task",
                       {{"task_id", task.task_id},
                        {"action", downloads::to_string(task.action)},
                        {"reason", task.reason}});
    }
    if (!recovered->tasks.empty()) {
        state->log.info("startup.recovery",
                        {{"completed", static_cast<std::uint64_t>(recovered->count(downloads::RecoveryAction::completed))},
                         {"retry", static_cast<std::uint64_t>(recovered->count(downloads::RecoveryAction::retry))},
                         {"failed", static_cast<std::uint64_t>(recovered->count(downloads::RecoveryAction::failed))},
                         {"deferred", static_cast<std::uint64_t>(recovered->count(downloads::RecoveryAction::deferred))},
                         {"canceled", static_cast<std::uint64_t>(recovered->count(downloads::RecoveryAction::canceled))}});
    }

    // Executions the last process left in a running stage are due again at once, at the
    // stage their evidence allows; attempts and deadlines are kept.
    auto requeued = inst.tasks->requeue_interrupted(clock->now());
    if (!requeued) {
        return fail(std::move(requeued).error(), "recovery");
    }
    for (const auto& task : requeued.value()) {
        const auto resume = task.resume_stage.value_or(domain::TaskStatus::resolving);
        state->log.info("startup.task_requeued",
                        {{"task_id", task.task_id}, {"resume_stage", std::string{domain::to_string(resume)}}});
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
    state->upstream = std::make_shared<application::transport::UpstreamClient>(state->admission, state->transport, clock);

    auto registry = providers::ProviderRegistry::build(
        options.bundles ? std::move(*options.bundles) : providers::builtin_bundles(), config.providers,
        providers::ProviderServices{state->upstream});
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

    state->catalog_executor = std::make_shared<runtime::BoundedExecutor>(
        "catalog", std::max<std::uint32_t>(1, config.upstream.short_workers), config.upstream.queue);
    application::catalog::CatalogLimits catalog_limits{config.catalog, config.cache, config.handles, config.lifetimes, 4};
    state->catalog = std::make_shared<application::catalog::CatalogService>(
        catalog_limits, state->registry, state->credentials, state->catalog_executor, clock,
        [ids = inst.ids] { return ids->next("cur_"); });
    state->assets = std::make_shared<application::catalog::AssetService>(
        config.lifetimes, config.handles, 4, state->registry, state->credentials, state->upstream, clock,
        [ids = inst.ids] { return ids->next("ast_"); }, config.catalog.total_budget);

    // With SSE enabled, every committed task version is published to the owner's streams.
    std::shared_ptr<application::ports::TaskRepository> tasks = inst.tasks;
    std::shared_ptr<application::ports::PublicationRepository> publications = inst.publications;
    if (config.events.enabled) {
        state->events_hub =
            std::make_shared<application::events::TaskEventHub>(config.event_limits, event_server::encode_task_event);
        tasks = std::make_shared<application::events::PublishingTaskRepository>(inst.tasks, state->events_hub);
        publications =
            std::make_shared<application::events::PublishingPublicationRepository>(inst.publications, state->events_hub);
    }

    // Download execution (T17): runners in downloads.workers slots, processing in its own pool.
    state->processing_executor = std::make_shared<runtime::BoundedExecutor>(
        "processing", std::max<std::uint32_t>(1, config.processing.workers), config.processing.queue);
    const auto download_slots = std::max<std::uint32_t>(1, config.downloads.workers);
    state->download_executor = std::make_shared<runtime::BoundedExecutor>("downloads", download_slots, download_slots);
    auto processors =
        std::make_shared<audio_processing::ProcessorCatalog>(audio_processing::ProcessorCatalog::builtin());
    downloads::RunnerOptions runner_options;
    runner_options.max_attempts = config.downloads.max_attempts;
    runner_options.transport_retries = config.downloads.transport_retries;
    runner_options.max_file_bytes = config.downloads.max_file_bytes;
    runner_options.initial_backoff = config.retry.initial_backoff;
    runner_options.backoff_multiplier = config.retry.multiplier;
    runner_options.attempt_budget = config.download_timeouts.attempt;
    runner_options.absolute_budget = config.download_timeouts.absolute;
    runner_options.progress_interval = config.event_stream.progress_interval;
    downloads::RunnerServices runner_services;
    runner_services.tasks = tasks;
    runner_services.providers = state->registry;
    runner_services.credentials = state->credentials;
    runner_services.upstream = state->upstream;
    runner_services.work_files = state->files;
    runner_services.files = state->files;
    runner_services.finalizer = std::make_shared<downloads::Finalizer>(publications, state->files);
    runner_services.processors = [processors](const application::ports::ProcessingPlan& plan) {
        return processors->find(plan);
    };
    runner_services.processing = state->processing_executor;
    runner_services.clock = clock;
    runner_services.file_name = [](std::string_view text) {
        return infrastructure::filesystem::sanitize_file_name(text);
    };
    auto runner = std::make_shared<downloads::DownloadRunner>(runner_options, std::move(runner_services));
    State* host = state.get();
    state->scheduler = std::make_shared<downloads::DownloadScheduler>(
        downloads::SchedulerOptions{download_slots}, tasks, state->admission, runner, state->download_executor, clock,
        [host](const application::ports::TaskRecord& claimed, const downloads::RunReport& report) {
            host->log.info("download.run", {{"task_id", claimed.task_id},
                                            {"outcome", std::string{downloads::to_string(report.outcome)}},
                                            {"detail", report.detail}});
        });
    state->scheduler->set_paused(options.downloads_paused);
    state->soft_shutdown = config.shutdown.soft;
    std::vector<application::tasks::DestinationRule> rules;
    for (const auto& d : state->destinations) {
        rules.push_back({d.config.id, infrastructure::filesystem::path_to_utf8(d.root), d.config.principals,
                         d.config.accept_new_tasks});
    }
    state->downloads = std::make_shared<application::tasks::DownloadService>(
        tasks, state->registry, state->credentials, std::move(rules),
        [](const application::tasks::DestinationRule& rule) {
            std::error_code ec;
            return std::filesystem::is_directory(infrastructure::filesystem::path_from_utf8(rule.canonical_root), ec);
        },
        [raw = state.get()](const std::string& destination_id, const std::string& relative_path) {
            for (const auto& d : raw->destinations) {
                if (d.config.id == destination_id) {
                    // Inside the bound root only; a link out of it is not the file.
                    auto path = infrastructure::filesystem::resolve_within(d.root, relative_path);
                    std::error_code ec;
                    return path && std::filesystem::is_regular_file(*path, ec);
                }
            }
            return false;
        },
        config.download_limits.max_batch_items, state->scheduler);

    if (config.events.enabled) {
        state->events = std::make_unique<event_server::EventServer>(
            event_server::EventServerOptions{config.events, config.event_stream, config.cors, state->report.instance_id},
            event_server::EventServerServices{
                state->authenticator, state->events_hub, [ids = inst.ids] { return ids->next("req_"); },
                [raw = state.get()](const std::string& request_id, const std::string& method, const std::string& path,
                                    int status, std::chrono::milliseconds elapsed) {
                    raw->log.info("events.request", {{"request_id", request_id},
                                                     {"method", method},
                                                     {"path", path},
                                                     {"status", status},
                                                     {"elapsed_ms", static_cast<std::int64_t>(elapsed.count())}});
                }});
        auto events_port = state->events->bind();
        if (!events_port) {
            return fail(std::move(events_port).error(), "events_listener");
        }
        state->events->start();
        state->report.events_base_url = base_url(config.events, events_port.value(), config.public_urls.events_base);
    }

    api::ControlApiOptions api_options{config.control, config.http, config.cors, config.catalog,
                                       config.download_limits, config.media_limits, {}};
    api_options.info.version = std::string{service_version};
    api_options.info.instance_id = state->report.instance_id;
    // Search (T13), task queries (T14), SSE (T18) and download execution (T17) are served;
    // files and media are declared once delivered.
    api_options.info.features.search = true;
    api_options.info.features.downloads = true;
    api_options.info.features.progress_polling = true;
    api_options.info.features.progress_sse = state->events != nullptr;
    api_options.info.events_base_url = state->report.events_base_url;
    State* raw = state.get();
    api::ControlApiServices services{
        state->authenticator,
        state->credentials,
        state->registry,
        state->catalog,
        state->assets,
        state->downloads,
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
        },
        std::make_shared<SchedulerPolicy>(state->scheduler)};
    state->control = std::make_unique<api::ControlApi>(std::move(api_options), std::move(services));
    state->control_workers = config.control.workers;
    auto port = state->control->bind();
    if (!port) {
        return fail(std::move(port).error(), "control_listener");
    }
    state->control->start();
    state->report.api_base_url = base_url(config.control, port.value(), config.public_urls.api_base);

    state->ready = true;
    // Ready first, then the queue (configuration-and-deployment.md, 启动与退出).
    state->scheduler->start();
    state->log.info("startup.ready", {{"instance_id", state->report.instance_id},
                                      {"api_base_url", state->report.api_base_url},
                                      {"events_base_url", state->report.events_base_url.value_or("")}});
    undo.state = nullptr;
    return std::unique_ptr<ProcessHost>{new ProcessHost(std::move(state))};
}

} // namespace music_service::server
