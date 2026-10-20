#include "api/httplib/control_api.hpp"

#include "application/accounts/scopes.hpp"

#ifdef _MSC_VER
// httplib.h has unreachable code (C4702) that MSVC reports while generating code
// for this file; it is third-party code, not ours.
#pragma warning(push)
#pragma warning(disable : 4702)
#endif
#include <httplib.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <ctime>
#include <exception>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

namespace music_service::api {

namespace {

using json = nlohmann::json;
namespace ports = application::ports;
namespace accounts = application::accounts;
using domain::ErrorCode;
using SteadyClock = std::chrono::steady_clock;

// ---------------------------------------------------------------- formatting

std::string utc(std::chrono::system_clock::time_point time) {
    const auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
    auto seconds = static_cast<std::time_t>(millis / 1000);
    auto ms = static_cast<int>(millis % 1000);
    if (ms < 0) {
        ms += 1000;
        --seconds;
    }
    std::tm parts{};
#ifdef _WIN32
    gmtime_s(&parts, &seconds);
#else
    gmtime_r(&seconds, &parts);
#endif
    char buffer[32];
    const auto written = std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &parts);
    std::string out(buffer, written);
    const char digits[] = {static_cast<char>('0' + ms / 100), static_cast<char>('0' + ms / 10 % 10),
                           static_cast<char>('0' + ms % 10)};
    out.push_back('.');
    out.append(digits, 3);
    out.push_back('Z');
    return out;
}

json optional_time(const std::optional<std::chrono::system_clock::time_point>& time) {
    return time ? json(utc(*time)) : json(nullptr);
}

json optional_string(const std::optional<std::string>& value) { return value ? json(*value) : json(nullptr); }

// ------------------------------------------------------------------- errors

struct ApiError {
    int status{500};
    std::string code;
    std::string message;
    bool retryable{false};
    std::vector<domain::ErrorDetail> details;
};

int status_for(ErrorCode code) {
    switch (code) {
    case ErrorCode::validation_failed:
    case ErrorCode::unknown_provider:
    case ErrorCode::cursor_mismatch:
        return 400;
    case ErrorCode::not_found:
        return 404;
    case ErrorCode::conflict:
    case ErrorCode::version_conflict:
    case ErrorCode::idempotency_conflict:
    case ErrorCode::task_finalizing:
    case ErrorCode::task_terminal:
    case ErrorCode::active_duplicate:
    case ErrorCode::invalid_transition:
        return 409;
    case ErrorCode::cursor_expired:
    case ErrorCode::resource_expired:
        return 410;
    case ErrorCode::capability_unsupported:
    case ErrorCode::provider_disabled:
    case ErrorCode::context_required:
    case ErrorCode::quality_unavailable:
    case ErrorCode::preview_rejected:
    case ErrorCode::credentials_required:
    case ErrorCode::destination_unavailable:
        return 422;
    case ErrorCode::queue_limit_exceeded:
        return 429;
    case ErrorCode::upstream_failed:
    case ErrorCode::upstream_rate_limited:
        return 502;
    case ErrorCode::service_busy:
    case ErrorCode::storage_unavailable:
    case ErrorCode::instance_locked:
        return 503;
    case ErrorCode::upstream_timeout:
    case ErrorCode::deadline_exceeded:
        return 504;
    case ErrorCode::canceled:
    case ErrorCode::destination_root_changed:
    case ErrorCode::destination_in_use:
    case ErrorCode::file_system:
    case ErrorCode::internal:
        break;
    }
    return 500;
}

ApiError from_domain(const domain::Error& error) {
    ApiError api{status_for(error.code), std::string{domain::to_string(error.code)}, error.message,
                 error.retryable, error.details};
    if (api.status == 500) {
        // Only a sanitized message and the request id leave the service.
        api.code = "INTERNAL_ERROR";
        api.message = "internal error";
        api.details.clear();
    }
    return api;
}

ApiError api_error(int status, std::string code, std::string message, std::vector<domain::ErrorDetail> details = {}) {
    return ApiError{status, std::move(code), std::move(message), false, std::move(details)};
}

ApiError bad_request(std::string field, std::string reason) {
    return api_error(400, "VALIDATION_FAILED", "request is invalid", {{std::move(field), std::move(reason)}});
}

json meta(const std::string& request_id) { return json{{"request_id", request_id}, {"partial", false}}; }

void send_data(httplib::Response& res, int status, json data, const std::string& request_id) {
    res.status = status;
    res.set_content(json{{"data", std::move(data)}, {"meta", meta(request_id)}, {"error", nullptr}}.dump(),
                    "application/json; charset=utf-8");
}

json error_json(const ApiError& error) {
    json details = json::array();
    for (const auto& detail : error.details) {
        details.push_back({{"field", detail.field}, {"reason", detail.reason}});
    }
    return json{{"code", error.code},
                {"message", error.message},
                {"retryable", error.retryable},
                {"details", std::move(details)}};
}

// Full envelope for responses that carry extra meta (search source results).
void send_envelope(httplib::Response& res, int status, json data, json meta_object, json error) {
    res.status = status;
    res.set_content(json{{"data", std::move(data)}, {"meta", std::move(meta_object)}, {"error", std::move(error)}}.dump(),
                    "application/json; charset=utf-8");
}

void send_error(httplib::Response& res, const ApiError& error, const std::string& request_id) {
    json details = json::array();
    for (const auto& detail : error.details) {
        details.push_back({{"field", detail.field}, {"reason", detail.reason}});
    }
    res.status = error.status;
    res.set_content(json{{"data", nullptr},
                         {"meta", meta(request_id)},
                         {"error",
                          {{"code", error.code},
                           {"message", error.message},
                           {"retryable", error.retryable},
                           {"details", std::move(details)}}}}
                        .dump(),
                    "application/json; charset=utf-8");
}

// ---------------------------------------------------------------- helpers

bool valid_client_request_id(const std::string& id) {
    if (id.empty() || id.size() > 64) {
        return false;
    }
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
               c == '-';
    });
}

bool is_json_content_type(const std::string& value) {
    const auto type = value.substr(0, value.find(';'));
    std::string lower;
    for (const char c : type) {
        if (c != ' ' && c != '\t') {
            lower.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
        }
    }
    return lower == "application/json";
}

std::string to_string(ports::CredentialStatus status) {
    switch (status) {
    case ports::CredentialStatus::invalid:
        return "invalid";
    case ports::CredentialStatus::revoked:
        return "revoked";
    case ports::CredentialStatus::active:
        break;
    }
    return "active";
}

json outcome_json(const std::optional<ports::VerifyOutcome>& outcome) {
    if (!outcome) {
        return nullptr;
    }
    switch (*outcome) {
    case ports::VerifyOutcome::valid:
        return "valid";
    case ports::VerifyOutcome::invalid:
        return "invalid";
    case ports::VerifyOutcome::unknown:
        break;
    }
    return "unknown";
}

// CredentialDescriptor: never the secret or its reference.
json credential_json(const ports::CredentialRecord& c) {
    return json{{"credential_id", c.id},
                {"source", c.source},
                {"label", c.label},
                {"kind", c.kind},
                {"auth_profile", c.auth_profile},
                {"scope_summary", nullptr},
                {"expires_at", optional_time(c.expires_at)},
                {"version", c.version},
                {"status", to_string(c.status)},
                {"last_verified_at", optional_time(c.last_verified_at)},
                {"last_verify_result", outcome_json(c.last_verify_result)},
                {"created_at", utc(c.created_at)}};
}

json selection_json(const ports::DefaultSelection& selection) {
    return json{{"source", selection.source},
                {"credential_id", optional_string(selection.credential_id)},
                {"selection_version", selection.selection_version}};
}

json ref_json(const domain::PlatformRef& ref) { return json{{"source", ref.source.value()}, {"id", ref.id.value()}}; }

json optional_ref(const std::optional<domain::PlatformRef>& ref) { return ref ? ref_json(*ref) : json(nullptr); }

std::string_view version_kind(domain::VersionKind kind) {
    switch (kind) {
    case domain::VersionKind::studio:
        return "studio";
    case domain::VersionKind::live:
        return "live";
    case domain::VersionKind::remix:
        return "remix";
    case domain::VersionKind::cover:
        return "cover";
    case domain::VersionKind::instrumental:
        return "instrumental";
    case domain::VersionKind::unknown:
        break;
    }
    return "unknown";
}

json track_json(const domain::Track& track) {
    json artists = json::array();
    for (const auto& artist : track.artists) {
        artists.push_back({{"name", artist.name}, {"ref", optional_ref(artist.ref)}});
    }
    json evidence = json::array();
    for (const auto& item : track.version.evidence) {
        evidence.push_back(item);
    }
    return json{{"ref", ref_json(track.ref)},
                {"title", track.title},
                {"artists", std::move(artists)},
                {"album", track.album ? json{{"title", track.album->title}, {"ref", optional_ref(track.album->ref)}}
                                      : json(nullptr)},
                {"duration_ms", track.duration_ms ? json(*track.duration_ms) : json(nullptr)},
                {"version",
                 {{"kind", version_kind(track.version.kind)},
                  {"label", optional_string(track.version.label)},
                  {"evidence", std::move(evidence)}}},
                {"artwork", nullptr},
                // No supported source needs private context yet.
                {"context_ref", nullptr}};
}

json warnings_json(const std::vector<domain::ResourceWarning>& warnings) {
    json out = json::array();
    for (const auto warning : warnings) {
        std::string_view message = "the resource could not be fully verified";
        switch (warning) {
        case domain::ResourceWarning::full_length_unverified:
            message = "no sufficient evidence of the full length";
            break;
        case domain::ResourceWarning::quality_unverified:
            message = "the actual quality is not verified";
            break;
        case domain::ResourceWarning::quality_lowered:
            message = "a lower quality than requested was accepted";
            break;
        case domain::ResourceWarning::preview_resource:
            message = "this is a preview, not the full track";
            break;
        }
        out.push_back({{"code", domain::to_string(warning)}, {"message", message}});
    }
    return out;
}

json optional_int(const std::optional<std::int64_t>& value) { return value ? json(*value) : json(nullptr); }

json string_array(const std::vector<std::string>& values) {
    json out = json::array();
    for (const auto& value : values) {
        out.push_back(value);
    }
    return out;
}

json capability_json(const ports::CapabilityView& capability) {
    const auto& spec = capability.spec;
    json out{{"supported", true},
             {"enabled", capability.enabled},
             {"requires_credentials", spec.credentials == ports::CredentialRequirement::required},
             {"credentials", ports::to_string(spec.credentials)},
             {"pagination", ports::to_string(spec.pagination)},
             {"requirements",
              {{"auth_profiles", string_array(spec.auth_profiles)},
               {"scopes", string_array(spec.scopes)},
               {"regions", string_array(spec.regions)},
               {"dependencies", string_array(spec.dependencies)}}},
             // No probing: availability is only known once calls report it.
             {"availability", {{"state", "unknown"}, {"reason", nullptr}, {"checked_at", nullptr}}}};
    if (spec.catalog_kind) {
        out["catalog_kind"] = ports::to_string(*spec.catalog_kind);
    }
    return out;
}

json provider_json(const ports::ProviderView& view, json credentials) {
    json capabilities = json::object();
    for (const auto& capability : view.capabilities) {
        capabilities[capability.spec.key] = capability_json(capability);
    }
    json methods = json::array();
    for (const auto& method : view.auth_methods) {
        json fields = json::array();
        for (const auto& field : method.input_fields) {
            fields.push_back({{"name", field.name}, {"required", field.required}, {"secret", field.secret}});
        }
        methods.push_back({{"auth_profile", method.auth_profile},
                           {"kind", ports::to_string(method.kind)},
                           {"version", method.version},
                           {"input_fields", std::move(fields)},
                           {"scopes", string_array(method.scopes)},
                           {"supports_verify", method.supports_verify},
                           {"supports_refresh", method.supports_refresh},
                           {"supports_application_token", method.supports_application_token}});
    }
    json qualities = json::array();
    for (const auto quality : view.quality_hints) {
        qualities.push_back(domain::to_string(quality));
    }
    return json{{"source", view.source.value()},
                {"name", view.name},
                {"enabled", view.enabled},
                {"implementation",
                 {{"id", view.implementation_id},
                  {"version", view.implementation_version},
                  {"execution_mode", ports::to_string(view.execution_mode)}}},
                {"capabilities", std::move(capabilities)},
                {"auth_methods", std::move(methods)},
                {"quality_hints", std::move(qualities)},
                {"credentials", std::move(credentials)}};
}

// One request on a route: identity, parsed input and the response.
struct Call {
    const httplib::Request& req;
    httplib::Response& res;
    std::string request_id;
    std::optional<ports::AuthenticatedToken> session;

    [[nodiscard]] const std::string& principal() const { return session->principal.id; }
    void ok(json data, int status = 200) const { send_data(res, status, std::move(data), request_id); }
    void fail(const ApiError& error) const { send_error(res, error, request_id); }
    void fail(const domain::Error& error) const { send_error(res, from_domain(error), request_id); }
};

// Unknown query parameters are rejected rather than silently ignored.
bool only_params(const Call& call, std::initializer_list<std::string_view> allowed) {
    for (const auto& [name, value] : call.req.params) {
        if (std::find(allowed.begin(), allowed.end(), name) == allowed.end()) {
            call.fail(bad_request(name, "unknown parameter"));
            return false;
        }
    }
    return true;
}

// The JSON object body, with only the allowed keys. Sends 400 and returns nullopt
// otherwise. An empty body counts as {} when allow_empty.
std::optional<json> body_object(const Call& call, std::initializer_list<std::string_view> allowed,
                                bool allow_empty = false) {
    if (call.req.body.empty()) {
        if (allow_empty) {
            return json::object();
        }
        call.fail(bad_request("body", "a JSON object is required"));
        return std::nullopt;
    }
    auto parsed = json::parse(call.req.body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        call.fail(bad_request("body", "must be a JSON object"));
        return std::nullopt;
    }
    for (const auto& [key, value] : parsed.items()) {
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            call.fail(bad_request(key, "unknown field"));
            return std::nullopt;
        }
    }
    return parsed;
}

// Reads an optional string member; sends 400 for another type.
bool read_string(const Call& call, const json& object, const char* key, std::optional<std::string>& out) {
    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) {
        return true;
    }
    if (!it->is_string()) {
        call.fail(bad_request(key, "must be a string"));
        return false;
    }
    out = it->get<std::string>();
    return true;
}

// The cookie from {"secret": {"cookie": "..."}} or the {"cookie": "..."} shorthand.
bool read_cookie(const Call& call, const json& body, std::optional<std::string>& cookie) {
    const auto secret = body.find("secret");
    const auto shorthand = body.find("cookie");
    if (secret != body.end() && shorthand != body.end()) {
        call.fail(bad_request("cookie", "cannot be combined with secret"));
        return false;
    }
    if (shorthand != body.end()) {
        return read_string(call, body, "cookie", cookie);
    }
    if (secret != body.end()) {
        if (!secret->is_object()) {
            call.fail(bad_request("secret", "must be an object"));
            return false;
        }
        for (const auto& [key, value] : secret->items()) {
            if (key != "cookie") {
                call.fail(bad_request("secret." + key, "unknown field"));
                return false;
            }
        }
        return read_string(call, *secret, "cookie", cookie);
    }
    return true;
}

thread_local SteadyClock::time_point request_started;

} // namespace

struct ControlApi::Impl {
    ControlApiOptions options;
    ControlApiServices services;
    httplib::Server server;
    std::thread thread;
    std::atomic<std::uint16_t> port{0};
    std::once_flag stopped;

    Impl(ControlApiOptions o, ControlApiServices s) : options(std::move(o)), services(std::move(s)) {
        configure();
        routes();
    }

    [[nodiscard]] bool origin_allowed(const std::string& origin) const {
        const auto& allowed = options.cors.allowed_origins;
        return !origin.empty() && std::find(allowed.begin(), allowed.end(), origin) != allowed.end();
    }

    void configure() {
        const auto workers = std::max<std::uint32_t>(options.listener.workers, 1);
        const auto queue = options.listener.connection_queue;
        server.new_task_queue = [workers, queue] { return new httplib::ThreadPool(workers, workers, queue); };
        server.set_payload_max_length(options.http.body_limit_bytes);
        server.set_read_timeout(static_cast<time_t>(options.http.read_idle.count()));
        server.set_write_timeout(static_cast<time_t>(options.http.write_idle.count()));
        server.set_keep_alive_timeout(static_cast<time_t>(options.http.keep_alive.count()));
        server.set_keep_alive_max_count(options.http.max_requests_per_connection);

        server.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
            request_started = SteadyClock::now();
            auto id = req.get_header_value("X-Request-Id");
            if (!valid_client_request_id(id)) {
                id = services.new_request_id();
            }
            res.set_header("X-Request-Id", id);
            const auto origin = req.get_header_value("Origin");
            if (origin_allowed(origin)) {
                res.set_header("Access-Control-Allow-Origin", origin);
                res.set_header("Vary", "Origin");
                res.set_header("Access-Control-Expose-Headers", "ETag, Retry-After, X-Request-Id");
            }
            if (req.method == "OPTIONS") {
                // Preflight never reaches a route or a platform.
                if (!origin_allowed(origin)) {
                    send_error(res, api_error(403, "FORBIDDEN", "origin is not allowed"), id);
                    return httplib::Server::HandlerResponse::Handled;
                }
                res.status = 204;
                res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, DELETE, OPTIONS");
                res.set_header("Access-Control-Allow-Headers",
                               "Authorization, Content-Type, Idempotency-Key, If-None-Match, X-Request-Id");
                res.set_header("Access-Control-Max-Age", "600");
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });

        // Library-generated errors (unknown route, oversized body) get the envelope too.
        server.set_error_handler([this](const httplib::Request&, httplib::Response& res) {
            if (!res.body.empty()) {
                return httplib::Server::HandlerResponse::Unhandled;
            }
            auto id = res.get_header_value("X-Request-Id");
            if (id.empty()) {
                id = services.new_request_id();
                res.set_header("X-Request-Id", id);
            }
            const auto status = res.status;
            if (status == 404) {
                send_error(res, api_error(404, "NOT_FOUND", "no such route"), id);
            } else if (status == 405) {
                send_error(res, api_error(405, "METHOD_NOT_ALLOWED", "method not allowed"), id);
            } else if (status == 413) {
                send_error(res, api_error(413, "PAYLOAD_TOO_LARGE", "request body is too large"), id);
            } else if (status >= 500) {
                send_error(res, api_error(500, "INTERNAL_ERROR", "internal error"), id);
            } else {
                send_error(res, api_error(status, "VALIDATION_FAILED", "request is invalid"), id);
            }
            res.status = status;
            return httplib::Server::HandlerResponse::Handled;
        });

        server.set_exception_handler([this](const httplib::Request&, httplib::Response& res, std::exception_ptr) {
            auto id = res.get_header_value("X-Request-Id");
            send_error(res, api_error(500, "INTERNAL_ERROR", "internal error"), id.empty() ? services.new_request_id() : id);
        });

        server.set_logger([this](const httplib::Request& req, const httplib::Response& res) {
            if (services.on_request) {
                const auto elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - request_started);
                services.on_request(res.get_header_value("X-Request-Id"), req.method, req.path, res.status, elapsed);
            }
        });
    }

    enum class Access { open, any_scope, scoped };

    // Wraps a route with authentication, scope and content-type checks.
    httplib::Server::Handler guard(Access access, std::string_view scope, std::function<void(Call&)> route) {
        return [this, access, scope = std::string{scope}, route = std::move(route)](const httplib::Request& req,
                                                                                   httplib::Response& res) {
            Call call{req, res, res.get_header_value("X-Request-Id"), std::nullopt};
            if (access != Access::open) {
                res.set_header("Cache-Control", "no-store");
                const auto header = req.get_header_value("Authorization");
                constexpr std::string_view bearer = "Bearer ";
                std::optional<ports::AuthenticatedToken> session;
                if (header.size() > bearer.size() && std::string_view{header}.substr(0, bearer.size()) == bearer) {
                    auto found = services.authenticator->authenticate(std::string_view{header}.substr(bearer.size()));
                    if (!found) {
                        call.fail(found.error());
                        return;
                    }
                    session = std::move(found).value();
                }
                if (!session) {
                    res.set_header("WWW-Authenticate", "Bearer");
                    call.fail(api_error(401, "UNAUTHENTICATED", "a valid bearer token is required"));
                    return;
                }
                if (access == Access::scoped && !accounts::has_scope(session->token.scopes, scope)) {
                    call.fail(api_error(403, "FORBIDDEN", "the token lacks the " + scope + " scope"));
                    return;
                }
                call.session = std::move(session);
            }
            if (!req.body.empty() && !is_json_content_type(req.get_header_value("Content-Type"))) {
                call.fail(api_error(415, "UNSUPPORTED_CONTENT_TYPE", "request bodies must be application/json"));
                return;
            }
            route(call);
        };
    }

    void routes() {
        server.Get("/health/live", guard(Access::open, {}, [](Call& call) { call.ok({{"status", "live"}}); }));
        server.Get("/health/ready", guard(Access::open, {}, [this](Call& call) {
            if (services.ready && services.ready()) {
                call.ok({{"status", "ready"}});
            } else {
                call.fail(api_error(503, "SERVICE_BUSY", "the service is not ready"));
            }
        }));
        server.Get("/api/v1/info", guard(Access::any_scope, {}, [this](Call& call) { info(call); }));
        server.Get("/api/v1/providers",
                   guard(Access::scoped, accounts::scope::catalog_read, [this](Call& call) { providers(call); }));
        server.Get("/api/v1/limits",
                   guard(Access::scoped, accounts::scope::catalog_read, [this](Call& call) { limits(call); }));
        server.Get("/api/v1/destinations",
                   guard(Access::scoped, accounts::scope::downloads_read, [this](Call& call) { destinations(call); }));

        const auto credentials = accounts::scope::credentials_write;
        server.Get("/api/v1/credentials", guard(Access::scoped, credentials, [this](Call& call) { list_credentials(call); }));
        server.Post("/api/v1/credentials", guard(Access::scoped, credentials, [this](Call& call) { create_credential(call); }));
        server.Put(R"(/api/v1/credentials/([A-Za-z0-9_]+))",
                   guard(Access::scoped, credentials, [this](Call& call) { update_credential(call); }));
        server.Post(R"(/api/v1/credentials/([A-Za-z0-9_]+)/verify)",
                    guard(Access::scoped, credentials, [this](Call& call) { verify_credential(call); }));
        server.Delete(R"(/api/v1/credentials/([A-Za-z0-9_]+))",
                      guard(Access::scoped, credentials, [this](Call& call) { revoke_credential(call); }));
        server.Get(R"(/api/v1/credential-defaults/([a-z0-9_]+))",
                   guard(Access::scoped, credentials, [this](Call& call) { get_default(call); }));
        server.Put(R"(/api/v1/credential-defaults/([a-z0-9_]+))",
                   guard(Access::scoped, credentials, [this](Call& call) { set_default(call); }));
        server.Delete(R"(/api/v1/credential-defaults/([a-z0-9_]+))",
                      guard(Access::scoped, credentials, [this](Call& call) { clear_default(call); }));

        const auto catalog = accounts::scope::catalog_read;
        server.Get("/api/v1/search/tracks", guard(Access::scoped, catalog, [this](Call& call) { search_tracks(call); }));
        server.Get("/api/v1/tracks/detail", guard(Access::scoped, catalog, [this](Call& call) { track_detail(call); }));
        server.Post("/api/v1/links/parse", guard(Access::scoped, catalog, [this](Call& call) { parse_link(call); }));
        server.Post("/api/v1/assets/resolve", guard(Access::scoped, catalog, [this](Call& call) { resolve_asset(call); }));
        server.Post("/api/v1/assets/probe", guard(Access::scoped, catalog, [this](Call& call) { probe_asset(call); }));

        server.Get("/api/v1/admin/status",
                   guard(Access::scoped, accounts::scope::admin, [this](Call& call) { admin_status(call); }));
    }

    // ------------------------------------------------------------ system

    void info(Call& call) {
        const auto& i = options.info;
        const auto& f = i.features;
        call.ok({{"service", "music-service"},
                 {"version", i.version},
                 {"api_version", "v1"},
                 {"instance_id", i.instance_id},
                 {"media_base_url", optional_string(i.media_base_url)},
                 {"events_base_url", optional_string(i.events_base_url)},
                 {"features",
                  {{"search", f.search},
                   {"downloads", f.downloads},
                   {"file_access", f.file_access},
                   {"media_proxy", f.media_proxy},
                   {"direct_media_urls", f.direct_media_urls},
                   {"automatic_fallback", f.automatic_fallback},
                   {"progress_polling", f.progress_polling},
                   {"progress_sse", f.progress_sse}}}});
    }

    void providers(Call& call) {
        if (!only_params(call, {"capability", "enabled", "limit", "cursor"})) {
            return;
        }
        ports::ProviderListQuery query;
        query.principal_id = call.principal();
        if (call.req.has_param("capability")) {
            query.capability = call.req.get_param_value("capability");
        }
        if (call.req.has_param("enabled")) {
            const auto value = call.req.get_param_value("enabled");
            if (value != "true" && value != "false") {
                call.fail(bad_request("enabled", "must be true or false"));
                return;
            }
            query.enabled = value == "true";
        }
        if (call.req.has_param("limit")) {
            const auto value = call.req.get_param_value("limit");
            unsigned limit = 0;
            const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), limit);
            if (ec != std::errc{} || end != value.data() + value.size() || limit < 1 || limit > 100) {
                call.fail(bad_request("limit", "must be between 1 and 100"));
                return;
            }
            query.limit = limit;
        }
        if (call.req.has_param("cursor")) {
            query.cursor = call.req.get_param_value("cursor");
        }
        auto page = services.providers->list(query);
        if (!page) {
            call.fail(page.error());
            return;
        }
        json items = json::array();
        for (const auto& view : page->items) {
            // The caller's own credential summary; never another principal's.
            const auto& source = view.source.value();
            auto owned = services.credentials->list(call.principal(), source);
            auto selection = services.credentials->get_default(call.principal(), source);
            if (!owned || !selection) {
                call.fail(!owned ? owned.error() : selection.error());
                return;
            }
            const auto active = std::count_if(owned->begin(), owned->end(), [](const ports::CredentialRecord& c) {
                return c.status != ports::CredentialStatus::revoked;
            });
            items.push_back(provider_json(view, {{"usable", active},
                                                 {"default_credential_id", optional_string(selection->credential_id)},
                                                 {"selection_version", selection->selection_version}}));
        }
        call.ok({{"items", std::move(items)},
                 {"page", {{"limit", query.limit}, {"next_cursor", optional_string(page->next_cursor)}}}});
    }

    void limits(Call& call) {
        if (!only_params(call, {})) {
            return;
        }
        const auto& c = options.catalog;
        call.ok({{"catalog",
                  {{"default_sources", string_array(c.default_sources)},
                   {"max_sources_per_request", c.max_sources_per_request},
                   {"per_source_limit", {{"default", 20}, {"max", 50}}},
                   {"timeout_ms", {{"default", c.total_budget.count()}, {"min", 1000}, {"max", c.max_budget.count()}}}}},
                 {"downloads",
                  {{"max_batch_items", options.download_limits.max_batch_items},
                   {"per_principal_active", options.download_limits.per_principal_active}}},
                 {"media", {{"per_principal", options.media_limits.per_principal}}}});
    }

    void destinations(Call& call) {
        if (!only_params(call, {})) {
            return;
        }
        json items = json::array();
        for (const auto& d : services.destinations(call.principal())) {
            items.push_back({{"destination_id", d.id},
                             {"name", d.name},
                             {"available", d.available},
                             {"accept_new_tasks", d.accept_new_tasks}});
        }
        call.ok({{"items", std::move(items)}});
    }

    void admin_status(Call& call) {
        const auto s = services.status();
        call.ok({{"state", s.state},
                 {"instance_id", s.instance_id},
                 {"version", s.version},
                 {"started_at", utc(s.started_at)},
                 {"storage", {{"available", s.storage_available}}},
                 {"providers", {{"registered", s.providers_registered}}},
                 {"destinations", {{"configured", s.destinations}}},
                 {"control", {{"workers", s.control_workers}}},
                 {"upstream", {{"connections_in_use", s.upstream_connections_in_use}, {"cooldowns", s.upstream_cooldowns}}}});
    }

    // ----------------------------------------------------------- catalog

    static std::string_view status_name(application::catalog::SourceStatus status) {
        switch (status) {
        case application::catalog::SourceStatus::ok:
            return "ok";
        case application::catalog::SourceStatus::unsupported:
            return "unsupported";
        case application::catalog::SourceStatus::disabled:
            return "disabled";
        case application::catalog::SourceStatus::failed:
            break;
        }
        return "failed";
    }

    void search_tracks(Call& call) {
        if (!only_params(call, {"q", "sources", "limit", "cursor", "page", "timeout_ms"})) {
            return;
        }
        if (call.req.has_param("page")) {
            call.fail(bad_request("page", "page jumps are not supported by the available sources; use cursor"));
            return;
        }
        application::catalog::SearchRequest request;
        request.owner_id = call.principal();
        request.q = call.req.get_param_value("q");
        if (call.req.has_param("sources")) {
            const auto text = call.req.get_param_value("sources");
            if (text == "all") {
                request.all_sources = true;
            } else {
                std::size_t at = 0;
                while (at <= text.size()) {
                    const auto comma = text.find(',', at);
                    const auto part = text.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
                    if (part.empty()) {
                        call.fail(bad_request("sources", "must be a comma-separated list of sources or all"));
                        return;
                    }
                    request.sources.push_back(part);
                    if (comma == std::string::npos) {
                        break;
                    }
                    at = comma + 1;
                }
            }
        }
        if (!read_uint_param(call, "limit", request.limit)) {
            return;
        }
        if (call.req.has_param("cursor")) {
            request.cursor = call.req.get_param_value("cursor");
        }
        if (call.req.has_param("timeout_ms")) {
            std::uint32_t timeout = 0;
            if (!read_uint_param(call, "timeout_ms", timeout)) {
                return;
            }
            request.timeout = std::chrono::milliseconds{timeout};
        }
        auto searched = services.catalog->search_tracks(request);
        if (!searched) {
            call.fail(searched.error());
            return;
        }
        auto& result = searched.value();
        json items = json::array();
        for (const auto& track : result.items) {
            items.push_back(track_json(track));
        }
        json sources = json::array();
        for (const auto& source : result.sources) {
            json page = nullptr;
            if (source.page) {
                const auto& p = *source.page;
                page = {{"limit", p.limit},
                        {"returned", p.returned},
                        {"total", p.total ? json(*p.total) : json(nullptr)},
                        {"has_more", p.has_more ? json(*p.has_more) : json(nullptr)},
                        {"next_cursor", optional_string(p.next_cursor)}};
            }
            json error = nullptr;
            if (source.error) {
                const auto api = from_domain(*source.error);
                error = {{"code", api.code}, {"message", api.message}, {"retryable", api.retryable}};
            }
            json cache = nullptr;
            if (source.cache) {
                cache = {{"hit", source.cache->hit}, {"created_at", utc(source.cache->created_at)}};
            }
            sources.push_back({{"source", source.source},
                               {"status", status_name(source.status)},
                               {"elapsed_ms", source.elapsed.count()},
                               {"page", std::move(page)},
                               {"error", std::move(error)},
                               {"cache", std::move(cache)}});
        }
        json meta_object{{"request_id", call.request_id}, {"partial", result.partial}, {"source_results", std::move(sources)}};
        if (result.failure) {
            const auto api = from_domain(*result.failure);
            send_envelope(call.res, api.status, nullptr, std::move(meta_object), error_json(api));
            return;
        }
        send_envelope(call.res, 200, {{"items", std::move(items)}}, std::move(meta_object), nullptr);
    }

    // Reads an optional unsigned integer parameter; sends 400 when malformed.
    static bool read_uint_param(const Call& call, const char* name, std::uint32_t& out) {
        if (!call.req.has_param(name)) {
            return true;
        }
        const auto value = call.req.get_param_value(name);
        std::uint32_t parsed = 0;
        const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (ec != std::errc{} || end != value.data() + value.size()) {
            call.fail(bad_request(name, "must be a non-negative integer"));
            return false;
        }
        out = parsed;
        return true;
    }

    void track_detail(Call& call) {
        if (!only_params(call, {"source", "id", "context_ref"})) {
            return;
        }
        if (!call.req.has_param("source") || !call.req.has_param("id")) {
            call.fail(bad_request("source", "source and id are required"));
            return;
        }
        auto detail = services.catalog->track_detail(call.principal(), call.req.get_param_value("source"),
                                                     call.req.get_param_value("id"));
        detail ? call.ok(track_json(detail->track)) : call.fail(detail.error());
    }

    void parse_link(Call& call) {
        if (!only_params(call, {})) {
            return;
        }
        auto body = body_object(call, {"text"});
        if (!body) {
            return;
        }
        std::optional<std::string> text;
        if (!read_string(call, *body, "text", text)) {
            return;
        }
        auto parsed = services.catalog->parse_link(call.principal(), text.value_or(""));
        if (!parsed) {
            call.fail(parsed.error());
            return;
        }
        call.ok({{"matched", parsed->matched},
                 {"kind", parsed->matched ? json(parsed->kind) : json(nullptr)},
                 {"ref", optional_ref(parsed->ref)}});
    }

    static json asset_json(const application::catalog::AssetDescriptor& a) {
        return json{{"asset_id", a.asset_id},
                    {"original_track", ref_json(a.original_track)},
                    {"actual_track", ref_json(a.actual_track)},
                    {"requested_quality", domain::to_string(a.requested_quality)},
                    {"actual_quality", a.actual_quality ? json(domain::to_string(*a.actual_quality)) : json(nullptr)},
                    {"container", optional_string(a.container)},
                    {"bitrate_bps", optional_int(a.bitrate_bps)},
                    {"size_bytes", nullptr},
                    {"access_state", "available"},
                    {"completeness", domain::to_string(a.completeness)},
                    {"expires_at", utc(a.expires_at)},
                    {"warnings", warnings_json(a.warnings)}};
    }

    void resolve_asset(Call& call) {
        if (!only_params(call, {})) {
            return;
        }
        auto body = body_object(call, {"track", "context_ref", "quality", "quality_policy", "allow_preview", "fallback",
                                       "match_selection"});
        if (!body) {
            return;
        }
        const auto track = body->find("track");
        if (track == body->end() || !track->is_object() || !(*track)["source"].is_string() ||
            !(*track)["id"].is_string() || track->size() != 2) {
            call.fail(bad_request("track", "must be {source, id}"));
            return;
        }
        auto source = domain::SourceId::parse((*track)["source"].get<std::string>());
        auto id = domain::PlatformId::parse((*track)["id"].get<std::string>());
        if (!source || !id) {
            call.fail(bad_request("track", "has an invalid source or id"));
            return;
        }
        application::catalog::ResolveRequest request{call.principal(), {std::move(source).value(), std::move(id).value()},
                                                     {}, false, false};
        std::optional<std::string> quality;
        std::optional<std::string> policy;
        if (!read_string(call, *body, "quality", quality) || !read_string(call, *body, "quality_policy", policy)) {
            return;
        }
        if (quality) {
            auto parsed = domain::parse_quality(*quality);
            if (!parsed) {
                call.fail(bad_request("quality", "must be standard, high, lossless or hires"));
                return;
            }
            request.requirement.requested = parsed.value();
        }
        if (policy) {
            auto parsed = domain::parse_quality_policy(*policy);
            if (!parsed) {
                call.fail(bad_request("quality_policy", "must be strict or allow_lower"));
                return;
            }
            request.requirement.policy = parsed.value();
        }
        if (const auto preview = body->find("allow_preview"); preview != body->end()) {
            if (!preview->is_boolean()) {
                call.fail(bad_request("allow_preview", "must be a boolean"));
                return;
            }
            request.requirement.allow_preview = preview->get<bool>();
        }
        if (const auto fallback = body->find("fallback"); fallback != body->end()) {
            if (!fallback->is_object() || !(*fallback)["mode"].is_string() || fallback->size() != 1) {
                call.fail(bad_request("fallback", "must be {mode: disabled|auto}"));
                return;
            }
            const auto mode = (*fallback)["mode"].get<std::string>();
            if (mode != "disabled" && mode != "auto") {
                call.fail(bad_request("fallback.mode", "must be disabled or auto"));
                return;
            }
            request.automatic_fallback = mode == "auto";
        }
        request.match_selection = body->contains("match_selection") && !(*body)["match_selection"].is_null();
        auto resolved = services.assets->resolve(request);
        resolved ? call.ok(asset_json(resolved.value())) : call.fail(resolved.error());
    }

    void probe_asset(Call& call) {
        if (!only_params(call, {})) {
            return;
        }
        auto body = body_object(call, {"asset_id"});
        if (!body) {
            return;
        }
        std::optional<std::string> asset_id;
        if (!read_string(call, *body, "asset_id", asset_id)) {
            return;
        }
        if (!asset_id) {
            call.fail(bad_request("asset_id", "is required"));
            return;
        }
        auto report = services.assets->probe(call.principal(), *asset_id);
        if (!report) {
            call.fail(report.error());
            return;
        }
        const auto& r = report.value();
        call.ok({{"asset_id", r.asset_id},
                 {"accessible", r.accessible},
                 {"http_status", r.http_status},
                 {"content_type", optional_string(r.content_type)},
                 {"size_bytes", optional_int(r.size_bytes)},
                 {"range_supported", r.range_supported},
                 {"format", optional_string(r.format)},
                 {"estimated_duration_ms", optional_int(r.estimated_duration_ms)},
                 {"completeness", domain::to_string(r.completeness)},
                 {"warnings", warnings_json(r.warnings)}});
    }

    // ------------------------------------------------------- credentials

    void list_credentials(Call& call) {
        if (!only_params(call, {"source"})) {
            return;
        }
        std::optional<std::string> source;
        if (call.req.has_param("source")) {
            source = call.req.get_param_value("source");
        }
        auto listed = services.credentials->list(call.principal(), source);
        if (!listed) {
            call.fail(listed.error());
            return;
        }
        json items = json::array();
        for (const auto& c : listed.value()) {
            items.push_back(credential_json(c));
        }
        call.ok({{"items", std::move(items)}});
    }

    // The platform's only cookie profile, for the {"cookie": ...} shorthand.
    domain::Result<std::string> default_cookie_profile(const std::string& source) {
        auto view = services.providers->describe(source);
        if (!view) {
            return std::move(view).error();
        }
        std::vector<std::string> cookie_profiles;
        for (const auto& method : view->auth_methods) {
            if (method.kind == ports::CredentialKind::cookie) {
                cookie_profiles.push_back(method.auth_profile);
            }
        }
        if (cookie_profiles.empty()) {
            return domain::Error{ErrorCode::capability_unsupported, "this source accepts no cookie credentials", false, {}};
        }
        if (cookie_profiles.size() > 1) {
            return domain::Error::validation("auth_profile", "this source has several cookie profiles; name one");
        }
        return cookie_profiles.front();
    }

    void create_credential(Call& call) {
        if (!only_params(call, {})) {
            return;
        }
        auto body = body_object(call, {"source", "label", "kind", "auth_profile", "secret", "cookie"});
        if (!body) {
            return;
        }
        std::optional<std::string> source;
        std::optional<std::string> label;
        std::optional<std::string> kind;
        std::optional<std::string> profile;
        std::optional<std::string> cookie;
        if (!read_string(call, *body, "source", source) || !read_string(call, *body, "label", label) ||
            !read_string(call, *body, "kind", kind) || !read_string(call, *body, "auth_profile", profile)) {
            return;
        }
        if (!source) {
            call.fail(bad_request("source", "is required"));
            return;
        }
        if (kind && *kind != "cookie" && *kind != "oauth2" && *kind != "api_key") {
            call.fail(bad_request("kind", "must be cookie, oauth2 or api_key"));
            return;
        }
        if (kind && *kind != "cookie") {
            call.fail(api_error(422, "CAPABILITY_UNSUPPORTED", "only cookie credentials are supported by this build"));
            return;
        }
        const bool shorthand = body->contains("cookie");
        if (!shorthand && (!kind || !profile || !body->contains("secret"))) {
            call.fail(bad_request("secret", "kind, auth_profile and secret are required unless cookie is given"));
            return;
        }
        if (!read_cookie(call, *body, cookie)) {
            return;
        }
        if (!cookie) {
            call.fail(bad_request("secret.cookie", "is required"));
            return;
        }
        if (!profile) {
            auto found = default_cookie_profile(*source);
            if (!found) {
                call.fail(found.error());
                return;
            }
            profile = std::move(found).value();
        }
        auto created = services.credentials->create({call.principal(), *source, label.value_or(""),
                                                     ports::CredentialKind::cookie, *profile,
                                                     ports::SecretBytes{std::move(*cookie)}, std::nullopt});
        if (!created) {
            call.fail(created.error());
            return;
        }
        call.ok(credential_json(created.value()), 201);
    }

    void update_credential(Call& call) {
        if (!only_params(call, {})) {
            return;
        }
        auto body = body_object(call, {"label", "secret", "cookie"});
        if (!body) {
            return;
        }
        std::optional<std::string> label;
        std::optional<std::string> cookie;
        if (!read_string(call, *body, "label", label) || !read_cookie(call, *body, cookie)) {
            return;
        }
        accounts::CredentialUpdate update;
        update.label = std::move(label);
        if (cookie) {
            update.secret = ports::SecretBytes{std::move(*cookie)};
        }
        auto updated = services.credentials->update(call.principal(), call.req.matches[1], std::move(update));
        if (!updated) {
            call.fail(updated.error());
            return;
        }
        call.ok(credential_json(updated.value()));
    }

    void verify_credential(Call& call) {
        if (!only_params(call, {}) || !body_object(call, {}, true)) {
            return;
        }
        const ports::OperationContext context{SteadyClock::now() + options.catalog.total_budget, {}};
        auto verified = services.credentials->verify(call.principal(), call.req.matches[1], context);
        if (!verified) {
            call.fail(verified.error());
            return;
        }
        const auto status = verified->verification.status;
        call.ok({{"result", status == ports::VerificationStatus::valid     ? "valid"
                            : status == ports::VerificationStatus::invalid ? "invalid"
                                                                           : "unknown"},
                 {"account_hint", optional_string(verified->verification.account_hint)},
                 {"credential", credential_json(verified->credential)}});
    }

    void revoke_credential(Call& call) {
        if (!only_params(call, {})) {
            return;
        }
        auto revoked = services.credentials->revoke(call.principal(), call.req.matches[1]);
        if (!revoked) {
            call.fail(revoked.error());
            return;
        }
        call.ok(credential_json(revoked.value()));
    }

    void get_default(Call& call) {
        if (!only_params(call, {})) {
            return;
        }
        auto selection = services.credentials->get_default(call.principal(), call.req.matches[1]);
        selection ? call.ok(selection_json(selection.value())) : call.fail(selection.error());
    }

    void set_default(Call& call) {
        if (!only_params(call, {})) {
            return;
        }
        auto body = body_object(call, {"credential_id"});
        if (!body) {
            return;
        }
        std::optional<std::string> credential_id;
        if (!read_string(call, *body, "credential_id", credential_id)) {
            return;
        }
        if (!credential_id) {
            call.fail(bad_request("credential_id", "is required"));
            return;
        }
        auto selection = services.credentials->set_default(call.principal(), call.req.matches[1], *credential_id);
        selection ? call.ok(selection_json(selection.value())) : call.fail(selection.error());
    }

    void clear_default(Call& call) {
        if (!only_params(call, {})) {
            return;
        }
        auto selection = services.credentials->clear_default(call.principal(), call.req.matches[1]);
        selection ? call.ok(selection_json(selection.value())) : call.fail(selection.error());
    }
};

ControlApi::ControlApi(ControlApiOptions options, ControlApiServices services)
    : impl_(std::make_unique<Impl>(std::move(options), std::move(services))) {}

ControlApi::~ControlApi() { stop(); }

domain::Result<std::uint16_t> ControlApi::bind() {
    const auto& listener = impl_->options.listener;
    int bound = -1;
    if (listener.port == 0) {
        bound = impl_->server.bind_to_any_port(listener.host);
    } else if (impl_->server.bind_to_port(listener.host, listener.port)) {
        bound = listener.port;
    }
    if (bound <= 0) {
        domain::Error error{ErrorCode::service_busy, "the control listener could not bind", false, {}};
        error.details.push_back({"control", listener.host + ":" + std::to_string(listener.port) +
                                                " is in use or not available"});
        return error;
    }
    impl_->port = static_cast<std::uint16_t>(bound);
    return impl_->port.load();
}

void ControlApi::start() {
    impl_->thread = std::thread([this] { impl_->server.listen_after_bind(); });
    impl_->server.wait_until_ready();
}

void ControlApi::stop() {
    if (!impl_) {
        return;
    }
    std::call_once(impl_->stopped, [this] {
        impl_->server.stop();
        if (impl_->thread.joinable()) {
            impl_->thread.join();
        }
    });
}

std::uint16_t ControlApi::port() const noexcept { return impl_->port.load(); }

} // namespace music_service::api
