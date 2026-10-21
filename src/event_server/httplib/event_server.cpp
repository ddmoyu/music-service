#include "event_server/httplib/event_server.hpp"

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
#include <mutex>
#include <thread>
#include <utility>

namespace music_service::event_server {

namespace {

using json = nlohmann::json;
namespace events = application::events;
namespace accounts = application::accounts;
using domain::ErrorCode;
using SteadyClock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// How long a stream waits before the writer loop checks the peer again.
constexpr auto wait_slice = 500ms;

std::string utc(std::chrono::system_clock::time_point time) {
    using namespace std::chrono;
    const auto millis = time_point_cast<milliseconds>(time);
    const auto day = floor<days>(millis);
    const year_month_day date{day};
    const hh_mm_ss clock{millis - day};
    char buffer[32];
    const auto two = [](char* out, long long value) {
        out[0] = static_cast<char>('0' + value / 10 % 10);
        out[1] = static_cast<char>('0' + value % 10);
    };
    const auto year = static_cast<int>(date.year());
    buffer[0] = static_cast<char>('0' + year / 1000 % 10);
    buffer[1] = static_cast<char>('0' + year / 100 % 10);
    two(buffer + 2, year % 100);
    buffer[4] = '-';
    two(buffer + 5, static_cast<unsigned>(date.month()));
    buffer[7] = '-';
    two(buffer + 8, static_cast<unsigned>(date.day()));
    buffer[10] = 'T';
    two(buffer + 11, clock.hours().count());
    buffer[13] = ':';
    two(buffer + 14, clock.minutes().count());
    buffer[16] = ':';
    two(buffer + 17, clock.seconds().count());
    buffer[19] = '.';
    const auto ms = clock.subseconds().count();
    buffer[20] = static_cast<char>('0' + ms / 100);
    two(buffer + 21, ms % 100);
    buffer[23] = 'Z';
    return std::string{buffer, 24};
}

std::string message(std::string_view event, const json& data) {
    std::string out{"event: "};
    out += event;
    out += "\ndata: ";
    out += data.dump(); // one line: JSON escapes control characters
    out += "\n\n";
    return out;
}

struct ApiError {
    int status{500};
    std::string code;
    std::string message;
    bool retryable{false};
    std::vector<domain::ErrorDetail> details;
};

int status_for(ErrorCode code) {
    switch (code) {
    case ErrorCode::caller_limit_exceeded:
        return 429;
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
    return ApiError{status_for(error.code), std::string{domain::to_string(error.code)}, error.message,
                    error.retryable, error.details};
}

bool valid_client_request_id(const std::string& id) {
    return !id.empty() && id.size() <= 64 && std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
               c == '-';
    });
}

thread_local SteadyClock::time_point request_started;

// State of one open stream, shared by the content provider and its releaser.
struct Stream {
    std::shared_ptr<events::Subscription> subscription;
    std::string token; // re-checked at heartbeats; never logged
    SteadyClock::time_point deadline;
    SteadyClock::time_point next_heartbeat;
    bool ready_sent{false};
};

} // namespace

std::string encode_task_event(const events::TaskEvent& event) {
    json progress = nullptr;
    if (event.progress_json) {
        progress = json::parse(*event.progress_json, nullptr, false);
        if (progress.is_discarded()) {
            progress = nullptr;
        }
    }
    return message("task.updated", json{{"task_id", event.task_id},
                                        {"version", event.version},
                                        {"status", domain::to_string(event.status)},
                                        {"progress", std::move(progress)},
                                        {"updated_at", utc(event.updated_at)}});
}

struct EventServer::Impl {
    EventServerOptions options;
    EventServerServices services;
    httplib::Server server;
    std::thread thread;
    std::atomic<std::uint16_t> port{0};
    std::atomic<bool> stopping{false};
    std::once_flag stopped;

    Impl(EventServerOptions o, EventServerServices s) : options(std::move(o)), services(std::move(s)) {
        configure();
        server.Get("/events/v1/downloads",
                   [this](const httplib::Request& req, httplib::Response& res) { downloads(req, res); });
    }

    [[nodiscard]] bool origin_allowed(const std::string& origin) const {
        const auto& allowed = options.cors.allowed_origins;
        return !origin.empty() && std::find(allowed.begin(), allowed.end(), origin) != allowed.end();
    }

    void configure() {
        // One worker per stream: the subscription limits keep spare workers for
        // rejections and preflight (config validation requires two).
        const auto workers = std::max<std::uint32_t>(options.listener.workers, 1);
        const auto queue = options.listener.connection_queue;
        server.new_task_queue = [workers, queue] { return new httplib::ThreadPool(workers, workers, queue); };
        server.set_payload_max_length(1024); // the route takes no body
        server.set_read_timeout(5, 0);
        server.set_write_timeout(static_cast<time_t>(options.stream.write_idle.count()), 0);
        server.set_keep_alive_max_count(1);

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
                res.set_header("Access-Control-Expose-Headers", "Retry-After, X-Request-Id");
            }
            if (req.method == "OPTIONS") {
                if (!origin_allowed(origin)) {
                    send_error(res, {403, "FORBIDDEN", "origin is not allowed", false, {}}, id);
                    return httplib::Server::HandlerResponse::Handled;
                }
                res.status = 204;
                res.set_header("Access-Control-Allow-Methods", "GET, OPTIONS");
                res.set_header("Access-Control-Allow-Headers", "Authorization, X-Request-Id");
                res.set_header("Access-Control-Max-Age", "600");
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });

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
                services.on_request(res.get_header_value("X-Request-Id"), req.method, req.path, res.status, elapsed);
            }
        });
    }

    // Checks the token again; false once it is revoked or its principal disabled.
    bool still_authorized(const std::string& token) const {
        auto found = services.authenticator->authenticate(token);
        return found && found.value() && accounts::has_scope(found.value()->token.scopes, accounts::scope::downloads_read);
    }

    void downloads(const httplib::Request& req, httplib::Response& res) {
        const auto request_id = res.get_header_value("X-Request-Id");
        res.set_header("Cache-Control", "no-store");
        if (stopping) {
            send_error(res, {503, "SERVICE_BUSY", "the event stream is stopping", true, {}}, request_id);
            return;
        }
        if (!req.params.empty()) {
            send_error(res, {400, "VALIDATION_FAILED", "request is invalid", false, {{"query", "no parameters are accepted"}}},
                       request_id);
            return;
        }
        const auto header = req.get_header_value("Authorization");
        constexpr std::string_view bearer = "Bearer ";
        std::optional<application::ports::AuthenticatedToken> session;
        std::string token;
        if (header.size() > bearer.size() && std::string_view{header}.substr(0, bearer.size()) == bearer) {
            token = header.substr(bearer.size());
            auto found = services.authenticator->authenticate(token);
            if (!found) {
                send_error(res, from_domain(found.error()), request_id);
                return;
            }
            session = std::move(found).value();
        }
        if (!session) {
            res.set_header("WWW-Authenticate", "Bearer");
            send_error(res, {401, "UNAUTHENTICATED", "a valid bearer token is required", false, {}}, request_id);
            return;
        }
        if (!accounts::has_scope(session->token.scopes, accounts::scope::downloads_read)) {
            send_error(res, {403, "FORBIDDEN", "the token lacks the downloads:read scope", false, {}}, request_id);
            return;
        }
        auto subscription = services.hub->subscribe(session->principal.id);
        if (!subscription) {
            if (subscription.error().code == ErrorCode::caller_limit_exceeded) {
                res.set_header("Retry-After", "5");
            }
            send_error(res, from_domain(subscription.error()), request_id);
            return;
        }

        auto stream = std::make_shared<Stream>();
        stream->subscription = std::move(subscription).value();
        stream->token = std::move(token);
        const auto now = SteadyClock::now();
        stream->deadline = now + options.stream.max_connection;
        stream->next_heartbeat = now + options.stream.heartbeat;

        res.status = 200;
        res.set_header("X-Accel-Buffering", "no");
        res.set_chunked_content_provider(
            "text/event-stream",
            [this, stream](std::size_t, httplib::DataSink& sink) { return pump(*stream, sink); },
            [stream](bool) { stream->subscription.reset(); }); // frees the slot at once
    }

    // One pass of the writer loop: a message, a heartbeat, or a short wait so the
    // library can notice a closed peer. sink.done() ends the stream normally.
    bool pump(Stream& stream, httplib::DataSink& sink) {
        if (!stream.ready_sent) {
            stream.ready_sent = true;
            const auto ready = message("stream.ready", json{{"instance_id", options.instance_id}, {"sync_required", true}});
            return sink.write(ready.data(), ready.size());
        }
        const auto now = SteadyClock::now();
        if (now >= stream.deadline) {
            sink.done(); // the client reconnects and resynchronizes
            return true;
        }
        if (now >= stream.next_heartbeat) {
            stream.next_heartbeat = now + options.stream.heartbeat;
            if (!still_authorized(stream.token)) {
                stream.subscription->close(events::CloseReason::revoked);
                sink.done();
                return true;
            }
            constexpr std::string_view heartbeat = ": heartbeat\n\n";
            return sink.write(heartbeat.data(), heartbeat.size());
        }
        const auto until = std::min({stream.deadline, stream.next_heartbeat, now + wait_slice});
        auto next = stream.subscription->wait(until);
        if (next.closed) {
            sink.done();
            return true;
        }
        if (next.message) {
            return sink.write(next.message->data(), next.message->size());
        }
        return true;
    }
};

EventServer::EventServer(EventServerOptions options, EventServerServices services)
    : impl_(std::make_unique<Impl>(std::move(options), std::move(services))) {}

EventServer::~EventServer() { stop(); }

domain::Result<std::uint16_t> EventServer::bind() {
    const auto& listener = impl_->options.listener;
    int bound = -1;
    if (listener.port == 0) {
        bound = impl_->server.bind_to_any_port(listener.host);
    } else if (impl_->server.bind_to_port(listener.host, listener.port)) {
        bound = listener.port;
    }
    if (bound <= 0) {
        domain::Error error{ErrorCode::service_busy, "the events listener could not bind", false, {}};
        error.details.push_back({"events", listener.host + ":" + std::to_string(listener.port) +
                                               " is in use or not available"});
        return error;
    }
    impl_->port = static_cast<std::uint16_t>(bound);
    return impl_->port.load();
}

void EventServer::start() {
    impl_->thread = std::thread([this] { impl_->server.listen_after_bind(); });
    impl_->server.wait_until_ready();
}

void EventServer::stop() {
    if (!impl_) {
        return;
    }
    std::call_once(impl_->stopped, [this] {
        impl_->stopping = true;
        // Wake every stream so its worker returns before the pool is joined.
        impl_->services.hub->close_all(events::CloseReason::shutdown);
        impl_->server.stop();
        if (impl_->thread.joinable()) {
            impl_->thread.join();
        }
    });
}

std::uint16_t EventServer::port() const noexcept { return impl_->port.load(); }

} // namespace music_service::event_server
