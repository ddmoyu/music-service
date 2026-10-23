// Online audio through the media proxy and direct URLs (T34), without network: a
// simulated source whose CDN serves its audio with validators and ranges. Ranges, HEAD
// and 416 pass through as the upstream answers (A24's upstream part); secret request
// headers stay on the server and decide between direct and proxy (A31); an expired
// address is resolved again for the same audio while a different resource ends the asset
// instead of being mixed in (A16 on the proxy path, A32); a client that stalls slows the
// upstream and one that leaves cancels it, and two clients never share a transfer (A24,
// A30); with the proxy off, assets are refused while files stay readable (A33).

#include "service_harness.hpp"

#include <atomic>
#include <map>
#include <mutex>
#include <set>

using namespace music_service;
using namespace music_service::test;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

std::string pattern(std::size_t size, char base = 'a') {
    std::string bytes(size, '\0');
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<char>(base + static_cast<char>(i % 26));
    }
    return bytes;
}

// Ids tell the resolver what kind of address to hand out:
//   plain-*      shareable, no headers
//   secret-*     needs a Cookie header, not shareable
//   enc-*        needs decryption
//   expiring-*   the first address stops working after one use; the next serves the same bytes
//   swap-*       likewise, but the next address serves other bytes
//   chunked-*    the CDN sends no Content-Length
//   norange-*    the CDN ignores Range
//   big-*        8 MiB, sent slowly
class Resolver final : public ports::IAssetResolver {
public:
    explicit Resolver(std::shared_ptr<std::atomic<int>> calls) : calls_(std::move(calls)) {}

    domain::Result<ports::ResolvedAudio> resolve_audio(const ports::ProviderContext&, const domain::TrackRef& track,
                                                       const domain::ResourceRequirement&) override {
        const auto& id = track.id.value();
        ++(*calls_);
        int seen = 0;
        {
            const std::lock_guard lock{mutex_};
            seen = ++per_id_[id];
        }
        const auto version = seen > 1 && (id.rfind("expiring", 0) == 0 || id.rfind("swap", 0) == 0) ? 2 : 1;
        ports::ResolvedAudio audio{track,
                                   domain::Quality::standard,
                                   domain::Completeness::full,
                                   std::string{"mp3"},
                                   std::int64_t{128000},
                                   "https://cdn.prx.test/v" + std::to_string(version) + "/" + id,
                                   {},
                                   std::nullopt,
                                   std::nullopt,
                                   ports::ProcessingPlan{}};
        if (id.rfind("secret", 0) == 0) {
            audio.request_headers = {{"Cookie", "cdn_key=s3cr3t"}};
        } else if (id.rfind("enc", 0) == 0) {
            audio.processing.decryption = "test.cipher";
        } else {
            audio.shareable = true;
        }
        // 模拟地址过期后，权限或音源处理方式发生变化，但 CDN 字节和校验器不变。
        if (seen > 1) {
            if (id == "expiring-preview") audio.completeness = domain::Completeness::preview;
            if (id == "expiring-unknown") audio.completeness = domain::Completeness::unknown;
            if (id == "expiring-encrypted") audio.processing.decryption = "test.cipher";
        }
        return audio;
    }

private:
    std::shared_ptr<std::atomic<int>> calls_;
    std::mutex mutex_;
    std::map<std::string, int> per_id_;
};

ports::ProviderBundle proxy_source(const std::shared_ptr<std::atomic<int>>& calls) {
    ports::ProviderBundle bundle{.source = source("prx"),
                                 .name = "prx",
                                 .implementation_id = "prx-test",
                                 .implementation_version = "1.0.0"};
    bundle.capabilities = {{.key = "resolve_audio"}};
    bundle.bindings = {ports::CapabilityBinding::bind<ports::IAssetResolver>(
        [calls](const ports::ProviderSetup&) -> domain::Result<std::shared_ptr<ports::IAssetResolver>> {
            return std::shared_ptr<ports::IAssetResolver>{std::make_shared<Resolver>(calls)};
        })};
    bundle.quality_hints = {domain::Quality::standard};
    return bundle;
}

class Cdn final : public ports::HttpTransport {
public:
    // The bytes behind an address ("v1/plain-1").
    static std::string bytes_of(const std::string& path) {
        if (path.find("big-") != std::string::npos) {
            return pattern(8 * 1024 * 1024);
        }
        const bool other = path.rfind("v2/swap", 0) == 0;
        return pattern(100 * 1024, other ? 'A' : 'a');
    }

    std::vector<ports::HttpRequest> requests() const {
        const std::lock_guard lock{mutex_};
        return requests_;
    }
    std::uint64_t sent() const { return sent_.load(); }
    int cancelled() const { return cancelled_.load(); }
    int active() const { return active_.load(); }

    domain::Result<ports::HttpResponse> request(const ports::HttpRequest& request, const ports::UpstreamLease&,
                                                const ports::OperationContext&) override {
        return ports::HttpResponse{{404, {{"Content-Length", "0"}}, request.url}, {}};
    }

    domain::Result<ports::HttpResponseHead> receive(const ports::HttpRequest& request, const ports::UpstreamLease&,
                                                    const ports::OperationContext& context,
                                                    const ports::HeadConsumer& on_head,
                                                    const ports::ChunkConsumer& on_chunk) override {
        const auto path = request.url.substr(std::string{"https://cdn.prx.test/"}.size());
        bool gone = false;
        {
            const std::lock_guard lock{mutex_};
            requests_.push_back(request);
            // A first address for expiring/swap works once.
            if (path.rfind("v1/expiring", 0) == 0 || path.rfind("v1/swap", 0) == 0) {
                gone = used_.count(path) != 0;
                used_.insert(path);
            }
        }
        if (gone) {
            ports::HttpResponseHead head{410, {{"Content-Length", "0"}}, request.url};
            (void)on_head(head);
            return head;
        }
        const auto body = bytes_of(path);
        const auto etag = "\"" + std::to_string(std::hash<std::string>{}(body)) + "\"";
        const bool ignores_range = path.find("norange-") != std::string::npos;
        const bool chunked = path.find("chunked-") != std::string::npos;
        const auto range = header(request, "Range");
        const auto if_range = header(request, "If-Range");

        std::size_t first = 0;
        std::size_t last = body.size() - 1;
        ports::HttpResponseHead head{200, {{"Content-Type", "audio/mpeg"}, {"ETag", etag}}, request.url};
        if (!ignores_range) {
            head.headers.push_back({"Accept-Ranges", "bytes"});
        }
        if (range && !ignores_range && (!if_range || *if_range == etag)) {
            const auto spec = range->substr(6);
            const auto dash = spec.find('-');
            if (dash == 0) {
                const auto n = std::min<std::size_t>(std::stoull(spec.substr(1)), body.size());
                first = body.size() - n;
            } else {
                first = std::stoull(spec.substr(0, dash));
                if (dash + 1 < spec.size()) {
                    last = std::min<std::size_t>(std::stoull(spec.substr(dash + 1)), body.size() - 1);
                }
            }
            if (first >= body.size()) {
                head.status = 416;
                head.headers.push_back({"Content-Range", "bytes */" + std::to_string(body.size())});
                head.headers.push_back({"Content-Length", "0"});
                (void)on_head(head);
                return head;
            }
            head.status = 206;
            head.headers.push_back({"Content-Range", "bytes " + std::to_string(first) + "-" + std::to_string(last) +
                                                         "/" + std::to_string(body.size())});
        }
        if (!chunked) {
            head.headers.push_back({"Content-Length", std::to_string(last - first + 1)});
        }
        if (!on_head(head)) {
            return head;
        }
        const bool slow = path.find("big-") != std::string::npos;
        ++active_;
        struct Leave {
            std::atomic<int>& active;
            ~Leave() { --active; }
        } leave{active_};
        for (auto at = first; at <= last;) {
            if (context.stop.stop_requested()) {
                ++cancelled_;
                return domain::Error{domain::ErrorCode::canceled, "the operation was canceled", false, {}};
            }
            const auto size = std::min<std::size_t>(16 * 1024, last - at + 1);
            if (!on_chunk(std::span<const char>{body.data() + at, size})) {
                ++cancelled_;
                head.stopped_by_consumer = true;
                return head;
            }
            at += size;
            sent_ += size;
            if (slow) {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
        }
        return head;
    }

private:
    static std::optional<std::string> header(const ports::HttpRequest& request, std::string_view name) {
        for (const auto& h : request.headers) {
            if (h.name == name) {
                return h.value;
            }
        }
        return std::nullopt;
    }

    mutable std::mutex mutex_;
    std::vector<ports::HttpRequest> requests_;
    std::set<std::string> used_;
    std::atomic<std::uint64_t> sent_{0};
    std::atomic<int> cancelled_{0};
    std::atomic<int> active_{0};
};

int port_of(const std::string& url) { return std::stoi(url.substr(url.rfind(':') + 1)); }

std::string path_of(const std::string& url) {
    const auto scheme = url.find("://");
    return url.substr(url.find('/', scheme + 3));
}

struct ProxyDeployment {
    explicit ProxyDeployment(std::string_view name, bool proxy = true, bool direct = true) : d{name} {
        d.config["media"] = {{"enabled", true}, {"port", 0}};
        d.config["features"] = {{"file_access", true}, {"media_proxy", proxy}, {"direct_media_urls", direct}};
        d.config["media_stream"] = {{"bridge_buffer_bytes", 64 * 1024}};
        d.write_config();
        server::CommandHooks hooks;
        hooks.host.bundles = bundles();
        const auto initialized = d.run({"init"}, hooks);
        INFO(initialized.err);
        REQUIRE(initialized.code == server::exit_ok);
        token = d.read(d.bootstrap());
        service = std::make_unique<RunningService>(d, std::make_shared<infrastructure::observability::MemorySink>(),
                                                   bundles(), true, cdn);
        api = std::make_unique<httplib::Client>(service->client(token));
    }

    std::vector<ports::ProviderBundle> bundles() { return {proxy_source(calls)}; }

    httplib::Client media() const {
        httplib::Client c("127.0.0.1", port_of(*service->report.media_base_url));
        c.set_connection_timeout(3s);
        c.set_read_timeout(10s);
        return c;
    }

    std::string resolve(const std::string& id) {
        const auto resolved =
            api->Post("/api/v1/assets/resolve", R"({"track":{"source":"prx","id":")" + id + R"("}})", "application/json");
        REQUIRE(resolved);
        INFO(resolved->body);
        REQUIRE(resolved->status == 200);
        return body(resolved)["data"]["asset_id"].get<std::string>();
    }

    httplib::Result access(const std::string& asset_id, const std::string& extra = "") {
        return api->Post("/api/v1/media-access",
                         R"({"target":{"kind":"asset","id":")" + asset_id + R"("})" + extra + "}", "application/json");
    }

    // The ticket path of a proxy access description.
    std::string ticket(const std::string& asset_id) {
        const auto described = access(asset_id, R"(,"mode":"proxy")");
        REQUIRE(described);
        INFO(described->body);
        REQUIRE(described->status == 200);
        const auto data = body(described)["data"];
        REQUIRE(data["delivery"] == "proxy");
        return path_of(data["url"].get<std::string>());
    }

    int media_responses() {
        return body(api->Get("/api/v1/admin/status"))["data"]["media"]["responses"].get<int>();
    }

    Deployment d;
    std::shared_ptr<std::atomic<int>> calls = std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<Cdn> cdn = std::make_shared<Cdn>();
    std::string token;
    std::unique_ptr<RunningService> service;
    std::unique_ptr<httplib::Client> api;
};

template <class Ready> bool eventually(Ready ready, std::chrono::milliseconds limit = 10s) {
    const auto until = Clock::now() + limit;
    while (Clock::now() < until) {
        if (ready()) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return ready();
}

} // namespace

TEST_CASE("proxied audio passes ranges and validators through, never secrets (A24, A31)", "[media][proxy]") {
    ProxyDeployment p{"proxy-ranges"};
    const auto source = Cdn::bytes_of("v1/plain-1");
    const auto total = std::to_string(source.size());
    const auto asset = p.resolve("plain-1");
    const auto ticket = p.ticket(asset);
    auto media = p.media();

    const auto whole = media.Get(ticket);
    REQUIRE(whole);
    CHECK(whole->status == 200);
    CHECK(whole->body == source);
    CHECK(whole->get_header_value("Content-Length") == total);
    CHECK(whole->get_header_value("Accept-Ranges") == "bytes");
    CHECK(whole->get_header_value("Content-Type") == "audio/mpeg");
    CHECK(whole->get_header_value("Cache-Control") == "no-store");
    const auto etag = whole->get_header_value("ETag");
    CHECK_FALSE(etag.empty());

    const auto part = media.Get(ticket, httplib::Headers{{"Range", "bytes=100-199"}});
    CHECK(part->status == 206);
    CHECK(part->get_header_value("Content-Range") == "bytes 100-199/" + total);
    CHECK(part->body == source.substr(100, 100));
    const auto tail = media.Get(ticket, httplib::Headers{{"Range", "bytes=-50"}});
    CHECK(tail->status == 206);
    CHECK(tail->body == source.substr(source.size() - 50));
    const auto from = media.Get(ticket, httplib::Headers{{"Range", "bytes=102000-"}});
    CHECK(from->status == 206);
    CHECK(from->body == source.substr(102000));
    const auto beyond = media.Get(ticket, httplib::Headers{{"Range", "bytes=" + total + "-"}});
    CHECK(beyond->status == 416);
    CHECK(beyond->get_header_value("Content-Range") == "bytes */" + total);
    CHECK(media.Get(ticket, httplib::Headers{{"Range", "bytes=0-1,5-6"}})->status == 400);
    // A stale validator gets the whole current resource, never a piece of another one.
    const auto stale = media.Get(ticket, httplib::Headers{{"Range", "bytes=0-9"}, {"If-Range", "\"other\""}});
    CHECK(stale->status == 200);
    CHECK(stale->body == source);
    const auto same = media.Get(ticket, httplib::Headers{{"Range", "bytes=0-9"}, {"If-Range", etag}});
    CHECK(same->status == 206);
    const auto head = media.Head(ticket);
    CHECK(head->status == 200);
    CHECK(head->get_header_value("Content-Length") == total);
    CHECK(head->body.empty());

    // A Bearer client reads the same audio from the asset route.
    auto bearer = p.media();
    bearer.set_bearer_token_auth(p.token);
    const auto direct_read = bearer.Get("/media/v1/assets/" + asset, httplib::Headers{{"Range", "bytes=0-3"}});
    CHECK(direct_read->status == 206);
    CHECK(direct_read->body == source.substr(0, 4));
    CHECK(p.media().Get("/media/v1/assets/" + asset)->status == 401);

    // An upstream that ignores Range answers 200, and so does the proxy.
    const auto norange = p.ticket(p.resolve("norange-1"));
    const auto ignored = media.Get(norange, httplib::Headers{{"Range", "bytes=10-19"}});
    CHECK(ignored->status == 200);
    CHECK(ignored->body == Cdn::bytes_of("v1/norange-1"));
    // No length from the upstream: the body is passed on without inventing one.
    const auto chunked = media.Get(p.ticket(p.resolve("chunked-1")));
    CHECK(chunked->status == 200);
    CHECK(chunked->body == Cdn::bytes_of("v1/chunked-1"));
    CHECK_FALSE(chunked->has_header("Content-Length"));

    // A31: an address that needs a secret header is only proxied; the header goes to the
    // CDN and nowhere else.
    const auto secret = p.resolve("secret-1");
    const auto refused = p.access(secret, R"(,"mode":"direct")");
    CHECK(refused->status == 422);
    CHECK(error_code(refused) == "DELIVERY_UNAVAILABLE");
    const auto automatic = p.access(secret);
    REQUIRE(automatic->status == 200);
    CHECK(body(automatic)["data"]["delivery"] == "proxy");
    CHECK(automatic->body.find("s3cr3t") == std::string::npos);
    const auto secret_read = media.Get(path_of(body(automatic)["data"]["url"].get<std::string>()));
    CHECK(secret_read->status == 200);
    CHECK(secret_read->body.find("s3cr3t") == std::string::npos);
    bool sent_secret = false;
    for (const auto& request : p.cdn->requests()) {
        for (const auto& h : request.headers) {
            CHECK(h.value.find("s3cr3t") == std::string::npos); // never as a plain header
        }
        for (const auto& h : request.secret_headers) {
            sent_secret = sent_secret || h.value == "cdn_key=s3cr3t";
        }
    }
    CHECK(sent_secret);

    // A shareable address goes out directly under auto; the description is not cacheable.
    const auto direct = p.access(asset);
    REQUIRE(direct->status == 200);
    CHECK(direct->get_header_value("Cache-Control") == "no-store");
    const auto described = body(direct)["data"];
    CHECK(described["delivery"] == "direct");
    CHECK(described["url"] == "https://cdn.prx.test/v1/plain-1");
    CHECK(described["actual_track"]["id"] == "plain-1");
    CHECK(described["actual_quality"] == "standard");
    // Audio that must be decrypted cannot be streamed.
    const auto encrypted = p.access(p.resolve("enc-1"), R"(,"mode":"proxy")");
    CHECK(encrypted->status == 422);
    CHECK(error_code(encrypted) == "DELIVERY_UNAVAILABLE");
}

TEST_CASE("an expired address is renewed for the same audio; other audio ends the asset (A16, A32)",
          "[media][proxy]") {
    ProxyDeployment p{"proxy-refresh"};
    auto media = p.media();

    const auto expiring = p.resolve("expiring-1");
    const auto ticket = p.ticket(expiring);
    CHECK(media.Get(ticket, httplib::Headers{{"Range", "bytes=0-9"}})->status == 206);
    // The first address is gone now: resolved again, same bytes, the client sees nothing.
    const auto calls = p.calls->load();
    const auto renewed = media.Get(ticket);
    CHECK(renewed->status == 200);
    CHECK(renewed->body == Cdn::bytes_of("v2/expiring-1"));
    CHECK(p.calls->load() == calls + 1);

    // The renewed address serves other bytes: the asset ends instead of mixing them in.
    const auto swap = p.resolve("swap-1");
    const auto swap_ticket = p.ticket(swap);
    CHECK(media.Get(swap_ticket, httplib::Headers{{"Range", "bytes=0-9"}})->status == 206);
    const auto ended = media.Get(swap_ticket, httplib::Headers{{"Range", "bytes=10-19"}});
    CHECK(ended->status == 410);
    CHECK(error_code(ended) == "RESOURCE_EXPIRED");
    CHECK(error_code(p.access(swap)) == "RESOURCE_EXPIRED"); // resolve again, not a new ticket
    CHECK(media.Get(swap_ticket)->status == 410);

    // A32: the access description never outlives the asset.
    const auto asset = body(p.api->Post("/api/v1/assets/resolve", R"({"track":{"source":"prx","id":"plain-2"}})",
                                        "application/json"))["data"];
    const auto described = body(p.access(asset["asset_id"].get<std::string>(), R"(,"mode":"proxy","ttl_seconds":300)"))["data"];
    CHECK(described["expires_at"].get<std::string>() <= asset["expires_at"].get<std::string>());
}

TEST_CASE("renewal cannot retain a full plain asset after preview or encryption changes",
          "[media][proxy]") {
    ProxyDeployment p{"proxy-renewal-permission"};
    auto media = p.media();
    for (const auto id : {"expiring-preview", "expiring-unknown", "expiring-encrypted"}) {
        INFO(id);
        const auto asset = p.resolve(id);
        const auto ticket = p.ticket(asset);
        const auto first = media.Get(ticket, httplib::Headers{{"Range", "bytes=0-9"}});
        REQUIRE(first);
        REQUIRE(first->status == 206);
        const auto requests = p.cdn->requests().size();
        const auto renewed = media.Get(ticket);
        REQUIRE(renewed);
        CHECK(renewed->status == 410);
        CHECK(error_code(renewed) == "RESOURCE_EXPIRED");
        // 只允许访问已失效的旧地址；重新解析后的音源不得送到 CDN 或客户端。
        CHECK(p.cdn->requests().size() == requests + 1);
        CHECK(error_code(p.access(asset)) == "RESOURCE_EXPIRED");
    }
}

TEST_CASE("a stalled client slows the upstream, a departed one cancels it, clients do not share (A24, A30)",
          "[media][proxy]") {
    ProxyDeployment p{"proxy-flow"};
    const auto asset = p.resolve("big-1");
    const auto ticket_a = p.ticket(asset);
    const auto ticket_b = p.ticket(asset);
    const auto source = Cdn::bytes_of("v1/big-1");

    // Client A reads 256 KiB, then stops reading for a while, then leaves.
    std::atomic<std::uint64_t> received{0};
    std::atomic<bool> paused{false};
    std::atomic<bool> leave{false};
    std::thread a{[&] {
        auto media = p.media();
        media.Get(ticket_a, [&](const char*, std::size_t size) {
            received += size;
            if (received >= 256 * 1024) {
                paused = true;
                while (!leave) {
                    std::this_thread::sleep_for(10ms);
                }
                return false;
            }
            return true;
        });
    }};
    REQUIRE(eventually([&] { return paused.load(); }));
    // While A stalls, the upstream stops too: far less than the 8 MiB is read.
    std::this_thread::sleep_for(500ms);
    const auto stalled_at = p.cdn->sent();
    std::this_thread::sleep_for(500ms);
    CHECK(p.cdn->sent() == stalled_at);
    CHECK(stalled_at < source.size() / 2);
    CHECK(p.media_responses() == 1);

    // B reads its own range of the same asset meanwhile, unaffected by A.
    const auto b = p.media().Get(ticket_b, httplib::Headers{{"Range", "bytes=1000000-1000099"}});
    CHECK(b->status == 206);
    CHECK(b->body == source.substr(1000000, 100));

    // A leaves: its upstream transfer is cancelled and its slot freed.
    const auto cancelled = p.cdn->cancelled();
    leave = true;
    a.join();
    CHECK(eventually([&] { return p.cdn->cancelled() > cancelled && p.cdn->active() == 0; }));
    CHECK(eventually([&] { return p.media_responses() == 0; }));
    CHECK(received.load() >= 256 * 1024);
}

TEST_CASE("without the proxy, assets are refused and nothing is fetched (A33)", "[media][proxy]") {
    ProxyDeployment p{"proxy-off", false, false};
    const auto asset = p.resolve("plain-1");
    const auto refused = p.access(asset);
    CHECK(refused->status == 501);
    CHECK(error_code(refused) == "FEATURE_UNAVAILABLE");
    CHECK(error_code(p.access(asset, R"(,"mode":"direct")")) == "FEATURE_UNAVAILABLE");
    auto bearer = p.media();
    bearer.set_bearer_token_auth(p.token);
    CHECK(bearer.Get("/media/v1/assets/" + asset)->status == 501);
    CHECK(p.cdn->requests().empty());
    const auto info = body(p.api->Get("/api/v1/info"))["data"];
    CHECK(info["features"]["media_proxy"] == false);
    CHECK(info["features"]["file_access"] == true);
}
