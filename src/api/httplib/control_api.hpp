#pragma once

#include "application/accounts/credential_service.hpp"
#include "application/accounts/identity_admin.hpp"
#include "application/catalog/asset_service.hpp"
#include "application/catalog/catalog_service.hpp"
#include "application/config/service_config.hpp"
#include "application/media/media_access.hpp"
#include "application/tasks/download_service.hpp"
#include "application/ports/providers/directory.hpp"
#include "domain/result.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace music_service::api {

// What GET /api/v1/info declares. Features reflect the build and the configuration.
struct ServiceInfo {
    std::string version;
    std::string instance_id;
    std::optional<std::string> media_base_url;
    std::optional<std::string> events_base_url;
    struct Features {
        bool search{false};
        bool downloads{false};
        bool file_access{false};
        bool media_proxy{false};
        bool direct_media_urls{false};
        bool automatic_fallback{false};
        bool progress_polling{false};
        bool progress_sse{false};
    } features;
};

// A destination as a caller may see it: never the directory itself.
struct DestinationView {
    std::string id;
    std::string name;
    bool available{false};
    bool accept_new_tasks{true};
};

// GET /api/v1/admin/status: process state without secrets.
struct AdminStatus {
    std::string state; // starting | ready | stopping
    std::string instance_id;
    std::string version;
    std::chrono::system_clock::time_point started_at;
    bool storage_available{false};
    std::size_t providers_registered{0};
    std::size_t destinations{0};
    std::uint32_t control_workers{0};
    std::uint32_t upstream_connections_in_use{0};
    std::size_t upstream_cooldowns{0};
    std::uint32_t download_workers{0};
    std::uint32_t downloads_running{0};
    bool downloads_paused{false};
    std::uint64_t downloads_claimed{0}; // since start
    bool media_listening{false};
    std::uint32_t media_responses{0}; // file responses holding a slot
};

// admin/download-policy: whether the scheduler claims new tasks.
class DownloadPolicy {
public:
    virtual ~DownloadPolicy() = default;
    [[nodiscard]] virtual bool paused() const = 0;
    virtual void set_paused(bool paused) = 0;
};

struct ControlApiOptions {
    application::config::ListenerConfig listener;
    application::config::HttpLimits http;
    application::config::Cors cors;
    application::config::Catalog catalog;
    application::config::DownloadLimits download_limits;
    application::config::MediaLimits media_limits;
    ServiceInfo info;
};

struct ControlApiServices {
    std::shared_ptr<const application::accounts::AccessTokenAuthenticator> authenticator;
    std::shared_ptr<application::accounts::CredentialService> credentials;
    std::shared_ptr<const application::ports::ProviderDirectory> providers;
    std::shared_ptr<application::catalog::CatalogService> catalog;
    std::shared_ptr<application::catalog::AssetService> assets;
    std::shared_ptr<application::tasks::DownloadService> downloads;
    // Destinations the principal may use, with current availability.
    std::function<std::vector<DestinationView>(const std::string& principal_id)> destinations;
    std::function<bool()> ready;
    std::function<AdminStatus()> status;
    std::function<std::string()> new_request_id; // "req_..." ids
    // Called once per finished request; must not throw. Carries no secrets.
    std::function<void(const std::string& request_id, const std::string& method, const std::string& path,
                       int status, std::chrono::milliseconds elapsed)>
        on_request;
    // Empty when downloads are not executed: the policy routes are not mounted.
    std::shared_ptr<DownloadPolicy> download_policy{};
    // File metadata and media-access (T19); empty or with file_access off they answer 501.
    std::shared_ptr<application::media::FileAccessService> files{};
    std::shared_ptr<application::media::MediaAccessService> media{};
};

// The control HTTP entry (/health, /api/v1) on cpp-httplib: Bearer authentication with
// scopes, the JSON envelope, request ids, CORS, body and content-type limits, and the
// system, provider, destination, credential, default-selection, search, track
// detail, link parsing, asset resolve/probe, download task and batch, file metadata,
// media-access, admin/status and admin/download-policy routes. Routes of later
// tasks are not mounted, so they answer 404 instead of a fake success. Owns its listener and worker pool; no GUI or desktop dependency.
class ControlApi {
public:
    ControlApi(ControlApiOptions options, ControlApiServices services);
    ~ControlApi();
    ControlApi(const ControlApi&) = delete;
    ControlApi& operator=(const ControlApi&) = delete;

    // Binds host:port (port 0 picks a free one) and returns the bound port. A port in
    // use is an error naming the listener and address.
    domain::Result<std::uint16_t> bind();
    // Serves on a background thread until stop().
    void start();
    // Stops accepting, finishes requests in flight and joins. Idempotent.
    void stop();

    [[nodiscard]] std::uint16_t port() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace music_service::api
