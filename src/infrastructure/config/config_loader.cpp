#include "infrastructure/config/config_loader.hpp"

#include "infrastructure/config/address.hpp"
#include "infrastructure/filesystem/paths.hpp"

#include "domain/catalog/refs.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <sstream>
#include <utility>

namespace music_service::infrastructure::config {

namespace cfg = application::config;
using json = nlohmann::json;
using filesystem::path_from_utf8;

namespace {

constexpr std::uint64_t u32_max = std::numeric_limits<std::uint32_t>::max();
// Sizes stay within the exact JSON integer range used by the API.
constexpr std::uint64_t json_safe_max = 9007199254740991ULL;

std::filesystem::path resolve(const std::filesystem::path& base, const std::filesystem::path& p) {
    return (p.is_absolute() ? p : base / p).lexically_normal();
}

using FieldHandler = std::function<void(const json& value, const std::string& path)>;
using FieldTable = std::initializer_list<std::pair<std::string_view, FieldHandler>>;

class Reader {
public:
    std::vector<domain::ErrorDetail> errors;

    void fail(std::string path, std::string reason) {
        errors.push_back({std::move(path), std::move(reason)});
    }

    void object(const json& node, const std::string& path, FieldTable fields) {
        if (!node.is_object()) {
            fail(path, "must be an object");
            return;
        }
        for (const auto& [key, value] : node.items()) {
            const auto child = path + "/" + key;
            bool known = false;
            for (const auto& [name, handler] : fields) {
                if (name == key) {
                    handler(value, child);
                    known = true;
                    break;
                }
            }
            if (!known) {
                fail(child, "unknown key");
            }
        }
    }

    template <class T>
    void number(const json& value, const std::string& path, T& out, std::uint64_t min,
                std::uint64_t max) {
        // nlohmann stores every non-negative integer literal as number_unsigned.
        if (!value.is_number_unsigned()) {
            fail(path, "must be a non-negative integer");
            return;
        }
        const auto n = value.get<std::uint64_t>();
        if (n < min || n > max) {
            fail(path, "must be between " + std::to_string(min) + " and " + std::to_string(max));
            return;
        }
        out = static_cast<T>(n);
    }

    template <class Rep, class Period>
    void duration(const json& value, const std::string& path,
                  std::chrono::duration<Rep, Period>& out, std::uint64_t min, std::uint64_t max) {
        std::uint64_t count = 0;
        const auto before = errors.size();
        number(value, path, count, min, max);
        if (errors.size() == before) {
            out = std::chrono::duration<Rep, Period>{static_cast<Rep>(count)};
        }
    }

    void boolean(const json& value, const std::string& path, bool& out) {
        if (!value.is_boolean()) {
            fail(path, "must be true or false");
            return;
        }
        out = value.get<bool>();
    }

    void string(const json& value, const std::string& path, std::string& out) {
        if (!value.is_string() || value.get_ref<const std::string&>().empty()) {
            fail(path, "must be a non-empty string");
            return;
        }
        out = value.get<std::string>();
    }

    void optional_string(const json& value, const std::string& path,
                         std::optional<std::string>& out) {
        if (value.is_null()) {
            out.reset();
            return;
        }
        std::string text;
        const auto before = errors.size();
        string(value, path, text);
        if (errors.size() == before) {
            out = std::move(text);
        }
    }

    void string_list(const json& value, const std::string& path, std::vector<std::string>& out) {
        if (!value.is_array()) {
            fail(path, "must be an array of strings");
            return;
        }
        out.clear();
        for (std::size_t i = 0; i < value.size(); ++i) {
            std::string item;
            const auto before = errors.size();
            string(value[i], path + "/" + std::to_string(i), item);
            if (errors.size() == before) {
                out.push_back(std::move(item));
            }
        }
    }

    void path(const json& value, const std::string& field_path, const std::filesystem::path& base,
              std::filesystem::path& out) {
        std::string text;
        const auto before = errors.size();
        string(value, field_path, text);
        if (errors.size() == before) {
            out = resolve(base, path_from_utf8(text));
        }
    }
};

void read_listener(Reader& r, const json& node, const std::string& path, cfg::ListenerConfig& l,
                   bool has_enabled) {
    r.object(node, path,
             {
                 {"enabled",
                  [&](const json& v, const std::string& p) {
                      if (has_enabled) {
                          r.boolean(v, p, l.enabled);
                      } else {
                          r.fail(p, "unknown key");
                      }
                  }},
                 {"host", [&](const json& v, const std::string& p) { r.string(v, p, l.host); }},
                 {"port", [&](const json& v, const std::string& p) { r.number(v, p, l.port, 0, 65535); }},
                 {"workers", [&](const json& v, const std::string& p) { r.number(v, p, l.workers, 1, 1024); }},
                 {"connection_queue",
                  [&](const json& v, const std::string& p) { r.number(v, p, l.connection_queue, 1, 65535); }},
             });
}

void read_providers(Reader& r, const json& node, const std::string& path,
                    std::map<std::string, cfg::ProviderConfig>& providers) {
    if (!node.is_object()) {
        r.fail(path, "must be an object keyed by source");
        return;
    }
    for (const auto& [source, value] : node.items()) {
        const auto child = path + "/" + source;
        if (!domain::SourceId::parse(source)) {
            r.fail(child, "source must match [a-z][a-z0-9_]{0,63}");
            continue;
        }
        auto& provider = providers[source];
        r.object(value, child,
                 {
                     {"enabled", [&](const json& v, const std::string& p) { r.boolean(v, p, provider.enabled); }},
                     {"implementation",
                      [&](const json& v, const std::string& p) { r.string(v, p, provider.implementation); }},
                     {"options",
                      [&](const json& v, const std::string& p) {
                          if (!v.is_object()) {
                              r.fail(p, "must be an object");
                          } else {
                              provider.options_json = v.dump();
                          }
                      }},
                     {"app_profile_id",
                      [&](const json& v, const std::string& p) { r.optional_string(v, p, provider.app_profile_id); }},
                     {"rate_limit_group",
                      [&](const json& v, const std::string& p) { r.optional_string(v, p, provider.rate_limit_group); }},
                 });
    }
}

void read_adapters(Reader& r, const json& node, const std::string& path, const std::filesystem::path& base,
                   std::map<std::string, cfg::ExternalAdapterConfig>& adapters) {
    if (!node.is_object()) {
        r.fail(path, "must be an object keyed by adapter id");
        return;
    }
    for (const auto& [id, value] : node.items()) {
        const auto child = path + "/" + id;
        auto& a = adapters[id];
        bool has_source = false;
        bool has_program = false;
        bool has_version = false;
        r.object(value, child,
                 {
                     {"source",
                      [&](const json& v, const std::string& p) {
                          r.string(v, p, a.source);
                          has_source = true;
                      }},
                     {"program",
                      [&](const json& v, const std::string& p) {
                          r.path(v, p, base, a.program);
                          has_program = true;
                      }},
                     {"args",
                      [&](const json& v, const std::string& p) {
                          // Arguments may be empty strings; only the type is checked here.
                          if (!v.is_array() ||
                              !std::all_of(v.begin(), v.end(), [](const json& x) { return x.is_string(); })) {
                              r.fail(p, "must be an array of strings");
                          } else {
                              a.args = v.get<std::vector<std::string>>();
                          }
                      }},
                     {"tool_version",
                      [&](const json& v, const std::string& p) {
                          r.string(v, p, a.tool_version);
                          has_version = true;
                      }},
                     {"operations", [&](const json& v, const std::string& p) { r.string_list(v, p, a.operations); }},
                     {"extractors", [&](const json& v, const std::string& p) { r.string_list(v, p, a.extractors); }},
                     {"domains", [&](const json& v, const std::string& p) { r.string_list(v, p, a.domains); }},
                     {"max_connections", [&](const json& v, const std::string& p) { r.number(v, p, a.max_connections, 1, 64); }},
                     {"startup_timeout_ms", [&](const json& v, const std::string& p) { r.duration(v, p, a.startup_timeout, 100, 300000); }},
                     {"operation_timeout_ms", [&](const json& v, const std::string& p) { r.duration(v, p, a.operation_timeout, 100, 3600000); }},
                     {"stop_grace_ms", [&](const json& v, const std::string& p) { r.duration(v, p, a.stop_grace, 0, 60000); }},
                     {"max_stderr_bytes", [&](const json& v, const std::string& p) { r.number(v, p, a.max_stderr_bytes, 0, 1 * cfg::mib); }},
                 });
        if (value.is_object()) {
            if (!has_source) {
                r.fail(child + "/source", "is required");
            }
            if (!has_program) {
                r.fail(child + "/program", "is required");
            }
            if (!has_version) {
                r.fail(child + "/tool_version", "is required");
            }
        }
    }
}

void read_destinations(Reader& r, const json& node, const std::string& path,
                       const std::filesystem::path& base,
                       std::vector<cfg::DestinationConfig>& destinations) {
    if (!node.is_array()) {
        r.fail(path, "must be an array");
        return;
    }
    destinations.clear();
    for (std::size_t i = 0; i < node.size(); ++i) {
        const auto child = path + "/" + std::to_string(i);
        cfg::DestinationConfig destination;
        bool has_id = false;
        bool has_root = false;
        r.object(node[i], child,
                 {
                     {"id",
                      [&](const json& v, const std::string& p) {
                          r.string(v, p, destination.id);
                          has_id = true;
                      }},
                     {"name", [&](const json& v, const std::string& p) { r.string(v, p, destination.name); }},
                     {"root",
                      [&](const json& v, const std::string& p) {
                          r.path(v, p, base, destination.root);
                          has_root = true;
                      }},
                     {"principals",
                      [&](const json& v, const std::string& p) { r.string_list(v, p, destination.principals); }},
                     {"accept_new_tasks",
                      [&](const json& v, const std::string& p) { r.boolean(v, p, destination.accept_new_tasks); }},
                 });
        if (node[i].is_object()) {
            if (!has_id) {
                r.fail(child + "/id", "is required");
            }
            if (!has_root) {
                r.fail(child + "/root", "is required");
            }
            if (destination.name.empty()) {
                destination.name = destination.id;
            }
        }
        destinations.push_back(std::move(destination));
    }
}

void read_document(Reader& r, const json& doc, const std::filesystem::path& base,
                   cfg::ServiceConfig& c) {
    r.object(doc, "",
             {
                 {"config_version", [&](const json& v, const std::string& p) { r.number(v, p, c.config_version, 1, 1); }},
                 {"data_dir", [&](const json& v, const std::string& p) { r.path(v, p, base, c.data_dir); }},
                 {"control", [&](const json& v, const std::string& p) { read_listener(r, v, p, c.control, false); }},
                 {"media", [&](const json& v, const std::string& p) { read_listener(r, v, p, c.media, true); }},
                 {"events", [&](const json& v, const std::string& p) { read_listener(r, v, p, c.events, true); }},
                 {"event_limits",
                  [&](const json& v, const std::string& p) {
                      auto& e = c.event_limits;
                      r.object(v, p,
                               {
                                   {"global_subscriptions", [&](const json& x, const std::string& q) { r.number(x, q, e.global_subscriptions, 1, 4096); }},
                                   {"per_principal_subscriptions", [&](const json& x, const std::string& q) { r.number(x, q, e.per_principal_subscriptions, 1, 4096); }},
                                   {"max_queued_events", [&](const json& x, const std::string& q) { r.number(x, q, e.max_queued_events, 1, 65535); }},
                                   {"max_queued_bytes", [&](const json& x, const std::string& q) { r.number(x, q, e.max_queued_bytes, 1024, 64 * cfg::mib); }},
                                   {"max_event_bytes", [&](const json& x, const std::string& q) { r.number(x, q, e.max_event_bytes, 256, 1 * cfg::mib); }},
                               });
                  }},
                 {"event_stream",
                  [&](const json& v, const std::string& p) {
                      auto& e = c.event_stream;
                      r.object(v, p,
                               {
                                   {"progress_interval_ms", [&](const json& x, const std::string& q) { r.duration(x, q, e.progress_interval, 100, 60000); }},
                                   {"heartbeat_seconds", [&](const json& x, const std::string& q) { r.duration(x, q, e.heartbeat, 1, 300); }},
                                   {"write_idle_seconds", [&](const json& x, const std::string& q) { r.duration(x, q, e.write_idle, 1, 300); }},
                                   {"max_connection_seconds", [&](const json& x, const std::string& q) { r.duration(x, q, e.max_connection, 10, 86400); }},
                               });
                  }},
                 {"public_urls",
                  [&](const json& v, const std::string& p) {
                      auto& u = c.public_urls;
                      r.object(v, p,
                               {
                                   {"api_base", [&](const json& x, const std::string& q) { r.optional_string(x, q, u.api_base); }},
                                   {"media_base", [&](const json& x, const std::string& q) { r.optional_string(x, q, u.media_base); }},
                                   {"events_base", [&](const json& x, const std::string& q) { r.optional_string(x, q, u.events_base); }},
                               });
                  }},
                 {"security",
                  [&](const json& v, const std::string& p) {
                      r.object(v, p,
                               {
                                   {"allow_insecure_remote", [&](const json& x, const std::string& q) { r.boolean(x, q, c.security.allow_insecure_remote); }},
                               });
                  }},
                 {"http",
                  [&](const json& v, const std::string& p) {
                      auto& h = c.http;
                      r.object(v, p,
                               {
                                   {"body_limit_bytes", [&](const json& x, const std::string& q) { r.number(x, q, h.body_limit_bytes, 1024, 16 * cfg::mib); }},
                                   {"read_idle_seconds", [&](const json& x, const std::string& q) { r.duration(x, q, h.read_idle, 1, 300); }},
                                   {"write_idle_seconds", [&](const json& x, const std::string& q) { r.duration(x, q, h.write_idle, 1, 300); }},
                                   {"keep_alive_seconds", [&](const json& x, const std::string& q) { r.duration(x, q, h.keep_alive, 0, 300); }},
                                   {"max_requests_per_connection", [&](const json& x, const std::string& q) { r.number(x, q, h.max_requests_per_connection, 1, 100000); }},
                               });
                  }},
                 {"cors",
                  [&](const json& v, const std::string& p) {
                      r.object(v, p,
                               {
                                   {"allowed_origins", [&](const json& x, const std::string& q) { r.string_list(x, q, c.cors.allowed_origins); }},
                               });
                  }},
                 {"auth",
                  [&](const json& v, const std::string& p) {
                      r.object(v, p, {{"mode", [&](const json& x, const std::string& q) { r.string(x, q, c.auth.mode); }}});
                  }},
                 {"admission",
                  [&](const json& v, const std::string& p) {
                      r.object(v, p,
                               {
                                   {"global", [&](const json& x, const std::string& q) { r.number(x, q, c.admission.global, 1, 4096); }},
                                   {"per_principal", [&](const json& x, const std::string& q) { r.number(x, q, c.admission.per_principal, 1, 4096); }},
                               });
                  }},
                 {"upstream",
                  [&](const json& v, const std::string& p) {
                      auto& u = c.upstream;
                      r.object(v, p,
                               {
                                   {"short_workers", [&](const json& x, const std::string& q) { r.number(x, q, u.short_workers, 1, 1024); }},
                                   {"queue", [&](const json& x, const std::string& q) { r.number(x, q, u.queue, 1, 65535); }},
                                   {"per_source_short", [&](const json& x, const std::string& q) { r.number(x, q, u.per_source_short, 1, 1024); }},
                                   {"per_principal", [&](const json& x, const std::string& q) { r.number(x, q, u.per_principal, 1, 4096); }},
                                   {"per_origin", [&](const json& x, const std::string& q) { r.number(x, q, u.per_origin, 1, 4096); }},
                                   {"max_total_connections", [&](const json& x, const std::string& q) { r.number(x, q, u.max_total_connections, 1, 4096); }},
                                   {"max_response_bytes", [&](const json& x, const std::string& q) { r.number(x, q, u.max_response_bytes, 1024, 256 * cfg::mib); }},
                                   {"redirect_max", [&](const json& x, const std::string& q) { r.number(x, q, u.redirect_max, 0, 20); }},
                                   {"connect_timeout_seconds", [&](const json& x, const std::string& q) { r.duration(x, q, u.connect_timeout, 1, 60); }},
                               });
                  }},
                 {"catalog",
                  [&](const json& v, const std::string& p) {
                      auto& k = c.catalog;
                      r.object(v, p,
                               {
                                   {"total_budget_ms", [&](const json& x, const std::string& q) { r.duration(x, q, k.total_budget, 1000, 15000); }},
                                   {"max_budget_ms", [&](const json& x, const std::string& q) { r.duration(x, q, k.max_budget, 1000, 15000); }},
                                   {"per_source_budget_ms", [&](const json& x, const std::string& q) { r.duration(x, q, k.per_source_budget, 100, 15000); }},
                                   {"fanout", [&](const json& x, const std::string& q) { r.number(x, q, k.fanout, 1, 64); }},
                                   {"max_sources_per_request", [&](const json& x, const std::string& q) { r.number(x, q, k.max_sources_per_request, 1, 64); }},
                                   {"default_sources", [&](const json& x, const std::string& q) { r.string_list(x, q, k.default_sources); }},
                               });
                  }},
                 {"providers", [&](const json& v, const std::string& p) { read_providers(r, v, p, c.providers); }},
                 {"app_profiles",
                  [&](const json& v, const std::string& p) {
                      if (!v.is_object()) {
                          r.fail(p, "must be an object");
                      } else if (!v.empty()) {
                          r.fail(p, "application profiles are not supported by this build yet");
                      }
                  }},
                 {"external_adapters",
                  [&](const json& v, const std::string& p) {
                      auto& e = c.external_adapters;
                      r.object(v, p,
                               {
                                   {"enabled", [&](const json& x, const std::string& q) { r.boolean(x, q, e.enabled); }},
                                   {"max_processes", [&](const json& x, const std::string& q) { r.number(x, q, e.max_processes, 1, 64); }},
                                   {"adapters", [&](const json& x, const std::string& q) { read_adapters(r, x, q, base, e.adapters); }},
                               });
                  }},
                 {"downloads",
                  [&](const json& v, const std::string& p) {
                      auto& d = c.downloads;
                      r.object(v, p,
                               {
                                   {"workers", [&](const json& x, const std::string& q) { r.number(x, q, d.workers, 1, 256); }},
                                   {"per_source", [&](const json& x, const std::string& q) { r.number(x, q, d.per_source, 1, 256); }},
                                   {"max_attempts", [&](const json& x, const std::string& q) { r.number(x, q, d.max_attempts, 1, 100); }},
                                   {"transport_retries", [&](const json& x, const std::string& q) { r.number(x, q, d.transport_retries, 0, 100); }},
                                   {"max_file_bytes", [&](const json& x, const std::string& q) { r.number(x, q, d.max_file_bytes, 1, json_safe_max); }},
                               });
                  }},
                 {"retry",
                  [&](const json& v, const std::string& p) {
                      r.object(v, p,
                               {
                                   {"initial_backoff_ms", [&](const json& x, const std::string& q) { r.duration(x, q, c.retry.initial_backoff, 1, 600000); }},
                                   {"multiplier", [&](const json& x, const std::string& q) { r.number(x, q, c.retry.multiplier, 1, 10); }},
                               });
                  }},
                 {"download_limits",
                  [&](const json& v, const std::string& p) {
                      auto& d = c.download_limits;
                      r.object(v, p,
                               {
                                   {"per_principal_active", [&](const json& x, const std::string& q) { r.number(x, q, d.per_principal_active, 1, 1000000); }},
                                   {"global_active", [&](const json& x, const std::string& q) { r.number(x, q, d.global_active, 1, 1000000); }},
                                   {"max_batch_items", [&](const json& x, const std::string& q) { r.number(x, q, d.max_batch_items, 1, 100); }},
                               });
                  }},
                 {"download_timeouts",
                  [&](const json& v, const std::string& p) {
                      auto& d = c.download_timeouts;
                      r.object(v, p,
                               {
                                   {"connect_seconds", [&](const json& x, const std::string& q) { r.duration(x, q, d.connect, 1, 300); }},
                                   {"read_idle_seconds", [&](const json& x, const std::string& q) { r.duration(x, q, d.read_idle, 1, 3600); }},
                                   {"attempt_seconds", [&](const json& x, const std::string& q) { r.duration(x, q, d.attempt, 10, 86400); }},
                                   {"absolute_seconds", [&](const json& x, const std::string& q) { r.duration(x, q, d.absolute, 10, 604800); }},
                               });
                  }},
                 {"idempotency",
                  [&](const json& v, const std::string& p) {
                      r.object(v, p, {{"retention_hours", [&](const json& x, const std::string& q) { r.duration(x, q, c.idempotency.retention, 24, 8760); }}});
                  }},
                 {"processing",
                  [&](const json& v, const std::string& p) {
                      r.object(v, p,
                               {
                                   {"workers", [&](const json& x, const std::string& q) { r.number(x, q, c.processing.workers, 1, 256); }},
                                   {"queue", [&](const json& x, const std::string& q) { r.number(x, q, c.processing.queue, 1, 65535); }},
                               });
                  }},
                 {"media_limits",
                  [&](const json& v, const std::string& p) {
                      auto& m = c.media_limits;
                      r.object(v, p,
                               {
                                   {"global", [&](const json& x, const std::string& q) { r.number(x, q, m.global, 1, 4096); }},
                                   {"per_principal", [&](const json& x, const std::string& q) { r.number(x, q, m.per_principal, 1, 4096); }},
                                   {"upstream", [&](const json& x, const std::string& q) { r.number(x, q, m.upstream, 0, 4096); }},
                                   {"local_files", [&](const json& x, const std::string& q) { r.number(x, q, m.local_files, 0, 4096); }},
                               });
                  }},
                 {"media_upstream",
                  [&](const json& v, const std::string& p) {
                      r.object(v, p, {{"workers", [&](const json& x, const std::string& q) { r.number(x, q, c.media_upstream.workers, 1, 1024); }}});
                  }},
                 {"media_stream",
                  [&](const json& v, const std::string& p) {
                      auto& m = c.media_stream;
                      r.object(v, p,
                               {
                                   {"bridge_buffer_bytes", [&](const json& x, const std::string& q) { r.number(x, q, m.bridge_buffer_bytes, 4 * cfg::kib, 64 * cfg::mib); }},
                                   {"connect_timeout_seconds", [&](const json& x, const std::string& q) { r.duration(x, q, m.connect_timeout, 1, 60); }},
                                   {"idle_seconds", [&](const json& x, const std::string& q) { r.duration(x, q, m.idle, 1, 3600); }},
                                   {"max_stream_seconds", [&](const json& x, const std::string& q) { r.duration(x, q, m.max_stream, 10, 86400); }},
                               });
                  }},
                 {"lifetimes",
                  [&](const json& v, const std::string& p) {
                      auto& l = c.lifetimes;
                      r.object(v, p,
                               {
                                   {"asset_seconds", [&](const json& x, const std::string& q) { r.duration(x, q, l.asset, 10, 86400); }},
                                   {"cursor_seconds", [&](const json& x, const std::string& q) { r.duration(x, q, l.cursor, 10, 86400); }},
                                   {"match_seconds", [&](const json& x, const std::string& q) { r.duration(x, q, l.match, 10, 86400); }},
                                   {"media_access_seconds", [&](const json& x, const std::string& q) { r.duration(x, q, l.media_access, 15, 300); }},
                               });
                  }},
                 {"storage",
                  [&](const json& v, const std::string& p) {
                      auto& s = c.storage;
                      r.object(v, p,
                               {
                                   {"writer_queue", [&](const json& x, const std::string& q) { r.number(x, q, s.writer_queue, 1, 65535); }},
                                   {"read_connections", [&](const json& x, const std::string& q) { r.number(x, q, s.read_connections, 1, 64); }},
                                   {"wait_budget_ms", [&](const json& x, const std::string& q) { r.duration(x, q, s.wait_budget, 10, 60000); }},
                               });
                  }},
                 {"cache",
                  [&](const json& v, const std::string& p) {
                      auto& k = c.cache;
                      r.object(v, p,
                               {
                                   {"catalog_entries", [&](const json& x, const std::string& q) { r.number(x, q, k.catalog_entries, 0, 1000000); }},
                                   {"catalog_bytes", [&](const json& x, const std::string& q) { r.number(x, q, k.catalog_bytes, 0, 4 * cfg::gib); }},
                                   {"catalog_ttl_seconds", [&](const json& x, const std::string& q) { r.duration(x, q, k.catalog_ttl, 0, 86400); }},
                                   {"failure_suppress_seconds", [&](const json& x, const std::string& q) { r.duration(x, q, k.failure_suppress, 0, 60); }},
                                   {"app_token_entries", [&](const json& x, const std::string& q) { r.number(x, q, k.app_token_entries, 0, 100000); }},
                               });
                  }},
                 {"handles",
                  [&](const json& v, const std::string& p) {
                      auto& h = c.handles;
                      r.object(v, p,
                               {
                                   {"contexts", [&](const json& x, const std::string& q) { r.number(x, q, h.contexts, 1, u32_max); }},
                                   {"cursors", [&](const json& x, const std::string& q) { r.number(x, q, h.cursors, 1, u32_max); }},
                                   {"assets", [&](const json& x, const std::string& q) { r.number(x, q, h.assets, 1, u32_max); }},
                                   {"matches", [&](const json& x, const std::string& q) { r.number(x, q, h.matches, 1, u32_max); }},
                                   {"media_tickets", [&](const json& x, const std::string& q) { r.number(x, q, h.media_tickets, 1, u32_max); }},
                                   {"total_bytes", [&](const json& x, const std::string& q) { r.number(x, q, h.total_bytes, 1 * cfg::mib, 4 * cfg::gib); }},
                               });
                  }},
                 {"shutdown",
                  [&](const json& v, const std::string& p) {
                      r.object(v, p, {{"soft_seconds", [&](const json& x, const std::string& q) { r.duration(x, q, c.shutdown.soft, 0, 3600); }}});
                  }},
                 {"filesystem",
                  [&](const json& v, const std::string& p) {
                      r.object(v, p, {{"failed_work_retention_hours", [&](const json& x, const std::string& q) { r.duration(x, q, c.filesystem.failed_work_retention, 0, 8760); }}});
                  }},
                 {"features",
                  [&](const json& v, const std::string& p) {
                      auto& f = c.features;
                      r.object(v, p,
                               {
                                   {"file_access", [&](const json& x, const std::string& q) { r.boolean(x, q, f.file_access); }},
                                   {"media_proxy", [&](const json& x, const std::string& q) { r.boolean(x, q, f.media_proxy); }},
                                   {"direct_media_urls", [&](const json& x, const std::string& q) { r.boolean(x, q, f.direct_media_urls); }},
                                   {"automatic_fallback", [&](const json& x, const std::string& q) { r.boolean(x, q, f.automatic_fallback); }},
                                   {"transcoding", [&](const json& x, const std::string& q) { r.boolean(x, q, f.transcoding); }},
                               });
                  }},
                 {"logging",
                  [&](const json& v, const std::string& p) {
                      r.object(v, p,
                               {
                                   {"format", [&](const json& x, const std::string& q) { r.string(x, q, c.logging.format); }},
                                   {"level", [&](const json& x, const std::string& q) { r.string(x, q, c.logging.level); }},
                               });
                  }},
                 {"destinations", [&](const json& v, const std::string& p) { read_destinations(r, v, p, base, c.destinations); }},
             });
}

void apply_listen_override(Reader& r, const EnvironmentLookup& env, std::string_view name,
                           cfg::ListenerConfig& listener) {
    if (const auto value = env(name)) {
        if (const auto address = detail::parse_listen_address(*value)) {
            listener.host = address->host;
            listener.port = address->port;
        } else {
            r.fail(std::string{"env:"} + std::string{name}, "must be host:port or [ipv6]:port");
        }
    }
}

void apply_environment(Reader& r, const LoadOptions& options, cfg::ServiceConfig& c) {
    const auto& env = options.environment;
    if (const auto value = env("MUSIC_SERVICE_DATA_DIR")) {
        if (value->empty()) {
            r.fail("env:MUSIC_SERVICE_DATA_DIR", "must not be empty");
        } else {
            c.data_dir = resolve(options.working_directory, path_from_utf8(*value));
        }
    }
    apply_listen_override(r, env, "MUSIC_SERVICE_CONTROL_LISTEN", c.control);
    apply_listen_override(r, env, "MUSIC_SERVICE_MEDIA_LISTEN", c.media);
    apply_listen_override(r, env, "MUSIC_SERVICE_EVENTS_LISTEN", c.events);
    if (const auto value = env("MUSIC_SERVICE_PUBLIC_API_URL")) {
        c.public_urls.api_base = *value;
    }
    if (const auto value = env("MUSIC_SERVICE_PUBLIC_MEDIA_URL")) {
        c.public_urls.media_base = *value;
    }
    if (const auto value = env("MUSIC_SERVICE_PUBLIC_EVENTS_URL")) {
        c.public_urls.events_base = *value;
    }
    if (const auto value = env("MUSIC_SERVICE_DOWNLOAD_DIR")) {
        bool applied = false;
        for (auto& destination : c.destinations) {
            if (destination.id == "dst_default") {
                destination.root = resolve(options.working_directory, path_from_utf8(*value));
                applied = true;
            }
        }
        if (!applied) {
            r.fail("env:MUSIC_SERVICE_DOWNLOAD_DIR", "requires a configured destination dst_default");
        }
    }
}

domain::Error invalid(std::vector<domain::ErrorDetail> details) {
    return domain::Error{domain::ErrorCode::validation_failed, "configuration is invalid", false,
                         std::move(details)};
}

domain::Result<cfg::ServiceConfig> load(const std::optional<std::string>& text,
                                        const std::optional<std::filesystem::path>& file,
                                        const LoadOptions& options) {
    cfg::ServiceConfig config;
    Reader reader;
    const auto base = file ? file->parent_path() : options.working_directory;
    config.data_dir = resolve(base, "data");
    config.config_file = file;
    // The download loop's file and event outlets are on by default once the build has them.
    config.media.enabled = options.build.media_server;
    config.events.enabled = options.build.event_server;
    bool file_access_given = false;

    if (text) {
        std::string_view body{*text};
        if (body.rfind("\xEF\xBB\xBF", 0) == 0) {
            body.remove_prefix(3);
        }
        const auto doc = json::parse(body, nullptr, false);
        if (doc.is_discarded()) {
            return invalid({{"config_file", "is not valid JSON"}});
        }
        read_document(reader, doc, base, config);
        const auto features = doc.find("features");
        file_access_given = features != doc.end() && features->is_object() && features->contains("file_access");
    }
    // Unless set, file access follows the media listener it is served on.
    if (!file_access_given) {
        config.features.file_access = config.media.enabled;
    }

    apply_environment(reader, options, config);
    if (options.data_dir) {
        config.data_dir = resolve(options.working_directory, *options.data_dir);
    }

    // Report read and cross-field problems together so one run shows everything to fix.
    auto details = std::move(reader.errors);
    if (auto valid = validate_service_config(config, options.build); !valid) {
        const auto& more = valid.error().details;
        details.insert(details.end(), more.begin(), more.end());
    }
    if (!details.empty()) {
        return invalid(std::move(details));
    }
    return config;
}

} // namespace

std::optional<std::string> process_environment(std::string_view name) {
#ifdef _WIN32
    // The wide API keeps non-ASCII values intact; convert to UTF-8.
    const std::wstring wide_name{name.begin(), name.end()};
    wchar_t* value = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&value, &length, wide_name.c_str()) != 0 || value == nullptr) {
        return std::nullopt;
    }
    const std::unique_ptr<wchar_t, decltype(&std::free)> owned{value, &std::free};
    const auto u8 = std::filesystem::path{owned.get()}.u8string();
    return std::string{u8.begin(), u8.end()};
#else
    const std::string key{name};
    const char* value = std::getenv(key.c_str());
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string{value};
#endif
}

domain::Result<cfg::ServiceConfig> load_service_config_text(std::string_view json_text,
                                                            const std::filesystem::path& file_path,
                                                            const LoadOptions& options) {
    return load(std::string{json_text}, resolve(options.working_directory, file_path), options);
}

domain::Result<cfg::ServiceConfig> load_service_config(const LoadOptions& options) {
    std::optional<std::filesystem::path> file = options.config_file;
    if (!file) {
        if (const auto value = options.environment("MUSIC_SERVICE_CONFIG")) {
            file = path_from_utf8(*value);
        }
    }
    if (!file) {
        return load(std::nullopt, std::nullopt, options);
    }

    const auto absolute_file = resolve(options.working_directory, *file);
    std::ifstream stream{absolute_file, std::ios::binary};
    if (!stream) {
        return invalid({{"config_file", "cannot be read"}});
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return load(buffer.str(), absolute_file, options);
}

} // namespace music_service::infrastructure::config
