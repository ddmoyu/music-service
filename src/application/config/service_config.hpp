#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

// Effective service configuration after defaults, file, environment and command line.
// Default values mirror the configuration baseline in
// docs/design/cpp20-refactor/backend-service/configuration-and-deployment.md;
// JSON key names are listed in config/README.md.
// Plain data: no JSON or filesystem access happens here.
namespace music_service::application::config {

using std::chrono::hours;
using std::chrono::milliseconds;
using std::chrono::seconds;

constexpr std::uint64_t kib = 1024;
constexpr std::uint64_t mib = 1024 * kib;
constexpr std::uint64_t gib = 1024 * mib;

struct ListenerConfig {
    bool enabled{true};
    std::string host{"127.0.0.1"};
    std::uint16_t port{0};
    std::uint32_t workers{0};
    std::uint32_t connection_queue{0};
};

struct EventLimits {
    std::uint32_t global_subscriptions{14};
    std::uint32_t per_principal_subscriptions{4};
    std::uint32_t max_queued_events{128};
    std::uint64_t max_queued_bytes{256 * kib};
    std::uint64_t max_event_bytes{16 * kib};
};

struct EventStream {
    milliseconds progress_interval{1000};
    seconds heartbeat{15};
    seconds write_idle{10};
    seconds max_connection{1800};
};

struct PublicUrls {
    std::optional<std::string> api_base;
    std::optional<std::string> media_base;
    std::optional<std::string> events_base;
};

struct Security {
    bool allow_insecure_remote{false};
};

struct HttpLimits {
    std::uint64_t body_limit_bytes{256 * kib};
    seconds read_idle{10};
    seconds write_idle{10};
    seconds keep_alive{3};
    std::uint32_t max_requests_per_connection{50};
};

struct Cors {
    std::vector<std::string> allowed_origins;
};

struct Auth {
    std::string mode{"bearer"};
};

struct Admission {
    std::uint32_t global{8};
    std::uint32_t per_principal{2};
};

struct Upstream {
    std::uint32_t short_workers{8};
    std::uint32_t queue{64};
    std::uint32_t per_source_short{2};
    std::uint32_t per_principal{8}; // concurrent upstream operations of one principal
    std::uint32_t per_origin{4};    // concurrent operations against one scheme://host:port
    std::uint32_t max_total_connections{16};
    std::uint64_t max_response_bytes{2 * mib};
    std::uint32_t redirect_max{5};
    seconds connect_timeout{3};
};

struct Catalog {
    milliseconds total_budget{10000};
    milliseconds max_budget{15000};
    milliseconds per_source_budget{5000};
    std::uint32_t fanout{4};
    std::uint32_t max_sources_per_request{8};
    std::vector<std::string> default_sources;
};

struct ProviderConfig {
    bool enabled{true};
    std::string implementation{"native"};
    std::string options_json{"{}"}; // validated by the provider bundle at registration
    std::optional<std::string> app_profile_id;
    std::optional<std::string> rate_limit_group;
};

struct Downloads {
    std::uint32_t workers{3};
    std::uint32_t per_source{2};
    std::uint32_t max_attempts{3};
    std::uint32_t transport_retries{2};
    std::uint64_t max_file_bytes{2 * gib};
};

struct Retry {
    milliseconds initial_backoff{1000};
    std::uint32_t multiplier{2};
};

struct DownloadLimits {
    std::uint32_t per_principal_active{500};
    std::uint32_t global_active{2000};
    std::uint32_t max_batch_items{100};
};

struct DownloadTimeouts {
    seconds connect{10};
    seconds read_idle{30};
    seconds attempt{600};
    seconds absolute{1800};
};

struct Idempotency {
    hours retention{24};
};

struct Processing {
    std::uint32_t workers{2};
    std::uint32_t queue{16};
};

struct MediaLimits {
    std::uint32_t global{10};
    std::uint32_t per_principal{4};
    std::uint32_t upstream{6};
    std::uint32_t local_files{8};
};

struct MediaUpstream {
    std::uint32_t workers{6};
};

struct MediaStream {
    std::uint64_t bridge_buffer_bytes{256 * kib};
    seconds connect_timeout{5};
    seconds idle{30};
    seconds max_stream{1800};
};

struct Lifetimes {
    seconds asset{300};
    seconds cursor{600};
    seconds match{300};
    seconds media_access{60};
};

struct Storage {
    std::uint32_t writer_queue{128};
    std::uint32_t read_connections{2};
    milliseconds wait_budget{500};
};

struct Cache {
    std::uint32_t catalog_entries{128};
    std::uint64_t catalog_bytes{16 * mib};
    seconds catalog_ttl{60};
    seconds failure_suppress{5};
    std::uint32_t app_token_entries{128};
};

struct Handles {
    std::uint32_t contexts{10000};
    std::uint32_t cursors{5000};
    std::uint32_t assets{1000};
    std::uint32_t matches{1000};
    std::uint32_t media_tickets{2000};
    std::uint64_t total_bytes{32 * mib};
};

struct Shutdown {
    seconds soft{15};
};

struct Filesystem {
    hours failed_work_retention{24};
};

struct Features {
    bool file_access{false};
    bool media_proxy{false};
    bool direct_media_urls{false};
    bool automatic_fallback{false};
    bool transcoding{false};
};

struct Logging {
    std::string format{"json"}; // json | text
    std::string level{"info"};  // trace | debug | info | warn | error
};

struct DestinationConfig {
    std::string id;
    std::string name;
    std::filesystem::path root; // absolute after loading
    std::vector<std::string> principals;
    bool accept_new_tasks{true};
};

struct ServiceConfig {
    std::uint32_t config_version{1};
    std::filesystem::path data_dir; // absolute after loading
    std::optional<std::filesystem::path> config_file;

    ListenerConfig control{true, "127.0.0.1", 17632, 16, 64};
    ListenerConfig media{false, "127.0.0.1", 17633, 12, 16};
    ListenerConfig events{false, "127.0.0.1", 17634, 16, 16};
    EventLimits event_limits;
    EventStream event_stream;
    PublicUrls public_urls;
    Security security;
    HttpLimits http;
    Cors cors;
    Auth auth;
    Admission admission;
    Upstream upstream;
    Catalog catalog;
    std::map<std::string, ProviderConfig> providers;
    Downloads downloads;
    Retry retry;
    DownloadLimits download_limits;
    DownloadTimeouts download_timeouts;
    Idempotency idempotency;
    Processing processing;
    MediaLimits media_limits;
    MediaUpstream media_upstream;
    MediaStream media_stream;
    Lifetimes lifetimes;
    Storage storage;
    Cache cache;
    Handles handles;
    Shutdown shutdown;
    Filesystem filesystem;
    Features features;
    Logging logging;
    std::vector<DestinationConfig> destinations;
};

} // namespace music_service::application::config
