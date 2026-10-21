#include "media_server/httplib/media_server.hpp"

#include "application/accounts/scopes.hpp"
#include "application/media/media_access.hpp"

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
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace music_service::media_server {

namespace {

using json = nlohmann::json;
namespace accounts = application::accounts;
namespace media = application::media;
using domain::ErrorCode;
using SteadyClock = std::chrono::steady_clock;

constexpr std::size_t chunk_bytes = 64 * 1024;
constexpr std::string_view ticket_route = "/media/v1/tickets/";

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
    case ErrorCode::multiple_ranges_unsupported:
        return 400;
    case ErrorCode::not_found:
        return 404;
    case ErrorCode::resource_expired:
    case ErrorCode::file_missing:
    case ErrorCode::media_ticket_expired:
        return 410;
    case ErrorCode::destination_unavailable:
        return 422;
    case ErrorCode::caller_limit_exceeded:
        return 429;
    case ErrorCode::feature_unavailable:
        return 501;
    case ErrorCode::service_busy:
    case ErrorCode::storage_unavailable:
        return 503;
    default:
        return 500;
    }
}

void send_error(httplib::Response& res, ApiError error, const std::string& request_id) {
    if (error.status == 500) {
        error.code = "INTERNAL_ERROR";
        error.message = "internal error";
        error.details.clear();
    }
    json details = json::array();
    for (const auto& detail : error.details) {
        details.push_back({{"field", detail.field}, {"reason", detail.reason}});
    }
    res.status = error.status;
    res.set_header("Cache-Control", "no-store");
    res.set_content(json{{"data", nullptr},
                         {"meta", {{"request_id", request_id}, {"partial", false}}},
                         {"error",
                          {{"code", error.code},
                           {"message", error.message},
                           {"retryable", error.retryable},
                           {"details", std::move(details)}}}}
                        .dump(),
                    "application/json; charset=utf-8");
}

ApiError from_domain(const domain::Error& error) {
    return ApiError{status_for(error.code), std::string{domain::to_string(error.code)}, error.message, error.retryable,
                    error.details};
}

bool valid_client_request_id(const std::string& id) {
    return !id.empty() && id.size() <= 64 && std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
               c == '-';
    });
}

// The ticket in the path never reaches a log.
std::string loggable_path(const std::string& path) {
    if (path.rfind(ticket_route, 0) == 0) {
        return std::string{ticket_route} + "<redacted>";
    }
    return path;
}

// Content-Disposition with an ASCII fallback and the UTF-8 name (RFC 6266, RFC 8187).
std::string disposition(std::string_view kind, const std::string& name) {
    std::string ascii;
    std::string encoded;
    static constexpr char digits[] = "0123456789ABCDEF";
    for (const char c : name) {
        const auto byte = static_cast<unsigned char>(c);
        const bool plain = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                           (byte >= '0' && byte <= '9') || byte == '.' || byte == '-' || byte == '_';
        ascii.push_back(byte >= 0x20 && byte < 0x7F && byte != '"' && byte != '\\' ? c : '_');
        if (plain) {
            encoded.push_back(c);
        } else {
            encoded.push_back('%');
            encoded.push_back(digits[byte >> 4]);
            encoded.push_back(digits[byte & 0x0F]);
        }
    }
    return std::string{kind} + "; filename=\"" + ascii + "\"; filename*=UTF-8''" + encoded;
}

// If-None-Match uses the weak comparison: W/"x" matches "x".
bool none_match(const std::string& header, const std::string& etag) {
    const auto strip = [](std::string_view tag) {
        while (!tag.empty() && (tag.front() == ' ' || tag.front() == '\t')) {
            tag.remove_prefix(1);
        }
        while (!tag.empty() && (tag.back() == ' ' || tag.back() == '\t')) {
            tag.remove_suffix(1);
        }
        if (tag.substr(0, 2) == "W/") {
            tag.remove_prefix(2);
        }
        return tag;
    };
    std::string_view rest{header};
    while (!rest.empty()) {
        const auto comma = rest.find(',');
        const auto tag = strip(rest.substr(0, comma));
        if (tag == "*" || tag == strip(etag)) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(comma + 1);
    }
    return false;
}

thread_local SteadyClock::time_point request_started;

// Response slots of media_limits: total, local files and per principal.
class Slots {
public:
    explicit Slots(application::config::MediaLimits limits) : limits_(limits) {}

    // A release function, or the error to answer with.
    domain::Result<std::shared_ptr<void>> acquire(const std::string& principal) {
        const std::lock_guard lock{mutex_};
        if (per_principal_[principal] >= limits_.per_principal) {
            return domain::Error{
                ErrorCode::caller_limit_exceeded, "too many media responses for this caller", true, {}};
        }
        if (total_ >= limits_.global || local_ >= limits_.local_files) {
            return domain::Error{ErrorCode::service_busy, "the media service is busy", true, {}};
        }
        ++total_;
        ++local_;
        ++per_principal_[principal];
        // Released exactly once, when the last owner of the guard lets go.
        return std::shared_ptr<void>{nullptr, [this, principal](void*) {
                                         const std::lock_guard release{mutex_};
                                         --total_;
                                         --local_;
                                         if (--per_principal_[principal] == 0) {
                                             per_principal_.erase(principal);
                                         }
                                     }};
    }

    [[nodiscard]] std::uint32_t active() const {
        const std::lock_guard lock{mutex_};
        return total_;
    }

private:
    application::config::MediaLimits limits_;
    mutable std::mutex mutex_;
    std::uint32_t total_{0};
    std::uint32_t local_{0};
    std::map<std::string, std::uint32_t> per_principal_;
};

} // namespace

struct MediaServer::Impl {
    MediaServerOptions options;
    MediaServerServices services;
    Slots slots;
    httplib::Server server;
    std::thread thread;
    std::atomic<std::uint16_t> port{0};
    std::atomic<bool> stopping{false};
    std::once_flag stopped;

    Impl(MediaServerOptions o, MediaServerServices s)
        : options(std::move(o)), services(std::move(s)), slots(options.limits) {
        configure();
        server.Get(R"(/media/v1/files/(fil_[A-Za-z0-9]+))",
                   [this](const httplib::Request& req, httplib::Response& res) { file(req, res); });
        server.Get(R"(/media/v1/tickets/([A-Za-z0-9_-]+))",
                   [this](const httplib::Request& req, httplib::Response& res) { ticket(req, res); });
        server.Get(R"(/media/v1/assets/(ast_[A-Za-z0-9]+))",
                   [this](const httplib::Request& req, httplib::Response& res) { asset(req, res); });
    }

    [[nodiscard]] bool origin_allowed(const std::string& origin) const {
        const auto& allowed = options.cors.allowed_origins;
        return !origin.empty() && std::find(allowed.begin(), allowed.end(), origin) != allowed.end();
    }

    void configure() {
        // Response slots stay below the workers, so rejections and preflight still get one.
        const auto workers = std::max<std::uint32_t>(options.listener.workers, 1);
        const auto queue = options.listener.connection_queue;
        server.new_task_queue = [workers, queue] { return new httplib::ThreadPool(workers, workers, queue); };
        server.set_payload_max_length(1024); // no route takes a body
        server.set_read_timeout(5, 0);
        server.set_write_timeout(static_cast<time_t>(options.stream.idle.count()), 0);

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
                res.set_header("Access-Control-Expose-Headers",
                               "Content-Length, Content-Range, Accept-Ranges, ETag, Content-Disposition, Retry-After, "
                               "X-Request-Id");
            }
            if (req.method == "OPTIONS") {
                if (!origin_allowed(origin)) {
                    send_error(res, {403, "FORBIDDEN", "origin is not allowed", false, {}}, id);
                    return httplib::Server::HandlerResponse::Handled;
                }
                res.status = 204;
                res.set_header("Access-Control-Allow-Methods", "GET, HEAD, OPTIONS");
                res.set_header("Access-Control-Allow-Headers",
                               "Authorization, Range, If-Range, If-None-Match, X-Request-Id");
                res.set_header("Access-Control-Max-Age", "600");
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });

        server.set_error_handler([this](const httplib::Request&, httplib::Response& res) {
            if (!res.body.empty() || res.status == 416) {
                return httplib::Server::HandlerResponse::Unhandled; // 416 carries no JSON audio body
            }
            auto id = res.get_header_value("X-Request-Id");
            if (id.empty()) {
                id = services.new_request_id();
                res.set_header("X-Request-Id", id);
            }
            const auto status = res.status;
            if (status == 404) {
                send_error(res, {404, "NOT_FOUND", "no such route", false, {}}, id);
            } else if (status == 405) {
                send_error(res, {405, "METHOD_NOT_ALLOWED", "method not allowed", false, {}}, id);
            } else if (status >= 500) {
                send_error(res, {500, "INTERNAL_ERROR", "internal error", false, {}}, id);
            } else {
                send_error(res, {status, "VALIDATION_FAILED", "request is invalid", false, {}}, id);
            }
            res.status = status;
            return httplib::Server::HandlerResponse::Handled;
        });

        server.set_exception_handler([this](const httplib::Request&, httplib::Response& res, std::exception_ptr) {
            auto id = res.get_header_value("X-Request-Id");
            send_error(res, {500, "INTERNAL_ERROR", "internal error", false, {}},
                       id.empty() ? services.new_request_id() : id);
        });

        server.set_logger([this](const httplib::Request& req, const httplib::Response& res) {
            if (services.on_request) {
                const auto elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - request_started);
                services.on_request(res.get_header_value("X-Request-Id"), req.method, loggable_path(req.path),
                                    res.status, elapsed);
            }
        });
    }

    // The Bearer session with the scope, or an error already written.
    std::optional<application::ports::AuthenticatedToken> bearer(const httplib::Request& req, httplib::Response& res,
                                                                 std::string_view scope) {
        const auto request_id = res.get_header_value("X-Request-Id");
        const auto header = req.get_header_value("Authorization");
        constexpr std::string_view prefix = "Bearer ";
        if (header.size() > prefix.size() && std::string_view{header}.substr(0, prefix.size()) == prefix) {
            auto found = services.authenticator->authenticate(std::string_view{header}.substr(prefix.size()));
            if (!found) {
                send_error(res, from_domain(found.error()), request_id);
                return std::nullopt;
            }
            if (found.value()) {
                if (!accounts::has_scope(found.value()->token.scopes, scope)) {
                    send_error(res,
                               {403, "FORBIDDEN", "the token lacks the " + std::string{scope} + " scope", false, {}},
                               request_id);
                    return std::nullopt;
                }
                return std::move(found).value();
            }
        }
        res.set_header("WWW-Authenticate", "Bearer");
        send_error(res, {401, "UNAUTHENTICATED", "a valid bearer token is required", false, {}}, request_id);
        return std::nullopt;
    }

    bool refuse_query(const httplib::Request& req, httplib::Response& res) {
        if (stopping) {
            send_error(res, {503, "SERVICE_BUSY", "the media service is stopping", true, {}},
                       res.get_header_value("X-Request-Id"));
            return true;
        }
        if (!req.params.empty()) {
            send_error(
                res, {400, "VALIDATION_FAILED", "request is invalid", false, {{"query", "no parameters are accepted"}}},
                res.get_header_value("X-Request-Id"));
            return true;
        }
        return false;
    }

    void file(const httplib::Request& req, httplib::Response& res) {
        if (refuse_query(req, res)) {
            return;
        }
        auto session = bearer(req, res, accounts::scope::files_read);
        if (!session) {
            return;
        }
        res.set_header("Cache-Control", "private, no-cache");
        serve(req, res, session->principal.id, req.matches[1], "attachment");
    }

    void ticket(const httplib::Request& req, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        res.set_header("Referrer-Policy", "no-referrer");
        if (refuse_query(req, res)) {
            return;
        }
        const auto request_id = res.get_header_value("X-Request-Id");
        auto grant = services.tickets->authorize(std::string{req.matches[1]});
        if (!grant) {
            send_error(res, from_domain(grant.error()), request_id);
            return;
        }
        // Revoking the token or disabling its principal ends the ticket with it.
        if (!services.grant_valid(grant->owner_id, grant->token_id)) {
            send_error(res, {404, "NOT_FOUND", "no such media ticket", false, {}}, request_id);
            return;
        }
        if (grant->kind != media::TargetKind::file) {
            send_error(res, {501, "FEATURE_UNAVAILABLE", "the online media proxy is not enabled", false, {}},
                       request_id);
            return;
        }
        serve(req, res, grant->owner_id, grant->target_id, "inline");
    }

    void asset(const httplib::Request& req, httplib::Response& res) {
        if (refuse_query(req, res)) {
            return;
        }
        if (!bearer(req, res, accounts::scope::media_read)) {
            return;
        }
        send_error(res, {501, "FEATURE_UNAVAILABLE", "the online media proxy is not enabled", false, {}},
                   res.get_header_value("X-Request-Id"));
    }

    void serve(const httplib::Request& req, httplib::Response& res, const std::string& owner,
               const std::string& file_id, std::string_view kind) {
        const auto request_id = res.get_header_value("X-Request-Id");
        // Single ranges only (api-contract.md 10.5); checked before any file work.
        if (req.method != "HEAD" && req.ranges.size() > 1) {
            send_error(res,
                       {400, "MULTIPLE_RANGES_UNSUPPORTED", "only one byte range per request is supported", false, {}},
                       request_id);
            return;
        }
        auto slot = slots.acquire(owner);
        if (!slot) {
            res.set_header("Retry-After", "1");
            send_error(res, from_domain(slot.error()), request_id);
            return;
        }
        auto opened = services.files->open(owner, file_id);
        if (!opened) {
            send_error(res, from_domain(opened.error()), request_id);
            return;
        }
        const auto& asset = opened->asset;
        const auto size = opened->file->size();
        res.set_header("ETag", asset.etag);
        res.set_header("Accept-Ranges", "bytes");
        res.set_header("Content-Disposition", disposition(kind, media::display_name(asset)));
        const auto content_type = media::content_type_of(asset.container.value_or(""));

        if (req.has_header("If-None-Match") && none_match(req.get_header_value("If-None-Match"), asset.etag)) {
            res.status = 304;
            return;
        }
        bool partial = req.method != "HEAD" && req.ranges.size() == 1;
        if (partial && req.has_header("If-Range") && req.get_header_value("If-Range") != asset.etag) {
            partial = false; // another representation: the whole current file, never a piece of it
        }
        if (partial) {
            const auto [first, last] = req.ranges.front();
            const auto total = static_cast<long long>(size);
            const bool satisfiable =
                first == -1 ? last > 0 && total > 0 : first >= 0 && first < total && (last == -1 || first <= last);
            if (!satisfiable) {
                res.status = 416;
                res.set_header("Content-Range", "bytes */" + std::to_string(size));
                return;
            }
        }
        if (size == 0) {
            res.status = 200;
            res.set_content(std::string{}, content_type);
            return;
        }
        // httplib cuts the single range from the full-length provider and writes
        // Content-Range for a 206; a 200 always sends the whole file.
        res.status = partial ? 206 : 200;
        auto file = std::shared_ptr<application::ports::ReadableFile>{std::move(opened->file)};
        const auto deadline = SteadyClock::now() + options.stream.max_stream;
        res.set_content_provider(
            static_cast<std::size_t>(size), content_type,
            [this, file, deadline](std::size_t offset, std::size_t length, httplib::DataSink& sink) {
                if (stopping || SteadyClock::now() >= deadline) {
                    return false;
                }
                std::vector<char> buffer(std::min(chunk_bytes, length));
                auto read = file->read(offset, buffer);
                if (!read || read.value() == 0) {
                    return false; // headers are out: the connection ends instead of an error body
                }
                return sink.write(buffer.data(), read.value());
            },
            [file, slot = std::move(slot).value()](bool) mutable {
                slot.reset(); // a finished, failed or disconnected response frees its slot
            });
    }
};

MediaServer::MediaServer(MediaServerOptions options, MediaServerServices services)
    : impl_(std::make_unique<Impl>(std::move(options), std::move(services))) {}

MediaServer::~MediaServer() {
    stop();
}

domain::Result<std::uint16_t> MediaServer::bind() {
    const auto& listener = impl_->options.listener;
    int bound = -1;
    if (listener.port == 0) {
        bound = impl_->server.bind_to_any_port(listener.host);
    } else if (impl_->server.bind_to_port(listener.host, listener.port)) {
        bound = listener.port;
    }
    if (bound <= 0) {
        domain::Error error{ErrorCode::service_busy, "the media listener could not bind", false, {}};
        error.details.push_back(
            {"media", listener.host + ":" + std::to_string(listener.port) + " is in use or not available"});
        return error;
    }
    impl_->port = static_cast<std::uint16_t>(bound);
    return impl_->port.load();
}

void MediaServer::start() {
    impl_->thread = std::thread([this] { impl_->server.listen_after_bind(); });
    impl_->server.wait_until_ready();
}

void MediaServer::stop() {
    if (!impl_) {
        return;
    }
    std::call_once(impl_->stopped, [this] {
        impl_->stopping = true; // content providers end at their next chunk
        impl_->server.stop();
        if (impl_->thread.joinable()) {
            impl_->thread.join();
        }
    });
}

std::uint16_t MediaServer::port() const noexcept {
    return impl_->port.load();
}

std::uint32_t MediaServer::active() const noexcept {
    return impl_->slots.active();
}

} // namespace music_service::media_server
