#include "infrastructure/config/config_loader.hpp"

#include "infrastructure/config/address.hpp"

#include "domain/catalog/refs.hpp"

#include <algorithm>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace music_service::infrastructure::config {

namespace cfg = application::config;

namespace {

class Problems {
public:
    void add(std::string field, std::string reason) {
        details_.push_back({std::move(field), std::move(reason)});
    }
    [[nodiscard]] bool empty() const noexcept { return details_.empty(); }
    std::vector<domain::ErrorDetail> take() { return std::move(details_); }

private:
    std::vector<domain::ErrorDetail> details_;
};

void check_listeners(const cfg::ServiceConfig& c, const BuildCapabilities& build, Problems& p) {
    struct Named {
        const char* name;
        const cfg::ListenerConfig* listener;
        const std::optional<std::string>* public_url;
    };
    const Named listeners[] = {{"control", &c.control, &c.public_urls.api_base},
                               {"media", &c.media, &c.public_urls.media_base},
                               {"events", &c.events, &c.public_urls.events_base}};

    if (c.media.enabled && !build.media_server) {
        p.add("/media/enabled", "this build has no media server");
    }
    if (c.events.enabled && !build.event_server) {
        p.add("/events/enabled", "this build has no event server");
    }

    for (const auto& [name, listener, public_url] : listeners) {
        if (!listener->enabled) {
            continue;
        }
        const std::string base = std::string{"/"} + name;
        const bool ipv6 = listener->host.find(':') != std::string::npos;
        if (!detail::parse_listen_address(ipv6 ? "[" + listener->host + "]:0" : listener->host + ":0")) {
            p.add(base + "/host", "is not a valid host name or address");
        }
        // Port 0 lets the system choose; only a local desktop launcher may use it.
        if (listener->port == 0 && !detail::is_loopback_host(listener->host)) {
            p.add(base + "/port", "port 0 is only allowed on a loopback host");
        }
        if (!detail::is_loopback_host(listener->host)) {
            if (!*public_url) {
                p.add(base + "/host",
                      "listening beyond loopback requires the matching public_urls entry");
            }
        }
    }

    for (std::size_t i = 0; i < std::size(listeners); ++i) {
        for (std::size_t j = i + 1; j < std::size(listeners); ++j) {
            const auto& a = *listeners[i].listener;
            const auto& b = *listeners[j].listener;
            if (a.enabled && b.enabled && a.port != 0 && a.port == b.port &&
                (a.host == b.host || a.host == "0.0.0.0" || b.host == "0.0.0.0" ||
                 a.host == "::" || b.host == "::")) {
                p.add(std::string{"/"} + listeners[j].name + "/port",
                      std::string{"conflicts with "} + listeners[i].name);
            }
        }
    }
}

void check_public_urls(const cfg::ServiceConfig& c, Problems& p) {
    const std::pair<const char*, const std::optional<std::string>*> urls[] = {
        {"/public_urls/api_base", &c.public_urls.api_base},
        {"/public_urls/media_base", &c.public_urls.media_base},
        {"/public_urls/events_base", &c.public_urls.events_base},
    };
    for (const auto& [field, value] : urls) {
        if (!*value) {
            continue;
        }
        const auto url = detail::parse_base_url(**value);
        if (!url) {
            p.add(field, "must be an absolute http(s) URL without credentials, query or fragment");
            continue;
        }
        // Bearer tokens and media tickets would cross the network in clear text.
        if (url->scheme == "http" && !detail::is_loopback_host(url->host) &&
            !c.security.allow_insecure_remote) {
            p.add(field, "remote access must use https (or set security.allow_insecure_remote)");
        }
    }
}

void check_capacity(const cfg::ServiceConfig& c, Problems& p) {
    if (c.control.workers < 2) {
        p.add("/control/workers", "must be at least 2 to reserve control capacity");
    }
    if (c.events.enabled && c.event_limits.global_subscriptions + 2 > c.events.workers) {
        p.add("/event_limits/global_subscriptions", "must be at least 2 below events.workers");
    }
    if (c.event_limits.per_principal_subscriptions > c.event_limits.global_subscriptions) {
        p.add("/event_limits/per_principal_subscriptions", "must not exceed the global limit");
    }
    if (c.event_limits.max_event_bytes > c.event_limits.max_queued_bytes) {
        p.add("/event_limits/max_event_bytes", "must not exceed max_queued_bytes");
    }
    if (c.media.enabled && c.media_limits.global + 2 > c.media.workers) {
        p.add("/media_limits/global", "must be at least 2 below media.workers");
    }
    if (c.media_limits.per_principal > c.media_limits.global) {
        p.add("/media_limits/per_principal", "must not exceed the global limit");
    }
    if (c.media_limits.upstream > c.media_limits.global) {
        p.add("/media_limits/upstream", "must not exceed the global limit");
    }
    if (c.media_limits.local_files > c.media_limits.global) {
        p.add("/media_limits/local_files", "must not exceed the global limit");
    }
    if (c.features.media_proxy && c.media_upstream.workers < c.media_limits.upstream) {
        p.add("/media_upstream/workers", "must cover media_limits.upstream");
    }
    if (c.admission.per_principal > c.admission.global) {
        p.add("/admission/per_principal", "must not exceed the global limit");
    }
    if (c.upstream.per_source_short > c.upstream.max_total_connections) {
        p.add("/upstream/per_source_short", "must not exceed max_total_connections");
    }
    if (c.upstream.per_origin > c.upstream.max_total_connections) {
        p.add("/upstream/per_origin", "must not exceed max_total_connections");
    }
    if (c.upstream.per_principal > c.upstream.max_total_connections) {
        p.add("/upstream/per_principal", "must not exceed max_total_connections");
    }
    if (c.downloads.per_source > c.upstream.max_total_connections) {
        p.add("/downloads/per_source", "must not exceed upstream.max_total_connections");
    }
    if (c.download_limits.per_principal_active > c.download_limits.global_active) {
        p.add("/download_limits/per_principal_active", "must not exceed global_active");
    }
    if (c.download_timeouts.attempt > c.download_timeouts.absolute) {
        p.add("/download_timeouts/attempt_seconds", "must not exceed absolute_seconds");
    }
    if (c.catalog.per_source_budget > c.catalog.total_budget) {
        p.add("/catalog/per_source_budget_ms", "must not exceed total_budget_ms");
    }
    if (c.catalog.total_budget > c.catalog.max_budget) {
        p.add("/catalog/total_budget_ms", "must not exceed max_budget_ms");
    }
    if (c.catalog.fanout > c.upstream.short_workers) {
        p.add("/catalog/fanout", "must not exceed upstream.short_workers");
    }
}

void check_features(const cfg::ServiceConfig& c, const BuildCapabilities& build, Problems& p) {
    const auto& f = c.features;
    if ((f.file_access || f.media_proxy) && !c.media.enabled) {
        p.add("/features", "file_access and media_proxy require media.enabled");
    }
    if (f.media_proxy && !build.media_proxy) {
        p.add("/features/media_proxy", "is not supported by this build");
    }
    if (f.direct_media_urls && !build.direct_media_urls) {
        p.add("/features/direct_media_urls", "is not supported by this build");
    }
    if (f.transcoding && !build.transcoding) {
        p.add("/features/transcoding", "is not supported by this build");
    }
    if (c.auth.mode != "bearer") {
        p.add("/auth/mode", "must be bearer");
    }
    if (c.logging.format != "json" && c.logging.format != "text") {
        p.add("/logging/format", "must be json or text");
    }
    if (c.logging.max_file_bytes < 64 * cfg::kib || c.logging.max_file_bytes > cfg::gib)
        p.add("/logging/max_file_bytes", "must be between 65536 and 1073741824");
    if (c.logging.retained_files > 20) p.add("/logging/retained_files", "must be between 0 and 20");
    static const std::set<std::string> levels{"trace", "debug", "info", "warn", "error"};
    if (levels.find(c.logging.level) == levels.end()) {
        p.add("/logging/level", "must be trace, debug, info, warn or error");
    }
}

void check_providers(const cfg::ServiceConfig& c, const BuildCapabilities& build, Problems& p) {
    for (const auto& [source, provider] : c.providers) {
        const auto field = "/providers/" + source;
        if (build.providers.find(source) == build.providers.end()) {
            p.add(field, "is not included in this build");
        }
        // Every platform is parsed by the service's own code; no external tools.
        if (provider.implementation != "native") {
            p.add(field + "/implementation", "must be native");
        }
        if (provider.app_profile_id && !build.app_profiles) {
            p.add(field + "/app_profile_id", "application profiles are not supported yet");
        }
    }

    const auto& defaults = c.catalog.default_sources;
    if (defaults.size() > c.catalog.max_sources_per_request) {
        p.add("/catalog/default_sources", "exceeds max_sources_per_request");
    }
    std::set<std::string> seen;
    for (const auto& source : defaults) {
        if (!seen.insert(source).second) {
            p.add("/catalog/default_sources", "lists " + source + " twice");
        }
        const auto it = c.providers.find(source);
        if (it == c.providers.end() || !it->second.enabled) {
            p.add("/catalog/default_sources", source + " is not a configured, enabled provider");
        }
    }
}

void check_paths(const cfg::ServiceConfig& c, Problems& p) {
    if (c.data_dir.empty() || !c.data_dir.is_absolute()) {
        p.add("/data_dir", "must resolve to an absolute path");
    }

    std::set<std::string> ids;
    for (std::size_t i = 0; i < c.destinations.size(); ++i) {
        const auto& d = c.destinations[i];
        const auto field = "/destinations/" + std::to_string(i);
        if (!domain::is_valid_service_id("dst_", d.id)) {
            p.add(field + "/id", "must be a dst_ service id");
        } else if (!ids.insert(d.id).second) {
            p.add(field + "/id", "duplicates another destination");
        }
        if (d.root.empty() || !d.root.is_absolute()) {
            p.add(field + "/root", "must resolve to an absolute path");
        } else if (!c.data_dir.empty() && d.root == c.data_dir) {
            p.add(field + "/root", "must differ from data_dir");
        }
        for (const auto& principal : d.principals) {
            if (!domain::is_valid_service_id("prn_", principal)) {
                p.add(field + "/principals", principal + " is not a prn_ service id");
            }
        }
    }
}

} // namespace

domain::Result<void> validate_service_config(const cfg::ServiceConfig& config,
                                             const BuildCapabilities& build) {
    Problems problems;
    check_listeners(config, build, problems);
    check_public_urls(config, problems);
    check_capacity(config, problems);
    check_features(config, build, problems);
    check_providers(config, build, problems);
    check_paths(config, problems);
    if (auto checked = domain::validate_output_options(config.downloads.output); !checked) {
        for (const auto& detail : checked.error().details) {
            auto field = detail.field;
            std::replace(field.begin(), field.end(), '.', '/');
            problems.add("/downloads/" + field, detail.reason);
        }
    }
    if (problems.empty()) {
        return domain::ok();
    }
    return domain::Error{domain::ErrorCode::validation_failed, "configuration is invalid", false,
                         problems.take()};
}

} // namespace music_service::infrastructure::config
