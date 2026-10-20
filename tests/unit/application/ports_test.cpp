// Exercises the outbound ports with in-memory doubles, so their contracts are usable
// before the real runtime and transport adapters exist.

#include "../domain/test_support.hpp"

#include "application/ports/runtime/admission.hpp"
#include "application/ports/transport/http_transport.hpp"

#include <map>
#include <string>
#include <vector>

using namespace music_service;
using namespace music_service::application::ports;

namespace {

// Per-source concurrency ledger: grants all-or-nothing and never blocks.
class FakeAdmission final : public UpstreamAdmissionPort {
public:
    explicit FakeAdmission(std::uint32_t per_source) : per_source_(per_source) {}

    domain::Result<AdmissionOutcome> try_acquire(const AdmissionRequest& request) override {
        auto& used = in_use_[request.source.value()];
        if (used + request.connections > per_source_) {
            return AdmissionOutcome{Deferred{"source", request.deadline}};
        }
        used += request.connections;
        return AdmissionOutcome{UpstreamLease{
            ++next_id_, request, [this, source = request.source.value(),
                                  n = request.connections](std::uint64_t) {
                in_use_[source] -= n;
                ++released_;
            }}};
    }

    void feedback(const AdmissionRequest&,
                  std::optional<std::chrono::steady_clock::time_point>) override {}

    std::optional<Deferred> readiness(const AdmissionRequest&) const override { return std::nullopt; }

    std::uint32_t in_use(const std::string& source) { return in_use_[source]; }
    int released() const { return released_; }

private:
    std::uint32_t per_source_;
    std::map<std::string, std::uint32_t> in_use_;
    std::uint64_t next_id_{0};
    int released_{0};
};

// Refuses to run without a lease for the request's origin.
class FakeTransport final : public HttpTransport {
public:
    domain::Result<HttpResponse> request(const HttpRequest& request, const UpstreamLease& lease,
                                         const OperationContext&) override {
        const auto& origin = lease.request().origin;
        const bool same_origin =
            request.url.rfind(origin, 0) == 0 &&
            (request.url.size() == origin.size() ||
             std::string_view{"/?#"}.find(request.url[origin.size()]) != std::string_view::npos);
        if (!lease.active() || !same_origin) {
            return domain::Error{domain::ErrorCode::internal, "lease does not match target",
                                 false, {}};
        }
        return HttpResponse{{200, {{"Content-Type", "application/json"}}, request.url}, "{}"};
    }

    domain::Result<HttpResponseHead> receive(const HttpRequest&, const UpstreamLease&,
                                             const OperationContext&, const HeadConsumer&,
                                             const ChunkConsumer&) override {
        return domain::Error{domain::ErrorCode::capability_unsupported, "not used", false, {}};
    }
};

AdmissionRequest request_for(std::string_view source, std::string origin) {
    AdmissionRequest request{
        "prn_a",
        test::source(source),
        "search_tracks",
        TrafficClass::short_request,
        std::nullopt,
        std::move(origin),
        "default",
        std::chrono::steady_clock::now() + std::chrono::seconds{5},
    };
    return request;
}

} // namespace

TEST_CASE("leases are returned exactly once", "[application][ports]") {
    int releases = 0;
    {
        UpstreamLease lease{1, request_for("kuwo", "http://search.kuwo.cn"),
                            [&](std::uint64_t) { ++releases; }};
        UpstreamLease moved{std::move(lease)};
        CHECK_FALSE(lease.active()); // NOLINT(bugprone-use-after-move): verifying the move
        CHECK(moved.active());
        moved.release();
        moved.release();
        CHECK_FALSE(moved.active());
    }
    CHECK(releases == 1);
}

TEST_CASE("admission defers instead of blocking and other sources proceed",
          "[application][ports]") {
    FakeAdmission admission{1};

    auto first = admission.try_acquire(request_for("kuwo", "http://search.kuwo.cn"));
    REQUIRE(first);
    REQUIRE(std::holds_alternative<UpstreamLease>(*first));

    auto second = admission.try_acquire(request_for("kuwo", "http://search.kuwo.cn"));
    REQUIRE(second);
    CHECK(std::holds_alternative<Deferred>(*second));

    auto other = admission.try_acquire(request_for("qq", "https://u.y.qq.com"));
    REQUIRE(other);
    CHECK(std::holds_alternative<UpstreamLease>(*other));

    std::get<UpstreamLease>(*first).release();
    CHECK(admission.in_use("kuwo") == 0);
    auto third = admission.try_acquire(request_for("kuwo", "http://search.kuwo.cn"));
    REQUIRE(third);
    CHECK(std::holds_alternative<UpstreamLease>(*third));
}

TEST_CASE("transport requires a matching lease", "[application][ports]") {
    FakeAdmission admission{2};
    FakeTransport transport;
    const OperationContext context{std::chrono::steady_clock::now() + std::chrono::seconds{5}, {}};

    auto granted = admission.try_acquire(request_for("kuwo", "http://search.kuwo.cn"));
    REQUIRE(granted);
    auto& lease = std::get<UpstreamLease>(*granted);

    HttpRequest same_origin{.method = HttpMethod::get, .url = "http://search.kuwo.cn/r.s?all=x"};
    HttpRequest other_origin{.method = HttpMethod::get, .url = "https://evil.example/x"};

    const auto response = transport.request(same_origin, lease, context);
    REQUIRE(response);
    CHECK(response->head.header("content-type") == "application/json");
    CHECK_FALSE(response->head.header("ETag").has_value());
    CHECK_FALSE(transport.request(other_origin, lease, context));
    HttpRequest prefixed_host{.method = HttpMethod::get, .url = "http://search.kuwo.cn.evil.example/x"};
    CHECK_FALSE(transport.request(prefixed_host, lease, context));
    HttpRequest other_port{.method = HttpMethod::get, .url = "http://search.kuwo.cn:8080/x"};
    CHECK_FALSE(transport.request(other_port, lease, context));

    lease.release();
    CHECK_FALSE(transport.request(same_origin, lease, context));
    CHECK(admission.released() == 1);
}
