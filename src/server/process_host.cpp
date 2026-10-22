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
#include "downloads/maintenance.hpp"
#include "downloads/recovery.hpp"
#include "downloads/runner.hpp"
#include "downloads/scheduler.hpp"
#include "application/transport/upstream_client.hpp"
#include "application/media/file_access.hpp"
#include "application/media/media_access.hpp"
#include "application/media/media_tickets.hpp"
#include "event_server/httplib/event_server.hpp"
#include "media_server/httplib/media_server.hpp"
#include "infrastructure/auth/access_tokens.hpp"
#include "infrastructure/filesystem/destination_files.hpp"
#include "infrastructure/filesystem/paths.hpp"
#include "infrastructure/secrets/file_secret_store.hpp"
#include "infrastructure/transport/httplib_transport.hpp"
#ifdef MUSIC_SERVICE_HAS_EXTERNAL_ADAPTERS
#include "providers/external/helper_runner.hpp"
#endif
#include "providers/registry/builtin_bundles.hpp"
#include "providers/registry/provider_registry.hpp"
#include "runtime/admission/admission_ledger.hpp"
#include "runtime/executors/bounded_executor.hpp"
#include "server/instance.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

namespace music_service::server {

namespace {

// How often the serve loop cleans up work directories and expired idempotency records.
constexpr std::chrono::hours maintenance_interval{1};

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
    limits.helper_processes = config.external_adapters.enabled ? config.external_adapters.max_processes : 0;
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
    std::shared_ptr<application::ports::HttpTransport> transport;
    std::shared_ptr<application::transport::UpstreamClient> upstream;
    std::shared_ptr<application::ports::ExternalHelper> helper; // external_adapters.enabled
    std::shared_ptr<runtime::BoundedExecutor> catalog_executor;
    std::shared_ptr<application::catalog::CatalogService> catalog;
    std::shared_ptr<application::catalog::AssetService> assets;
    std::shared_ptr<application::events::TaskEventHub> events_hub;
    std::shared_ptr<application::tasks::DownloadService> downloads;
    std::shared_ptr<runtime::BoundedExecutor> processing_executor;
    std::shared_ptr<runtime::BoundedExecutor> download_executor;
    std::shared_ptr<downloads::DownloadScheduler> scheduler;
    std::shared_ptr<downloads::Maintenance> maintenance;
    std::chrono::steady_clock::time_point next_maintenance;
    std::unique_ptr<event_server::EventServer> events;
    std::shared_ptr<application::media::FileAccessService> file_access;
    std::shared_ptr<application::media::MediaTicketService> tickets;
    std::shared_ptr<application::media::MediaAccessService> media_access;
    std::unique_ptr<media_server::MediaServer> media;
    std::shared_ptr<runtime::BoundedExecutor> media_upstream_executor; // proxy reads (media_proxy)
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
        if (media) {
            s.media_listening = true;
            s.media_responses = media->active();
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

    // Work directory and idempotency cleanup; problems are logged and retried next time.
    void maintain() {
        next_maintenance = std::chrono::steady_clock::now() + maintenance_interval;
        auto done = maintenance->run(std::chrono::system_clock::now());
        if (!done) {
            log.warn("maintenance.failed", {{"code", std::string{domain::to_string(done.error().code)}},
                                            {"message", done.error().message}});
            return;
        }
        for (const auto& w : done->workspaces) {
            if (w.verdict != downloads::WorkspaceVerdict::kept_active &&
                w.verdict != downloads::WorkspaceVerdict::kept_retained) {
                log.info("maintenance.workspace_removed", {{"destination_id", w.destination_id},
                                                           {"task_id", w.task_id},
                                                           {"reason", std::string{downloads::to_string(w.verdict)}}});
            }
        }
        for (const auto& error : done->errors) {
            log.warn("maintenance.error", {{"message", error}});
        }
        if (done->idempotency_purged > 0) {
            log.info("maintenance.idempotency_purged",
                     {{"records", static_cast<std::uint64_t>(done->idempotency_purged)}});
        }
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
            if (media) {
                media->stop(); // responses in flight end at their next chunk
                media.reset();
            }
            if (media_upstream_executor) {
                // Ending responses cancelled their upstream reads; wait for those to return.
                media_upstream_executor->stop_accepting();
                media_upstream_executor->request_stop();
                media_upstream_executor->join();
                media_upstream_executor.reset();
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
            media_access.reset();
            tickets.reset();
            file_access.reset();
            scheduler.reset();
            download_executor.reset();
            processing_executor.reset();
            events_hub.reset();
            assets.reset();
            catalog.reset();
            catalog_executor.reset();
            registry.reset(); // capability objects hold the upstream client and the helper runner
            helper.reset();
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
        if (state_ && state_->maintenance && std::chrono::steady_clock::now() >= state_->next_maintenance) {
            state_->maintain();
        }
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

    // Leftovers of finished, failed or unknown tasks go now, before new work starts; the
    // serve loop repeats this every maintenance_interval.
    std::vector<std::string> destination_ids;
    for (const auto& d : state->destinations) {
        destination_ids.push_back(d.config.id);
    }
    state->maintenance = std::make_shared<downloads::Maintenance>(
        inst.maintenance, state->files, std::move(destination_ids),
        downloads::MaintenanceOptions{config.filesystem.failed_work_retention});
    state->maintain();

    state->admission = std::make_shared<runtime::AdmissionLedger>(admission_limits(config), clock);
    if (options.transport) {
        state->transport = std::move(options.transport);
    } else {
        infrastructure::transport::HttplibTransportOptions transport_options;
        transport_options.connect_timeout = config.upstream.connect_timeout;
        transport_options.max_response_bytes = static_cast<std::size_t>(config.upstream.max_response_bytes);
        transport_options.redirect_max = config.upstream.redirect_max;
        auto transport = infrastructure::transport::HttplibTransport::create(transport_options);
        if (!transport) {
            return fail(std::move(transport).error(), "transport");
        }
        state->transport = std::move(transport).value();
    }
    state->upstream = std::make_shared<application::transport::UpstreamClient>(state->admission, state->transport, clock);

#ifdef MUSIC_SERVICE_HAS_EXTERNAL_ADAPTERS
    if (config.external_adapters.enabled) {
        // Working directories of helpers the last process left behind go first.
        const auto helper_work = inst.layout.cache / "helpers";
        std::error_code ec;
        std::filesystem::remove_all(helper_work, ec);
        state->helper = std::make_shared<providers::external::HelperRunner>(
            providers::external::HelperRunnerOptions{config.external_adapters.adapters, helper_work,
                                                     config.upstream.max_response_bytes},
            state->admission, clock, state->log.with({{"component", "helper"}}));
        state->log.info("startup.external_adapters",
                        {{"adapters", static_cast<std::uint64_t>(config.external_adapters.adapters.size())},
                         {"max_processes", config.external_adapters.max_processes}});
    }
#endif
    auto registry = providers::ProviderRegistry::build(
        options.bundles ? std::move(*options.bundles) : providers::builtin_bundles(), config.providers,
        providers::ProviderServices{state->upstream, state->helper});
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
    runner_options.max_retry_after = admission_limits(config).max_cooldown; // the same cap as cooldowns
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
        [raw = state.get()](const std::string& destination_id, const std::string& relative_path,
                            std::uint64_t size_bytes) {
            for (const auto& d : raw->destinations) {
                if (d.config.id == destination_id) {
                    // Inside the bound root only; a link out of it is not the file. A file
                    // of another size was rewritten or cut and is not the published one.
                    auto path = infrastructure::filesystem::resolve_within(d.root, relative_path);
                    std::error_code ec;
                    if (!path || !std::filesystem::is_regular_file(*path, ec)) {
                        return false;
                    }
                    const auto size = std::filesystem::file_size(*path, ec);
                    return !ec && size == size_bytes;
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

    // Published files and their tickets (T19), and online audio through the proxy (T34).
    state->file_access = std::make_shared<application::media::FileAccessService>(
        inst.publications, state->files, [raw = state.get()](const std::string& principal, const std::string& destination) {
            for (const auto& d : raw->destinations) {
                if (d.config.id == destination) {
                    const auto& allowed = d.config.principals;
                    return std::find(allowed.begin(), allowed.end(), principal) != allowed.end();
                }
            }
            return false;
        });
    const auto per_owner_share = std::max<std::uint32_t>(1, config.handles.media_tickets / 4);
    state->tickets = std::make_shared<application::media::MediaTicketService>(state->codec, clock,
                                                                              config.handles.media_tickets, per_owner_share);
    const bool media_proxy = config.media.enabled && config.features.media_proxy;
    if (media_proxy) {
        state->media_upstream_executor = std::make_shared<runtime::BoundedExecutor>(
            "media-upstream", config.media_upstream.workers, config.media_upstream.workers);
    }
    if (config.media.enabled) {
        state->media = std::make_unique<media_server::MediaServer>(
            media_server::MediaServerOptions{config.media, config.media_limits, config.media_stream, config.cors,
                                             config.features.media_proxy},
            media_server::MediaServerServices{
                state->authenticator, state->file_access, state->tickets, state->assets,
                state->media_upstream_executor,
                [identities = inst.identities](const std::string& owner, const std::string& token_id) {
                    // Revoking the token or disabling the principal ends its tickets too.
                    auto principal = identities->get_principal(owner);
                    if (!principal || !principal->enabled) {
                        return false;
                    }
                    auto tokens = identities->list_tokens(owner);
                    return tokens && std::any_of(tokens->begin(), tokens->end(), [&](const auto& token) {
                               return token.id == token_id && !token.revoked_at;
                           });
                },
                [ids = inst.ids] { return ids->next("req_"); },
                [raw = state.get()](const std::string& request_id, const std::string& method, const std::string& path,
                                    int status, std::chrono::milliseconds elapsed) {
                    raw->log.info("media.request", {{"request_id", request_id},
                                                    {"method", method},
                                                    {"path", path},
                                                    {"status", status},
                                                    {"elapsed_ms", static_cast<std::int64_t>(elapsed.count())}});
                }});
        auto media_port = state->media->bind();
        if (!media_port) {
            return fail(std::move(media_port).error(), "media_listener");
        }
        state->media->start();
        state->report.media_base_url = base_url(config.media, media_port.value(), config.public_urls.media_base);
    }
    const bool file_access = state->media != nullptr && config.features.file_access;
    state->media_access = std::make_shared<application::media::MediaAccessService>(
        application::media::MediaFeatures{file_access, state->media != nullptr && media_proxy,
                                          config.features.direct_media_urls},
        state->report.media_base_url.value_or(""),
        config.lifetimes.media_access, state->file_access, state->tickets, state->assets, clock);

    api::ControlApiOptions api_options{config.control, config.http, config.cors, config.catalog,
                                       config.download_limits, config.media_limits, {}};
    api_options.info.version = std::string{service_version};
    api_options.info.instance_id = state->report.instance_id;
    // Search (T13), task queries (T14), SSE (T18), download execution (T17) and published
    // files (T19) are served; the online media proxy and direct URLs are declared once delivered.
    api_options.info.features.search = true;
    api_options.info.features.file_access = file_access;
    api_options.info.features.media_proxy = state->media != nullptr && media_proxy;
    api_options.info.features.direct_media_urls = config.features.direct_media_urls;
    api_options.info.media_base_url = state->report.media_base_url;
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
        std::make_shared<SchedulerPolicy>(state->scheduler),
        state->file_access,
        state->media_access};
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
                                      {"events_base_url", state->report.events_base_url.value_or("")},
                                      {"media_base_url", state->report.media_base_url.value_or("")}});
    undo.state = nullptr;
    return std::unique_ptr<ProcessHost>{new ProcessHost(std::move(state))};
}

} // namespace music_service::server
