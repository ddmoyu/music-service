#include "infrastructure/transport/httplib_transport.hpp"

#include "application/transport/http_rules.hpp"
#include "application/transport/url.hpp"
#include "infrastructure/filesystem/files.hpp"

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

#include <algorithm>
#include <atomic>
#include <stop_token>
#include <utility>

namespace music_service::infrastructure::transport {

namespace ports = application::ports;
using application::transport::Url;
using application::transport::transport_error;

namespace {

using Clock = std::chrono::steady_clock;

// A redirect body is read only to keep the response well-formed; never passed on.
constexpr std::size_t max_redirect_body = 64 * 1024;
constexpr std::size_t max_ca_bundle_bytes = 4 * 1024 * 1024;

const char* method_name(ports::HttpMethod method) {
    switch (method) {
    case ports::HttpMethod::head:
        return "HEAD";
    case ports::HttpMethod::post:
        return "POST";
    case ports::HttpMethod::get:
        break;
    }
    return "GET";
}

domain::Error timeout_error(std::string category, std::string message) {
    domain::Error error{domain::ErrorCode::upstream_timeout, std::move(message), true, {}};
    error.details.push_back({"transport", std::move(category)});
    return error;
}

domain::Error map_error(httplib::Error error) {
    switch (error) {
    case httplib::Error::Connection:
    case httplib::Error::ProxyConnection:
        return transport_error("connect", "could not connect to the upstream", true);
    case httplib::Error::ConnectionTimeout:
        return timeout_error("connect_timeout", "connecting to the upstream timed out");
    case httplib::Error::Timeout:
        return timeout_error("timeout", "the upstream did not respond in time");
    case httplib::Error::Read:
    case httplib::Error::Write:
    case httplib::Error::ConnectionClosed:
        return transport_error("connection_lost", "the upstream connection was interrupted", true);
    case httplib::Error::SSLConnection:
        return transport_error("tls_handshake", "TLS handshake with the upstream failed", false);
    case httplib::Error::SSLLoadingCerts:
        return transport_error("tls_trust_store", "trusted certificates could not be loaded", false);
    case httplib::Error::SSLServerVerification:
        return transport_error("tls_certificate", "the upstream certificate is not trusted", false);
    case httplib::Error::SSLServerHostnameVerification:
        return transport_error("tls_hostname", "the upstream certificate does not match the host", false);
    case httplib::Error::ExceedMaxPayloadSize:
        return transport_error("response_too_large", "the upstream response exceeds the limit", false);
    default:
        break;
    }
    return transport_error("protocol", "the upstream response could not be processed", false);
}

bool is_user_agent(std::string_view name) noexcept {
    constexpr std::string_view wanted = "user-agent";
    return name.size() == wanted.size() &&
           std::equal(name.begin(), name.end(), wanted.begin(), [](char a, char b) {
               return (a >= 'A' && a <= 'Z' ? static_cast<char>(a - 'A' + 'a') : a) == b;
           });
}

// Why a hop ended early, as decided by our own callbacks.
struct HopState {
    std::optional<std::string> location; // set for a redirect response
    std::size_t redirect_bytes{0};
    bool consumer_stopped{false};
    bool canceled{false};
    bool deadline{false};
    bool too_large{false};
};

} // namespace

struct HttplibTransport::Sink {
    bool bounded{false};
    std::size_t limit{0};
    std::string body;
    const ports::HeadConsumer* on_head{nullptr};
    const ports::ChunkConsumer* on_chunk{nullptr};
};

domain::Result<std::shared_ptr<HttplibTransport>> HttplibTransport::create(HttplibTransportOptions options) {
    std::string pem;
    if (options.ca_bundle) {
        auto loaded = filesystem::read_file(*options.ca_bundle, max_ca_bundle_bytes);
        if (!loaded) {
            return std::move(loaded).error();
        }
        pem = std::move(loaded).value();
        // Parse once now so a broken bundle fails at startup, not on the first request.
        auto* store = httplib::tls::create_ca_store(pem.c_str(), pem.size());
        if (store == nullptr) {
            return domain::Error::validation("ca_bundle", "is not a PEM certificate bundle");
        }
        httplib::tls::free_ca_store(store);
    }
    return std::shared_ptr<HttplibTransport>{new HttplibTransport(std::move(options), std::move(pem))};
}

HttplibTransport::HttplibTransport(HttplibTransportOptions options, std::string ca_pem)
    : options_(std::move(options)), ca_pem_(std::move(ca_pem)) {}

domain::Result<ports::HttpResponse> HttplibTransport::request(const ports::HttpRequest& request,
                                                              const ports::UpstreamLease& lease,
                                                              const ports::OperationContext& context) {
    Sink sink;
    sink.bounded = true;
    sink.limit = request.max_response_bytes == 0 ? options_.max_response_bytes : request.max_response_bytes;
    auto head = perform(request, lease, context, sink);
    if (!head) {
        return std::move(head).error();
    }
    return ports::HttpResponse{std::move(head).value(), std::move(sink.body)};
}

domain::Result<ports::HttpResponseHead> HttplibTransport::receive(const ports::HttpRequest& request,
                                                                  const ports::UpstreamLease& lease,
                                                                  const ports::OperationContext& context,
                                                                  const ports::HeadConsumer& on_head,
                                                                  const ports::ChunkConsumer& on_chunk) {
    Sink sink;
    sink.on_head = &on_head;
    sink.on_chunk = &on_chunk;
    return perform(request, lease, context, sink);
}

domain::Result<ports::HttpResponseHead> HttplibTransport::perform(const ports::HttpRequest& request,
                                                                  const ports::UpstreamLease& lease,
                                                                  const ports::OperationContext& context,
                                                                  Sink& sink) {
    ports::HttpRequest current = request;
    while (true) {
        auto url = application::transport::check_request(current);
        if (!url) {
            return std::move(url).error();
        }
        // No lease, no network; and a lease covers exactly one origin.
        if (!lease.active() || lease.request().origin != url->origin()) {
            return domain::Error::validation("lease", "does not cover the target origin");
        }
        if (context.stop.stop_requested()) {
            return domain::Error{domain::ErrorCode::canceled, "the operation was canceled", false, {}};
        }
        const auto now = Clock::now();
        if (context.expired(now)) {
            return domain::Error{domain::ErrorCode::deadline_exceeded, "the operation deadline passed", false, {}};
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(context.deadline - now);

        httplib::Client client(url->origin());
        client.set_follow_location(false);
        client.set_keep_alive(false);
        client.set_path_encode(false); // the target is already encoded and checked
        client.enable_server_certificate_verification(true);
        client.enable_server_hostname_verification(true);
        if (!ca_pem_.empty()) {
            client.set_ca_cert_store(httplib::tls::create_ca_store(ca_pem_.c_str(), ca_pem_.size()));
        }
        client.set_connection_timeout(std::min<std::chrono::milliseconds>(options_.connect_timeout, remaining));
        client.set_read_timeout(std::min<std::chrono::milliseconds>(options_.idle_timeout, remaining));
        client.set_write_timeout(std::min<std::chrono::milliseconds>(options_.idle_timeout, remaining));
        client.set_max_timeout(remaining);

        HopState state;
        ports::HttpResponseHead head;
        head.final_url = url->str();
        sink.body.clear();

        httplib::Request req;
        req.method = method_name(current.method);
        req.path = url->target();
        bool has_user_agent = false;
        for (const auto& header : current.headers) {
            has_user_agent = has_user_agent || is_user_agent(header.name);
            req.headers.emplace(header.name, header.value);
        }
        for (const auto& header : current.secret_headers) {
            req.headers.emplace(header.name, header.value);
        }
        if (!has_user_agent) {
            req.headers.emplace("User-Agent", options_.user_agent);
        }
        req.body = current.body;

        req.response_handler = [&](const httplib::Response& response) {
            head.status = response.status;
            head.headers.clear();
            for (const auto& [name, value] : response.headers) {
                head.headers.push_back({name, value});
            }
            if (application::transport::is_redirect_status(response.status) && response.has_header("Location")) {
                state.location = response.get_header_value("Location");
                return true; // read and discard the small body
            }
            if (sink.bounded) {
                if (response.has_header("Content-Length") &&
                    response.get_header_value_u64("Content-Length") > sink.limit) {
                    state.too_large = true;
                    return false;
                }
                return true;
            }
            if (!(*sink.on_head)(head)) {
                state.consumer_stopped = true;
                return false;
            }
            return true;
        };
        req.content_receiver = [&](const char* data, std::size_t length, std::size_t, std::size_t) {
            if (context.stop.stop_requested()) {
                state.canceled = true;
                return false;
            }
            if (context.expired(Clock::now())) {
                state.deadline = true;
                return false;
            }
            if (state.location) {
                state.redirect_bytes += length;
                if (state.redirect_bytes > max_redirect_body) {
                    state.too_large = true;
                    return false;
                }
                return true;
            }
            if (sink.bounded) {
                if (length > sink.limit - sink.body.size()) {
                    state.too_large = true;
                    return false;
                }
                sink.body.append(data, length);
                return true;
            }
            if (!(*sink.on_chunk)(std::span<const char>{data, length})) {
                state.consumer_stopped = true;
                return false;
            }
            return true;
        };

        httplib::Response response;
        auto error = httplib::Error::Success;
        bool ok = false;
        {
            // Aborts the socket from whichever thread requests the stop; send()
            // returns once the connection is really closed.
            std::stop_callback abort_on_stop{context.stop, [&client] { client.stop(); }};
            ok = client.send(req, response, error);
        }
        if (!ok) {
            if (state.consumer_stopped) {
                head.stopped_by_consumer = true;
                return head;
            }
            if (state.canceled || context.stop.stop_requested()) {
                return domain::Error{domain::ErrorCode::canceled, "the operation was canceled", false, {}};
            }
            if (state.too_large) {
                return transport_error("response_too_large", "the upstream response exceeds the limit", false);
            }
            if (state.deadline || context.expired(Clock::now())) {
                return domain::Error{domain::ErrorCode::deadline_exceeded, "the operation deadline passed", false,
                                     {}};
            }
            return map_error(error);
        }

        if (!state.location) {
            return head;
        }
        auto next = application::transport::next_redirect(current, url.value(), head.status, *state.location,
                                                          options_.redirect_max);
        if (!next) {
            return std::move(next).error();
        }
        auto next_url = Url::parse(next->url);
        if (!next_url) {
            return std::move(next_url).error();
        }
        if (next_url->origin() == url->origin()) {
            current = std::move(next).value(); // same origin: same lease, next hop
            continue;
        }
        head.redirect = std::make_shared<const ports::HttpRequest>(std::move(next).value());
        return head;
    }
}

} // namespace music_service::infrastructure::transport
