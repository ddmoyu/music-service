// Upstream HTTP(S) transport against local httplib servers: TLS verification,
// redirects with per-origin admission, streaming, size limits, interruption and
// cancellation. Certificates come from tests/fixtures/tls; no outside network.

#include "../unit/domain/test_support.hpp"

#include "application/transport/upstream_client.hpp"
#include "infrastructure/clock.hpp"
#include "infrastructure/config/config_loader.hpp"
#include "infrastructure/transport/httplib_transport.hpp"
#include "runtime/admission/admission_ledger.hpp"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4702)
#endif
#include <httplib.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stop_token>
#include <string>
#include <thread>

using namespace music_service;
using namespace std::chrono_literals;
namespace ports = application::ports;
using application::transport::PendingHop;
using application::transport::UpstreamClient;
using infrastructure::transport::HttplibTransport;
using infrastructure::transport::HttplibTransportOptions;

namespace {

const std::filesystem::path fixtures{MUSIC_SERVICE_TEST_FIXTURES};

std::string tls_file(const char* name) { return (fixtures / "tls" / name).string(); }

// What a server saw, for checking which headers crossed which hop.
struct Seen {
    std::mutex mutex;
    std::map<std::string, std::string> cookie_by_path; // "" when absent
    std::map<std::string, std::string> method_by_path;
    std::map<std::string, std::string> body_by_path;
    int requests{0};

    void record(const httplib::Request& req) {
        const std::lock_guard lock{mutex};
        cookie_by_path[req.path] = req.get_header_value("Cookie");
        method_by_path[req.path] = req.method;
        body_by_path[req.path] = req.body;
        ++requests;
    }
    std::string cookie(const std::string& path) {
        const std::lock_guard lock{mutex};
        const auto it = cookie_by_path.find(path);
        return it == cookie_by_path.end() ? "<none>" : it->second;
    }
    bool saw(const std::string& path) {
        const std::lock_guard lock{mutex};
        return method_by_path.count(path) != 0;
    }
};

// A local server on a random loopback port, plain or TLS.
class TestServer {
public:
    enum class Kind { plain, trusted_tls, rogue_tls };

    explicit TestServer(Kind kind) {
        if (kind == Kind::plain) {
            server_ = std::make_unique<httplib::Server>();
        } else {
            const auto cert = tls_file(kind == Kind::trusted_tls ? "server.pem" : "rogue.pem");
            const auto key = tls_file(kind == Kind::trusted_tls ? "server.key" : "rogue.key");
            server_ = std::make_unique<httplib::SSLServer>(cert.c_str(), key.c_str());
        }
        REQUIRE(server_->is_valid());
        scheme_ = kind == Kind::plain ? "http" : "https";
        server_->set_pre_routing_handler([this](const httplib::Request& req, httplib::Response&) {
            seen.record(req);
            return httplib::Server::HandlerResponse::Unhandled;
        });
    }
    ~TestServer() { stop(); }

    httplib::Server& routes() { return *server_; }

    // Dual-stack, so "localhost" (often ::1 first) and 127.0.0.1 both connect at once;
    // a refused ::1 attempt costs about two seconds on Windows.
    void start() {
        server_->set_ipv6_v6only(false);
        port_ = server_->bind_to_any_port("::");
        REQUIRE(port_ > 0);
        thread_ = std::thread([this] { server_->listen_after_bind(); });
        server_->wait_until_ready();
    }
    void stop() {
        closing.store(true);
        if (thread_.joinable()) {
            server_->stop();
            thread_.join();
        }
    }

    // Through "localhost", which the test certificate names.
    [[nodiscard]] std::string url(const std::string& path) const {
        return scheme_ + "://localhost:" + std::to_string(port_) + path;
    }
    [[nodiscard]] std::string ip_url(const std::string& path) const {
        return scheme_ + "://127.0.0.1:" + std::to_string(port_) + path;
    }
    [[nodiscard]] std::string origin() const { return scheme_ + "://localhost:" + std::to_string(port_); }
    [[nodiscard]] int port() const { return port_; }

    Seen seen;
    std::atomic<bool> closing{false};

private:
    std::unique_ptr<httplib::Server> server_;
    std::string scheme_;
    int port_{0};
    std::thread thread_;
};

// A loopback listener that hands each accepted socket to a script, for upstreams
// httplib's server cannot imitate: a TCP peer that never speaks TLS, or one that
// sends its response head too slowly. The script ends when closing is set.
class RawServer {
public:
    using Script = std::function<void(socket_t, const std::atomic<bool>& closing)>;

    explicit RawServer(Script script) : script_(std::move(script)) {
        listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        REQUIRE(listener_ != INVALID_SOCKET);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(::bind(listener_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
        REQUIRE(::listen(listener_, 4) == 0);
        socklen_t length = sizeof(addr);
        REQUIRE(::getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &length) == 0);
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] {
            while (!closing_.load()) {
                const socket_t peer = ::accept(listener_, nullptr, nullptr);
                if (peer == INVALID_SOCKET) {
                    return;
                }
                script_(peer, closing_);
                close(peer);
            }
        });
    }
    ~RawServer() {
        closing_.store(true);
#ifndef _WIN32
        ::shutdown(listener_, SHUT_RDWR); // close() alone does not wake accept on Linux
#endif
        close(listener_); // wakes the blocked accept
        thread_.join();
    }

    [[nodiscard]] int port() const { return port_; }

private:
    static void close(socket_t sock) {
#ifdef _WIN32
        ::closesocket(sock);
#else
        ::close(sock);
#endif
    }

    Script script_;
    socket_t listener_{INVALID_SOCKET};
    int port_{0};
    std::atomic<bool> closing_{false};
    std::thread thread_;
};

void send_text(socket_t sock, std::string_view text) {
#ifdef _WIN32
    (void)::send(sock, text.data(), static_cast<int>(text.size()), 0);
#else
    (void)::send(sock, text.data(), text.size(), 0);
#endif
}

// Waits without reading, so the request stays unanswered.
void hold(const std::atomic<bool>& closing, std::chrono::milliseconds at_most) {
    const auto until = std::chrono::steady_clock::now() + at_most;
    while (!closing.load() && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(10ms);
    }
}

std::string category(const domain::Error& error) {
    for (const auto& detail : error.details) {
        if (detail.field == "transport") {
            return detail.reason;
        }
    }
    return std::string{domain::to_string(error.code)};
}

struct Harness {
    explicit Harness(runtime::AdmissionLimits limits = {}, bool trust_test_ca = true,
                     std::chrono::seconds connect_timeout = 3s) {
        HttplibTransportOptions options;
        options.connect_timeout = connect_timeout;
        options.max_response_bytes = 1024 * 1024;
        options.redirect_max = 3;
        if (trust_test_ca) {
            options.ca_bundle = fixtures / "tls" / "ca.pem";
        }
        transport = test::unwrap(HttplibTransport::create(options));
        ledger = std::make_shared<runtime::AdmissionLedger>(limits, clock);
        client = std::make_unique<UpstreamClient>(ledger, transport, clock);
    }

    ports::AdmissionRequest base() const {
        ports::AdmissionRequest request{"prn_a", test::source("mock"), "search", ports::TrafficClass::short_request,
                                        std::nullopt, "", "", {}};
        return request;
    }

    static ports::OperationContext context(std::chrono::milliseconds budget = 5000ms, std::stop_token stop = {}) {
        return {std::chrono::steady_clock::now() + budget, std::move(stop)};
    }

    domain::Result<application::transport::HopResult<ports::HttpResponse>>
    fetch(ports::HttpRequest request, ports::OperationContext ctx = context()) {
        if (request.operation.empty()) {
            request.operation = "search";
        }
        return client->fetch(std::move(request), base(), ctx);
    }

    ports::HttpResponse fetched(ports::HttpRequest request) {
        auto result = test::unwrap(fetch(std::move(request)));
        REQUIRE(std::holds_alternative<ports::HttpResponse>(result));
        return std::get<ports::HttpResponse>(std::move(result));
    }

    std::shared_ptr<infrastructure::SystemClock> clock = std::make_shared<infrastructure::SystemClock>();
    std::shared_ptr<HttplibTransport> transport;
    std::shared_ptr<runtime::AdmissionLedger> ledger;
    std::unique_ptr<UpstreamClient> client;
};

ports::HttpRequest get(std::string url) {
    ports::HttpRequest request;
    request.url = std::move(url);
    return request;
}

} // namespace

TEST_CASE("HTTPS GET and POST reach a trusted upstream and return the lease", "[transport]") {
    TestServer server{TestServer::Kind::trusted_tls};
    server.routes().Get("/hello", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"ok":true})", "application/json");
    });
    server.routes().Post("/echo", [](const httplib::Request& req, httplib::Response& res) {
        res.set_content(req.get_header_value("Content-Type") + "|" + req.body, "text/plain");
    });
    server.start();
    Harness h;

    const auto hello = h.fetched(get(server.url("/hello")));
    CHECK(hello.head.status == 200);
    CHECK(hello.body == R"({"ok":true})");
    CHECK(hello.head.header("content-type") == "application/json");
    CHECK(hello.head.final_url == server.url("/hello"));

    auto post = get(server.url("/echo"));
    post.method = ports::HttpMethod::post;
    post.body = "a=1&b=2";
    post.headers.push_back({"Content-Type", "application/x-www-form-urlencoded"});
    post.secret_headers.push_back({"Cookie", "session=abc"});
    const auto echoed = h.fetched(post);
    CHECK(echoed.body == "application/x-www-form-urlencoded|a=1&b=2");
    CHECK(server.seen.cookie("/echo") == "session=abc");

    // HTTP error statuses are responses, not transport errors.
    const auto missing = h.fetched(get(server.url("/nope")));
    CHECK(missing.head.status == 404);

    CHECK(h.ledger->usage().total == 0);
}

TEST_CASE("TLS rejects untrusted chains and mismatched host names", "[transport]") {
    TestServer trusted{TestServer::Kind::trusted_tls};
    trusted.routes().Get("/x", [](const httplib::Request&, httplib::Response& res) { res.set_content("x", "text/plain"); });
    trusted.start();
    TestServer rogue{TestServer::Kind::rogue_tls};
    rogue.routes().Get("/x", [](const httplib::Request&, httplib::Response& res) { res.set_content("x", "text/plain"); });
    rogue.start();
    Harness h;

    const auto rogue_result = h.fetch(get(rogue.url("/x")));
    REQUIRE_FALSE(rogue_result.has_value());
    CHECK(rogue_result.error().code == domain::ErrorCode::upstream_failed);
    CHECK(category(rogue_result.error()).rfind("tls_", 0) == 0);
    CHECK_FALSE(rogue_result.error().retryable);

    // The certificate names localhost only.
    const auto by_ip = h.fetch(get(trusted.ip_url("/x")));
    REQUIRE_FALSE(by_ip.has_value());
    CHECK(category(by_ip.error()).rfind("tls_", 0) == 0);

    // Without the test CA the system roots do not trust it either.
    Harness system_roots{{}, false};
    const auto untrusted = system_roots.fetch(get(trusted.url("/x")));
    REQUIRE_FALSE(untrusted.has_value());
    CHECK(category(untrusted.error()).rfind("tls_", 0) == 0);

    CHECK_FALSE(trusted.seen.saw("/x"));
    CHECK_FALSE(rogue.seen.saw("/x"));
    CHECK(h.ledger->usage().total == 0);
}

TEST_CASE("plaintext HTTP needs a registered endpoint and never carries secrets", "[transport]") {
    TestServer plain{TestServer::Kind::plain};
    plain.routes().Get("/r.s", [](const httplib::Request&, httplib::Response& res) { res.set_content("ok", "text/plain"); });
    plain.start();
    Harness h;
    const ports::PlaintextEndpoint endpoint{"127.0.0.1", static_cast<std::uint16_t>(plain.port()), "/r.s", "search",
                                            false};

    auto request = get(plain.ip_url("/r.s?all=x"));
    CHECK(h.fetch(request).error().code == domain::ErrorCode::validation_failed);
    CHECK(plain.seen.requests == 0);

    request.plaintext_endpoints.push_back(endpoint);
    CHECK(h.fetched(request).body == "ok");

    request.secret_headers.push_back({"Cookie", "s=1"});
    CHECK(h.fetch(request).error().code == domain::ErrorCode::validation_failed);
    CHECK(plain.seen.requests == 1);
}

TEST_CASE("redirects stay on the lease within an origin and re-admit across origins", "[transport]") {
    TestServer a{TestServer::Kind::trusted_tls};
    TestServer b{TestServer::Kind::trusted_tls};
    TestServer plain{TestServer::Kind::plain};
    Harness h;
    std::set<std::string> origins_during_b;
    a.routes().Get("/hop", [](const httplib::Request&, httplib::Response& res) { res.set_redirect("/final", 302); });
    a.routes().Get("/final", [](const httplib::Request&, httplib::Response& res) { res.set_content("final", "text/plain"); });
    a.routes().Get("/away", [&](const httplib::Request&, httplib::Response& res) { res.set_redirect(b.url("/landing"), 302); });
    a.routes().Get("/loop", [](const httplib::Request&, httplib::Response& res) { res.set_redirect("/loop", 302); });
    a.routes().Get("/down", [&](const httplib::Request&, httplib::Response& res) {
        res.set_redirect(plain.ip_url("/cdn/a.mp3"), 302);
    });
    a.routes().Post("/form", [](const httplib::Request&, httplib::Response& res) { res.set_redirect("/done", 303); });
    a.routes().Get("/done", [](const httplib::Request&, httplib::Response& res) { res.set_content("done", "text/plain"); });
    b.routes().Get("/landing", [&](const httplib::Request&, httplib::Response& res) {
        for (const auto& [origin, count] : h.ledger->usage().origins) {
            if (count > 0) {
                origins_during_b.insert(origin);
            }
        }
        res.set_content("landed", "text/plain");
    });
    plain.routes().Get("/cdn/a.mp3", [](const httplib::Request&, httplib::Response& res) { res.set_content("mp3", "audio/mpeg"); });
    a.start();
    b.start();
    plain.start();

    // Same origin: followed on one lease, the cookie kept.
    auto hop = get(a.url("/hop"));
    hop.secret_headers.push_back({"Cookie", "s=1"});
    const auto final_response = h.fetched(hop);
    CHECK(final_response.body == "final");
    CHECK(final_response.head.final_url == a.url("/final"));
    CHECK(a.seen.cookie("/final") == "s=1");

    // A42: another origin gets its own lease after the first is returned; no cookie.
    auto away = get(a.url("/away"));
    away.secret_headers.push_back({"Cookie", "s=1"});
    const auto landed = h.fetched(away);
    CHECK(landed.body == "landed");
    CHECK(landed.head.final_url == b.url("/landing"));
    CHECK(b.seen.cookie("/landing").empty());
    CHECK(origins_during_b == std::set<std::string>{b.origin()});

    // 303 after POST continues as GET without the body.
    auto form = get(a.url("/form"));
    form.method = ports::HttpMethod::post;
    form.body = "x=1";
    form.headers.push_back({"Content-Type", "application/x-www-form-urlencoded"});
    CHECK(h.fetched(form).body == "done");
    CHECK(a.seen.method_by_path["/done"] == "GET");
    CHECK(a.seen.body_by_path["/done"].empty());

    // Hop limit.
    CHECK(category(h.fetch(get(a.url("/loop"))).error()) == "too_many_redirects");

    // HTTPS to HTTP only to a registered download endpoint, without the cookie.
    auto down = get(a.url("/down"));
    down.operation = "download";
    down.secret_headers.push_back({"Cookie", "s=1"});
    CHECK(category(h.fetch(down).error()) == "redirect_rejected");
    CHECK_FALSE(plain.seen.saw("/cdn/a.mp3"));
    down.plaintext_endpoints.push_back(
        {"127.0.0.1", static_cast<std::uint16_t>(plain.port()), "/cdn/", "download", true});
    CHECK(h.fetched(down).body == "mp3");
    CHECK(plain.seen.cookie("/cdn/a.mp3").empty());

    CHECK(h.ledger->usage().total == 0);
}

TEST_CASE("a cross-origin hop without quota is handed back, not waited for", "[transport]") {
    TestServer a{TestServer::Kind::trusted_tls};
    TestServer b{TestServer::Kind::trusted_tls};
    a.routes().Get("/away", [&](const httplib::Request&, httplib::Response& res) { res.set_redirect(b.url("/landing"), 302); });
    b.routes().Get("/landing", [](const httplib::Request&, httplib::Response& res) { res.set_content("landed", "text/plain"); });
    a.start();
    b.start();
    runtime::AdmissionLimits limits;
    limits.per_origin = 1;
    Harness h{limits};

    // Someone else holds b's only origin slot.
    auto blocker_request = h.base();
    blocker_request.origin = b.origin();
    blocker_request.deadline = std::chrono::steady_clock::now() + 5s;
    auto blocker = test::unwrap(h.ledger->try_acquire(blocker_request));
    REQUIRE(std::holds_alternative<ports::UpstreamLease>(blocker));

    auto away = get(a.url("/away"));
    away.secret_headers.push_back({"Cookie", "s=1"});
    const auto started = std::chrono::steady_clock::now();
    auto result = test::unwrap(h.fetch(away));
    CHECK(std::chrono::steady_clock::now() - started < 2s);
    REQUIRE(std::holds_alternative<PendingHop>(result));
    const auto& pending = std::get<PendingHop>(result);
    CHECK(pending.next.url == b.url("/landing"));
    CHECK(pending.next.secret_headers.empty());
    CHECK(pending.next.redirects_followed == 1);
    CHECK(pending.deferred.blocked_dimension == "origin");
    CHECK_FALSE(b.seen.saw("/landing"));
    CHECK(h.ledger->usage().total == 1); // only the blocker; a's lease is back

    std::get<ports::UpstreamLease>(blocker).release();
    auto resumed = test::unwrap(h.client->fetch(pending.next, h.base(), Harness::context()));
    REQUIRE(std::holds_alternative<ports::HttpResponse>(resumed));
    CHECK(std::get<ports::HttpResponse>(resumed).body == "landed");
}

TEST_CASE("Retry-After cools the source down in the admission ledger", "[transport]") {
    TestServer server{TestServer::Kind::trusted_tls};
    server.routes().Get("/limited", [](const httplib::Request&, httplib::Response& res) {
        res.status = 429;
        res.set_header("Retry-After", "30");
    });
    server.start();
    Harness h;

    CHECK(h.fetched(get(server.url("/limited"))).head.status == 429);
    CHECK_FALSE(h.ledger->usage().cooldowns.empty());
    auto again = test::unwrap(h.fetch(get(server.url("/limited"))));
    REQUIRE(std::holds_alternative<PendingHop>(again));
    CHECK(server.seen.requests == 1);
}

TEST_CASE("Retry-After too large to add to the clock still cools down", "[transport]") {
    TestServer server{TestServer::Kind::trusted_tls};
    std::string value;
    server.routes().Get("/limited", [&](const httplib::Request&, httplib::Response& res) {
        res.status = 503;
        res.set_header("Retry-After", value);
    });
    server.start();

    for (const auto* huge : {"9223372036854775807", "99999999999999999999999"}) {
        value = huge;
        Harness h;
        CHECK(h.fetched(get(server.url("/limited"))).head.status == 503);
        CHECK(h.ledger->usage().cooldowns.size() == 1);
        REQUIRE(std::holds_alternative<PendingHop>(test::unwrap(h.fetch(get(server.url("/limited"))))));
    }
    // A sign is not delta-seconds.
    value = "-5";
    Harness h;
    CHECK(h.fetched(get(server.url("/limited"))).head.status == 503);
    CHECK(h.ledger->usage().cooldowns.empty());
}

TEST_CASE("HEAD and 204 responses keep their status and headers", "[transport]") {
    TestServer server{TestServer::Kind::trusted_tls};
    server.routes().Get("/file", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("ETag", "\"v1\"");
        res.set_content(std::string(4096, 'f'), "audio/mpeg");
    });
    server.routes().Get("/moved", [](const httplib::Request&, httplib::Response& res) {
        res.set_redirect("/file", 302);
    });
    server.routes().Get("/empty", [](const httplib::Request&, httplib::Response& res) {
        res.status = 204;
        res.set_header("X-Empty", "yes");
    });
    server.start();
    Harness h;

    auto head_request = get(server.url("/file"));
    head_request.method = ports::HttpMethod::head;
    const auto head = h.fetched(head_request);
    CHECK(head.head.status == 200);
    CHECK(head.head.header("ETag") == "\"v1\"");
    CHECK(head.body.empty());

    // Content-Length of a HEAD response describes a body that never comes.
    auto small = head_request;
    small.max_response_bytes = 16;
    CHECK(h.fetched(small).head.status == 200);

    auto moved = get(server.url("/moved"));
    moved.method = ports::HttpMethod::head;
    const auto followed = h.fetched(moved);
    CHECK(followed.head.status == 200);
    CHECK(followed.head.final_url == server.url("/file"));
    CHECK(server.seen.method_by_path["/file"] == "HEAD");

    const auto empty = h.fetched(get(server.url("/empty")));
    CHECK(empty.head.status == 204);
    CHECK(empty.head.header("X-Empty") == "yes");

    int heads = 0;
    auto streamed = test::unwrap(h.client->stream(
        get(server.url("/empty")), h.base(), Harness::context(),
        [&](const ports::HttpResponseHead& seen) {
            ++heads;
            return seen.status == 204;
        },
        [](std::span<const char>) { return true; }));
    REQUIRE(std::holds_alternative<ports::HttpResponseHead>(streamed));
    CHECK(heads == 1);
    CHECK(std::get<ports::HttpResponseHead>(streamed).status == 204);
    CHECK_FALSE(std::get<ports::HttpResponseHead>(streamed).stopped_by_consumer);

    auto refused = test::unwrap(h.client->stream(
        get(server.url("/empty")), h.base(), Harness::context(), [](const ports::HttpResponseHead&) { return false; },
        [](std::span<const char>) { return true; }));
    REQUIRE(std::holds_alternative<ports::HttpResponseHead>(refused));
    CHECK(std::get<ports::HttpResponseHead>(refused).stopped_by_consumer);
    CHECK(h.ledger->usage().total == 0);
}

TEST_CASE("a TLS handshake that never completes is a retryable timeout", "[transport]") {
    // Accepts the TCP connection and never answers the ClientHello.
    RawServer silent{[](socket_t, const std::atomic<bool>& closing) { hold(closing, 5s); }};
    Harness h{{}, true, 1s};

    const auto started = std::chrono::steady_clock::now();
    const auto result = h.fetch(get("https://127.0.0.1:" + std::to_string(silent.port()) + "/x"));
    CHECK(std::chrono::steady_clock::now() - started < 3s);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == domain::ErrorCode::upstream_timeout);
    CHECK(result.error().retryable);
    CHECK(category(result.error()) == "tls_handshake_timeout");
    CHECK(h.ledger->usage().total == 0);
}

TEST_CASE("bodies stream in chunks and bounded responses stop at the limit", "[transport]") {
    constexpr std::size_t total = 4 * 1024 * 1024;
    const std::string piece(64 * 1024, 'a');
    TestServer server{TestServer::Kind::trusted_tls};
    server.routes().Get("/chunked", [&](const httplib::Request&, httplib::Response& res) {
        res.set_chunked_content_provider("audio/mpeg", [&](std::size_t offset, httplib::DataSink& sink) {
            if (offset >= total) {
                sink.done();
                return true;
            }
            return sink.write(piece.data(), piece.size());
        });
    });
    server.routes().Get("/sized", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content(std::string(2 * 1024 * 1024, 'b'), "application/octet-stream");
    });
    server.start();
    Harness h;

    std::size_t received = 0;
    std::size_t chunks = 0;
    std::size_t largest = 0;
    int heads = 0;
    auto streamed = test::unwrap(h.client->stream(
        [&] {
            auto r = get(server.url("/chunked"));
            r.operation = "download";
            return r;
        }(),
        h.base(), Harness::context(),
        [&](const ports::HttpResponseHead& head) {
            ++heads;
            return head.status == 200;
        },
        [&](std::span<const char> chunk) {
            received += chunk.size();
            largest = std::max(largest, chunk.size());
            ++chunks;
            return true;
        }));
    REQUIRE(std::holds_alternative<ports::HttpResponseHead>(streamed));
    CHECK(heads == 1);
    CHECK(received == total);
    CHECK(chunks > 16);
    CHECK(largest < total / 4); // never handed over as one buffered body

    // The same bodies through the bounded API fail instead of being truncated.
    CHECK(category(h.fetch(get(server.url("/chunked"))).error()) == "response_too_large");
    CHECK(category(h.fetch(get(server.url("/sized"))).error()) == "response_too_large");

    // A consumer that refuses the head stops before the body.
    std::size_t refused_bytes = 0;
    auto refused = test::unwrap(h.client->stream(
        get(server.url("/chunked")), h.base(), Harness::context(), [](const ports::HttpResponseHead&) { return false; },
        [&](std::span<const char> chunk) {
            refused_bytes += chunk.size();
            return true;
        }));
    REQUIRE(std::holds_alternative<ports::HttpResponseHead>(refused));
    CHECK(std::get<ports::HttpResponseHead>(refused).stopped_by_consumer);
    CHECK(refused_bytes == 0);
    CHECK(h.ledger->usage().total == 0);
}

TEST_CASE("a response cut short is a retryable connection error", "[transport]") {
    TestServer server{TestServer::Kind::trusted_tls};
    server.routes().Get("/cut", [](const httplib::Request&, httplib::Response& res) {
        res.set_content_provider(1000, "application/json", [](std::size_t offset, std::size_t, httplib::DataSink& sink) {
            if (offset == 0) {
                sink.write("{\"partial\":", 11);
                return true;
            }
            return false; // the server drops the connection
        });
    });
    server.start();
    Harness h;

    const auto result = h.fetch(get(server.url("/cut")));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == domain::ErrorCode::upstream_failed);
    CHECK(result.error().retryable);
    CHECK(category(result.error()) == "connection_lost");
    CHECK(h.ledger->usage().total == 0);
}

TEST_CASE("cancellation and deadlines end the transfer and release the lease", "[transport]") {
    TestServer server{TestServer::Kind::trusted_tls};
    server.routes().Get("/slow-head", [&](const httplib::Request&, httplib::Response& res) {
        // Answers two seconds after the request, or when the server closes.
        const auto until = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < until && !server.closing.load()) {
            std::this_thread::sleep_for(10ms);
        }
        res.set_content("late", "text/plain");
    });
    const std::string piece(1024, 'c');
    server.routes().Get("/trickle", [&](const httplib::Request&, httplib::Response& res) {
        res.set_chunked_content_provider("audio/mpeg", [&](std::size_t, httplib::DataSink& sink) {
            if (server.closing.load()) {
                return false;
            }
            std::this_thread::sleep_for(20ms);
            return sink.write(piece.data(), piece.size());
        });
    });
    server.start();
    Harness h;

    SECTION("stop while waiting for the response head") {
        std::stop_source stop;
        std::atomic<std::uint32_t> held_after_stop{0};
        std::atomic<bool> returned{false};
        std::jthread canceler([&] {
            std::this_thread::sleep_for(200ms);
            stop.request_stop();
            std::this_thread::sleep_for(300ms);
            if (!returned.load()) {
                held_after_stop.store(h.ledger->usage().total);
            }
        });
        const auto started = std::chrono::steady_clock::now();
        const auto result = h.fetch(get(server.url("/slow-head")), Harness::context(10s, stop.get_token()));
        returned.store(true);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == domain::ErrorCode::canceled);
#ifdef _WIN32
        // A local shutdown() does not wake a blocked wait on Windows and httplib's
        // client has no interruptible read: the call ends when the upstream answers,
        // the idle timeout or the deadline fires. The lease stays held until then.
        CHECK(elapsed < 2500ms); // the upstream answers at about 2 s
        canceler.join();
        CHECK(held_after_stop.load() == 1);
#else
        CHECK(elapsed < 1500ms);
#endif
    }
    SECTION("stop while the body streams") {
        std::stop_source stop;
        std::size_t received = 0;
        const auto result = h.client->stream(
            get(server.url("/trickle")), h.base(), Harness::context(10s, stop.get_token()),
            [](const ports::HttpResponseHead&) { return true; },
            [&](std::span<const char> chunk) {
                received += chunk.size();
                if (received >= 3 * 1024) {
                    stop.request_stop();
                }
                return true;
            });
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == domain::ErrorCode::canceled);
    }
    SECTION("the deadline holds while the head trickles in") {
        // Each header line arrives well within the idle timeout; only the total
        // deadline can end the request.
        RawServer trickle{[](socket_t peer, const std::atomic<bool>& closing) {
            send_text(peer, "HTTP/1.1 200 OK\r\n");
            for (int i = 0; i < 400 && !closing.load(); ++i) {
                send_text(peer, "X-Slow: " + std::to_string(i) + "\r\n");
                std::this_thread::sleep_for(20ms);
            }
        }};
        auto request = get("http://127.0.0.1:" + std::to_string(trickle.port()) + "/slow");
        request.plaintext_endpoints.push_back(
            {"127.0.0.1", static_cast<std::uint16_t>(trickle.port()), "/slow", "search", false});
        const auto started = std::chrono::steady_clock::now();
        const auto result = h.fetch(request, Harness::context(300ms));
        CHECK(std::chrono::steady_clock::now() - started < 600ms);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == domain::ErrorCode::deadline_exceeded);
    }
    SECTION("the deadline passes") {
        const auto started = std::chrono::steady_clock::now();
        const auto result = h.fetch(get(server.url("/slow-head")), Harness::context(300ms));
        CHECK(std::chrono::steady_clock::now() - started < 1500ms);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == domain::ErrorCode::deadline_exceeded);
    }
    CHECK(h.ledger->usage().total == 0);
    server.closing.store(true);
}

TEST_CASE("the transport refuses to run without a lease for the target origin", "[transport]") {
    TestServer server{TestServer::Kind::trusted_tls};
    server.routes().Get("/x", [](const httplib::Request&, httplib::Response& res) { res.set_content("x", "text/plain"); });
    server.start();
    Harness h;

    auto other = h.base();
    other.origin = "https://elsewhere.example";
    other.deadline = std::chrono::steady_clock::now() + 5s;
    auto granted = test::unwrap(h.ledger->try_acquire(other));
    auto& lease = std::get<ports::UpstreamLease>(granted);
    const auto ctx = Harness::context();

    CHECK(h.transport->request(get(server.url("/x")), lease, ctx).error().code == domain::ErrorCode::validation_failed);
    lease.release();
    CHECK(h.transport->request(get(server.url("/x")), lease, ctx).error().code == domain::ErrorCode::validation_failed);
    CHECK(server.seen.requests == 0);
}

// Opt-in (MUSIC_SERVICE_ONLINE_TESTS=1): a real public HTTPS endpoint through the
// operating system's trust roots, i.e. the Windows certificate store or the Linux CA
// files, as deployed. Skipped otherwise so the suite needs no outside network.
TEST_CASE("public HTTPS verifies against the system trust roots", "[transport][online]") {
    const auto enabled = infrastructure::config::process_environment("MUSIC_SERVICE_ONLINE_TESTS");
    if (enabled != "1") {
        SKIP("set MUSIC_SERVICE_ONLINE_TESTS=1 to run");
    }
    Harness h{{}, false};
    auto request = get("https://www.example.com/");
    request.operation = "search";
    const auto result = h.fetch(request, Harness::context(15s));
    INFO((result ? std::string{"ok"} : category(result.error()) + ": " + result.error().message));
    REQUIRE(result.has_value());
    REQUIRE(std::holds_alternative<ports::HttpResponse>(result.value()));
    const auto& response = std::get<ports::HttpResponse>(result.value());
    CHECK(response.head.status == 200);
    CHECK(response.body.find("Example Domain") != std::string::npos);

    CHECK(h.ledger->usage().total == 0);
}
