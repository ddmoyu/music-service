// T32, the S3 reliability gate, without network: the real music-service process with two
// simulated sources whose CDNs serve MP3, FLAC and M4A and inject the faults the runner
// must survive: bodies cut mid-transfer, 503, 429 with Retry-After, a resource that
// changes between attempts, a 206 for the wrong range, a damaged file, a file that is
// gone for good, a destination that goes offline and comes back, and a restart under
// load. Every task must end as a whole, faithful file or as a clear failure; nothing
// partial, mixed or misplaced is published. The CDN measures concurrency against the
// configured bounds, and the run reports threads, peak memory and control latency as a
// "T32-METRICS" line on stdout.

#include "service_harness.hpp"

#include "audio_processing/format/byte_reader.hpp"
#include "audio_processing/format/flac.hpp"
#include "audio_processing/format/mp4.hpp"

#include <algorithm>
#include <condition_variable>
#include <iostream>
#include <map>
#include <mutex>
#include <set>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#endif

using namespace music_service;
using namespace music_service::test;
namespace fs = std::filesystem;

namespace {

using ErrorCode = domain::ErrorCode;
using Clock = std::chrono::steady_clock;

std::string fixture(const char* name) {
    std::ifstream file(fs::path{MUSIC_SERVICE_TEST_FIXTURES} / "audio" / name, std::ios::binary);
    REQUIRE(file);
    return {std::istreambuf_iterator<char>(file), {}};
}

// 128 kbit/s MPEG-1 Layer III frames at 44.1 kHz whose payload byte tells the content
// apart, so a file mixed from two resources is detectable.
std::string mp3(std::size_t frames, char fill) {
    std::string bytes;
    for (std::size_t i = 0; i < frames; ++i) {
        const auto padding = i % 3 == 0;
        std::string frame(static_cast<std::size_t>(144 * 128 * 1000 / 44100 + (padding ? 1 : 0)), fill);
        frame[0] = '\xFF';
        frame[1] = '\xFB';
        frame[2] = static_cast<char>((9U << 4) | (padding ? 0x02U : 0U));
        frame[3] = '\x40';
        bytes += frame;
    }
    return bytes;
}

std::string without_id3v2(const std::string& bytes) {
    if (bytes.size() < 10 || bytes.compare(0, 3, "ID3") != 0) {
        return bytes;
    }
    const auto size = (std::size_t{static_cast<unsigned char>(bytes[6]) & 0x7FU} << 21) |
                      (std::size_t{static_cast<unsigned char>(bytes[7]) & 0x7FU} << 14) |
                      (std::size_t{static_cast<unsigned char>(bytes[8]) & 0x7FU} << 7) |
                      std::size_t{static_cast<unsigned char>(bytes[9]) & 0x7FU};
    return bytes.substr(std::min(bytes.size(), 10 + size));
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

std::size_t process_threads() {
#ifdef _WIN32
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    REQUIRE(snapshot != INVALID_HANDLE_VALUE);
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    std::size_t count = 0;
    for (auto more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID == GetCurrentProcessId()) {
            ++count;
        }
    }
    CloseHandle(snapshot);
    return count;
#elif defined(__APPLE__)
    thread_act_array_t threads{};
    mach_msg_type_number_t count = 0;
    REQUIRE(task_threads(mach_task_self(), &threads, &count) == KERN_SUCCESS);
    for (mach_msg_type_number_t i = 0; i < count; ++i) {
        mach_port_deallocate(mach_task_self(), threads[i]);
    }
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads), sizeof(thread_t) * count);
    return count;
#else
    std::size_t count = 0;
    for ([[maybe_unused]] const auto& entry : fs::directory_iterator("/proc/self/task")) {
        ++count;
    }
    return count;
#endif
}

// Peak resident memory of this process (the test and the service together), in bytes.
std::uint64_t peak_resident_bytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof counters)) {
        return counters.PeakWorkingSetSize;
    }
    return 0;
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) ==
        KERN_SUCCESS) {
        return info.resident_size_max;
    }
    return 0;
#else
    std::ifstream status("/proc/self/status");
    for (std::string line; std::getline(status, line);) {
        if (line.rfind("VmHWM:", 0) == 0) {
            return std::stoull(line.substr(6)) * 1024;
        }
    }
    return 0;
#endif
}

// ---------------------------------------------------------------------------------------
// The simulated sources. Track ids are "<format>-<fault>-<n>"; the resolver hands out
// https://cdn-<source>.s3.test/<id> and the CDN below decides from the id what to do.

class GateDetail final : public ports::ITrackDetail {
public:
    domain::Result<domain::Track> track_detail(const ports::ProviderContext&, const domain::TrackRef& track) override {
        return domain::Track{track, "曲目 " + track.id.value(), {{"S3 歌手", std::nullopt}}, {{"S3 专辑", std::nullopt}},
                             std::nullopt, {}};
    }
};

class GateSearch final : public ports::ITrackSearch {
public:
    domain::Result<ports::TrackSearchPage> search_tracks(const ports::ProviderContext& c,
                                                        const ports::TrackSearchQuery& query) override {
        const std::string prefix = "曲目 ";
        const std::string suffix = " S3 歌手";
        ports::TrackSearchPage result;
        if (!query.text.starts_with(prefix) || !query.text.ends_with(suffix)) return result;
        const auto id = query.text.substr(prefix.size(), query.text.size() - prefix.size() - suffix.size());
        auto ref = domain::PlatformId::parse(id);
        if (!ref) return ref.error();
        auto detail = GateDetail{}.track_detail(c, {c.identity.binding().source(), *ref});
        if (!detail) return detail.error();
        result.page.items.push_back(*detail);
        result.page.page = {query.limit, 1, 1, false};
        return result;
    }
};

class GateResolver final : public ports::IAssetResolver {
public:
    domain::Result<ports::ResolvedAudio> resolve_audio(const ports::ProviderContext&, const domain::TrackRef& track,
                                                       const domain::ResourceRequirement&) override {
        const auto& id = track.id.value();
        const auto container = id.substr(0, id.find('-'));
        return ports::ResolvedAudio{track,
                                    domain::Quality::standard,
                                    domain::Completeness::full,
                                    container == "m4a" ? std::string{"mp4"} : container,
                                    std::nullopt,
                                    "https://cdn-" + track.source.value() + ".s3.test/" + id,
                                    {},
                                    std::nullopt,
                                    std::nullopt,
                                    ports::ProcessingPlan{}};
    }
};

ports::ProviderBundle gate_source(std::string_view name) {
    ports::ProviderBundle bundle{.source = source(name),
                                 .name = std::string{name},
                                 .implementation_id = std::string{name} + "-gate",
                                 .implementation_version = "1.0.0"};
    bundle.capabilities = {{.key = "track_detail"}, {.key = "resolve_audio"}, {.key = "search_tracks"}};
    bundle.bindings = {
        ports::CapabilityBinding::bind<ports::ITrackSearch>(
            [](const ports::ProviderSetup&) -> domain::Result<std::shared_ptr<ports::ITrackSearch>> {
                return std::shared_ptr<ports::ITrackSearch>{std::make_shared<GateSearch>()};
            }),
        ports::CapabilityBinding::bind<ports::ITrackDetail>(
            [](const ports::ProviderSetup&) -> domain::Result<std::shared_ptr<ports::ITrackDetail>> {
                return std::shared_ptr<ports::ITrackDetail>{std::make_shared<GateDetail>()};
            }),
        ports::CapabilityBinding::bind<ports::IAssetResolver>(
            [](const ports::ProviderSetup&) -> domain::Result<std::shared_ptr<ports::IAssetResolver>> {
                return std::shared_ptr<ports::IAssetResolver>{std::make_shared<GateResolver>()};
            }),
    };
    bundle.quality_hints = {domain::Quality::standard};
    return bundle;
}

std::vector<ports::ProviderBundle> gate_sources() { return {gate_source("s3a"), gate_source("s3b")}; }

// What one CDN request saw, for timing and range assertions.
struct Served {
    std::string id;
    std::string origin;
    Clock::time_point at;
    std::optional<std::string> range;
    int status{0};
};

// The CDN of both sources. Faults by the <fault> part of the id:
//   ok       the whole body, Range honored under a matching If-Range
//   cut      the first body stops after cut_at bytes with a connection error
//   503      the first request is answered 503
//   429      the first request is answered 429 with Retry-After: 1
//   changed  the first body is cut; afterwards a different resource (new ETag, other bytes)
//   bad206   the first body is cut; the resume is answered 206 from the wrong offset
//   broken   a damaged FLAC (most audio missing) with a matching Content-Length
//   gone     404 every time
class Cdn final : public ports::HttpTransport {
public:
    // The bytes the id serves now (after a change, the new resource).
    std::string content(const std::string& id) const {
        const std::lock_guard lock{mutex_};
        return current(id);
    }
    // The bytes the id served first.
    std::string original(const std::string& id) const { return make(id, 'a'); }

    void hold(bool on) {
        const std::lock_guard lock{mutex_};
        hold_ = on;
        changed_.notify_all();
    }
    std::size_t holding() const {
        const std::lock_guard lock{mutex_};
        return holding_;
    }
    std::vector<Served> served() const {
        const std::lock_guard lock{mutex_};
        return served_;
    }
    std::size_t peak_total() const {
        const std::lock_guard lock{mutex_};
        return peak_total_;
    }
    std::map<std::string, std::size_t> peak_by_origin() const {
        const std::lock_guard lock{mutex_};
        return peak_origin_;
    }
    std::uint64_t bytes_sent() const {
        const std::lock_guard lock{mutex_};
        return bytes_sent_;
    }

    domain::Result<ports::HttpResponse> request(const ports::HttpRequest& request, const ports::UpstreamLease&,
                                                const ports::OperationContext&) override {
        return ports::HttpResponse{{404, {{"Content-Length", "0"}}, request.url}, {}};
    }

    domain::Result<ports::HttpResponseHead> receive(const ports::HttpRequest& request, const ports::UpstreamLease&,
                                                    const ports::OperationContext& context,
                                                    const ports::HeadConsumer& on_head,
                                                    const ports::ChunkConsumer& on_chunk) override {
        const auto start = request.url.find("://") + 3;
        const auto slash = request.url.find('/', start);
        const auto origin = request.url.substr(0, slash);
        const auto id = request.url.substr(slash + 1);
        const auto fault = id.substr(id.find('-') + 1, id.rfind('-') - id.find('-') - 1);
        const auto range = header(request, "Range");
        const auto if_range = header(request, "If-Range");

        std::string body;
        std::string etag;
        std::size_t from = 0;
        int status = 200;
        std::size_t cut_at = 0; // 0: no cut
        {
            const std::lock_guard lock{mutex_};
            const auto seen = calls_[id]++;
            if (fault == "gone" || (fault == "503" && seen == 0) || (fault == "429" && seen == 0)) {
                status = fault == "gone" ? 404 : fault == "503" ? 503 : 429;
            }
            if ((fault == "cut" || fault == "changed" || fault == "bad206") && seen == 0) {
                cut_at = id.rfind("flac", 0) == 0 ? 8 * 1024 : 40 * 1024;
            }
            if (fault == "changed" && seen > 0) {
                changed_ids_.insert(id);
            }
            body = current(id);
            etag = "\"" + id + (changed_ids_.count(id) != 0 ? "-v2" : "-v1") + "\"";
            if (status == 200 && range && range->rfind("bytes=", 0) == 0 && (!if_range || *if_range == etag)) {
                from = std::stoull(range->substr(6));
                if (fault == "bad206" && seen == 1) {
                    from = from / 2; // answers another range than asked
                }
                status = 206;
            }
            served_.push_back({id, origin, Clock::now(), range, status});
        }
        ports::HttpResponseHead head{status, {}, request.url};
        if (status == 404 || status == 503 || status == 429) {
            head.headers = {{"Content-Length", "0"}};
            if (status == 429) {
                head.headers.push_back({"Retry-After", "1"});
            }
            (void)on_head(head);
            return head;
        }
        if (status == 206) {
            head.headers.push_back({"Content-Range", "bytes " + std::to_string(from) + "-" +
                                                         std::to_string(body.size() - 1) + "/" +
                                                         std::to_string(body.size())});
        }
        head.headers.push_back({"Content-Type", id.rfind("mp3", 0) == 0    ? "audio/mpeg"
                                                : id.rfind("flac", 0) == 0 ? "audio/flac"
                                                                           : "audio/mp4"});
        head.headers.push_back({"Content-Length", std::to_string(body.size() - from)});
        head.headers.push_back({"ETag", etag});
        head.headers.push_back({"Accept-Ranges", "bytes"});
        if (!on_head(head)) {
            return head;
        }

        const Streaming streaming{*this, origin};
        for (auto sent = from; sent < body.size();) {
            if (context.stop.stop_requested()) {
                return domain::Error{ErrorCode::canceled, "the operation was canceled", false, {}};
            }
            if (sent > from && !wait_released(context)) {
                return domain::Error{ErrorCode::canceled, "the operation was canceled", false, {}};
            }
            if (cut_at != 0 && sent >= cut_at) {
                return domain::Error{ErrorCode::upstream_failed, "connection reset by the test CDN", true, {}};
            }
            auto size = std::min<std::size_t>(4 * 1024, body.size() - sent);
            if (cut_at != 0) {
                size = std::min(size, cut_at - sent);
            }
            if (!on_chunk(std::span<const char>{body.data() + sent, size})) {
                head.stopped_by_consumer = true;
                return head;
            }
            sent += size;
            {
                const std::lock_guard lock{mutex_};
                bytes_sent_ += size;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1}); // a transfer takes a while
        }
        return head;
    }

private:
    // Counts a body in flight for the concurrency peaks.
    struct Streaming {
        Streaming(Cdn& cdn, std::string origin) : cdn_(cdn), origin_(std::move(origin)) {
            const std::lock_guard lock{cdn_.mutex_};
            cdn_.peak_total_ = std::max(cdn_.peak_total_, ++cdn_.active_total_);
            cdn_.peak_origin_[origin_] = std::max(cdn_.peak_origin_[origin_], ++cdn_.active_origin_[origin_]);
        }
        ~Streaming() {
            const std::lock_guard lock{cdn_.mutex_};
            --cdn_.active_total_;
            --cdn_.active_origin_[origin_];
        }
        Streaming(const Streaming&) = delete;
        Streaming& operator=(const Streaming&) = delete;
        Cdn& cdn_;
        std::string origin_;
    };

    static std::optional<std::string> header(const ports::HttpRequest& request, std::string_view name) {
        for (const auto& h : request.headers) {
            if (h.name == name) {
                return h.value;
            }
        }
        return std::nullopt;
    }

    static std::string make(const std::string& id, char variant) {
        const auto fill = static_cast<char>(variant + static_cast<char>(std::hash<std::string>{}(id) % 20));
        if (id.rfind("flac", 0) == 0) {
            auto flac = fixture("sine-3s.flac");
            if (id.find("-broken-") != std::string::npos) {
                flac.resize(flac.size() * 2 / 5); // most audio frames are missing
            }
            return flac;
        }
        if (id.rfind("m4a", 0) == 0) {
            return fixture("sine-3s.m4a");
        }
        return mp3(300, fill);
    }

    std::string current(const std::string& id) const {
        return make(id, changed_ids_.count(id) != 0 ? 'A' : 'a');
    }

    bool wait_released(const ports::OperationContext& context) {
        std::unique_lock lock{mutex_};
        if (!hold_) {
            return true;
        }
        ++holding_;
        while (hold_ && !context.stop.stop_requested()) {
            changed_.wait_for(lock, std::chrono::milliseconds{5});
        }
        --holding_;
        return !context.stop.stop_requested();
    }

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::map<std::string, int> calls_;
    std::set<std::string> changed_ids_;
    std::vector<Served> served_;
    std::size_t active_total_{0};
    std::size_t peak_total_{0};
    std::map<std::string, std::size_t> active_origin_;
    std::map<std::string, std::size_t> peak_origin_;
    std::uint64_t bytes_sent_{0};
    bool hold_{false};
    std::size_t holding_{0};
};

// The audio a published file holds, without what tagging added.
struct Payload {
    std::string container;
    std::string bytes;           // MP3: frames; FLAC: from the first frame; MP4: empty
    std::int64_t media_bytes{0}; // MP4 mdat payload
    std::optional<std::int64_t> duration_ms;
};

Payload payload_of(const fs::path& file, const std::string& container) {
    Payload p{container, {}, 0, std::nullopt};
    const ports::OperationContext context{Clock::now() + std::chrono::minutes{1}, {}};
    if (container == "mp3") {
        p.bytes = without_id3v2(read_file(file));
        return p;
    }
    auto reader = audio_processing::format::ByteReader::open(file);
    REQUIRE(reader);
    if (container == "flac") {
        auto flac = audio_processing::format::scan_flac(reader.value(), context);
        REQUIRE(flac);
        p.bytes = read_file(file).substr(static_cast<std::size_t>(flac->audio_offset));
        p.duration_ms = flac->duration_ms;
        return p;
    }
    auto mp4 = audio_processing::format::scan_mp4(reader.value(), context);
    REQUIRE(mp4);
    p.media_bytes = mp4->media_bytes;
    p.duration_ms = mp4->duration_ms;
    return p;
}

std::string accepted(const httplib::Result& created) {
    REQUIRE(created);
    INFO(created->body);
    REQUIRE(created->status == 202);
    return body(created)["data"]["task_id"].get<std::string>();
}

const std::set<std::string> terminal{"completed", "failed", "canceled"};

double percentile(std::vector<double> values, double p) {
    if (values.empty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    const auto at = static_cast<std::size_t>(p * static_cast<double>(values.size() - 1));
    return values[at];
}

// The deployment both gate cases use: three workers, at most two transfers per source
// and per origin, five attempts one second apart.
struct Gate {
    explicit Gate(std::string_view name) : d(name) {
        d.config["downloads"] = {{"workers", 3}, {"per_source", 2}, {"max_attempts", 5}, {"transport_retries", 2}};
        d.config["retry"] = {{"initial_backoff_ms", 1000}, {"multiplier", 1}};
        d.config["upstream"] = {{"per_origin", 2}, {"per_principal", 6}, {"max_total_connections", 6}};
        d.config["destinations"].push_back({{"id", "dst_b"},
                                            {"name", "第二目录"},
                                            {"root", infrastructure::filesystem::path_to_utf8(d.dir / "second")},
                                            {"principals", {"prn_local"}}});
        d.write_config();
        server::CommandHooks hooks;
        hooks.host.bundles = gate_sources();
        const auto initialized = d.run({"init"}, hooks);
        INFO(initialized.err);
        REQUIRE(initialized.code == server::exit_ok);
        token = d.read(d.bootstrap());
    }
    std::unique_ptr<RunningService> start(const std::shared_ptr<Cdn>& cdn) {
        return std::make_unique<RunningService>(d, std::make_shared<infrastructure::observability::MemorySink>(),
                                                gate_sources(), false, cdn);
    }
    Deployment d;
    std::string token;
};

std::string request_body(const std::string& source, const std::string& id, const std::string& destination) {
    return R"({"track":{"source":")" + source + R"(","id":")" + id + R"("},"destination_id":")" + destination +
           R"(","metadata_policy":"required"})";
}

} // namespace

TEST_CASE("T32: faults on a loaded runner end in whole files or clear failures, within the bounds",
          "[gate][t32]") {
    Gate g{"t32-faults"};
    auto cdn = std::make_shared<Cdn>();
    auto service = g.start(cdn);
    auto api = service->client(g.token);
    const auto idle_threads = process_threads();

    struct Planned {
        std::string source;
        std::string id;
        std::string destination;
        bool completes;
    };
    const std::vector<Planned> plan{
        {"s3a", "mp3-ok-1", "dst_main", true},     {"s3a", "mp3-ok-2", "dst_main", true},
        {"s3a", "flac-ok-1", "dst_main", true},    {"s3a", "m4a-ok-1", "dst_main", true},
        {"s3a", "mp3-cut-1", "dst_main", true},    {"s3a", "flac-cut-1", "dst_main", true},
        {"s3a", "mp3-503-1", "dst_main", true},    {"s3a", "mp3-429-1", "dst_main", true},
        {"s3a", "mp3-changed-1", "dst_main", true}, {"s3a", "mp3-bad206-1", "dst_main", true},
        {"s3a", "flac-broken-1", "dst_main", false}, {"s3a", "mp3-gone-1", "dst_main", false},
        {"s3b", "mp3-ok-3", "dst_main", true},     {"s3b", "mp3-ok-4", "dst_main", true},
        {"s3b", "m4a-ok-2", "dst_main", true},     {"s3b", "flac-ok-2", "dst_main", true},
        {"s3b", "mp3-ok-5", "dst_b", true},        {"s3b", "flac-ok-3", "dst_b", true},
    };

    // Everything is accepted while new work is paused, then the second destination goes
    // offline and the queue is released.
    const auto policy = [&](bool paused) {
        const auto changed = api.Patch("/api/v1/admin/download-policy", paused ? R"({"paused":true})" : R"({"paused":false})",
                                       "application/json");
        REQUIRE(changed);
        INFO(changed->body);
        REQUIRE(changed->status == 200);
    };
    policy(true);
    std::map<std::string, std::string> task_of; // id -> task
    int n = 0;
    for (const auto& p : plan) {
        task_of[p.id] = accepted(api.Post("/api/v1/downloads", {{"Idempotency-Key", "t32-faults-" + std::to_string(++n) + "-key"}},
                                          request_body(p.source, p.id, p.destination), "application/json"));
    }
    const auto second = g.d.dir / "second";
    fs::remove_all(second);
    const auto started = Clock::now();
    policy(false);

    // Until every task ends: the control API keeps answering, the offline destination
    // comes back once its tasks had to wait, and threads and latency are sampled.
    std::vector<double> latency_ms;
    std::size_t peak_threads = idle_threads;
    bool restored = false;
    std::map<std::string, json> final;
    while (final.size() < plan.size()) {
        REQUIRE(Clock::now() - started < std::chrono::seconds{90});
        const auto asked = Clock::now();
        const auto listed = api.Get("/api/v1/downloads?limit=100");
        latency_ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - asked).count());
        REQUIRE(listed);
        REQUIRE(listed->status == 200);
        peak_threads = std::max(peak_threads, process_threads());
        const auto items = body(listed)["data"]["items"];
        for (const auto& task : items) {
            const auto id = task["original_track"]["id"].get<std::string>();
            if (!restored && task["destination_id"] == "dst_b" && task["status"] == "waiting_retry") {
                fs::create_directories(second);
                restored = true;
            }
            if (terminal.count(task["status"].get<std::string>()) != 0) {
                final[id] = task;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    const auto elapsed = std::chrono::duration<double>(Clock::now() - started).count();
    CHECK(restored);

    // Every task ended as planned; completed files hold exactly the resource they came
    // from, failed ones published nothing.
    std::set<fs::path> published;
    for (const auto& p : plan) {
        INFO(p.id << " " << final[p.id].dump());
        const auto& task = final[p.id];
        if (!p.completes) {
            CHECK(task["status"] == "failed");
            CHECK(task["result"].is_null());
            CHECK(task["attempt"] == 2); // 两个平台各尝试一次，不重复请求失败平台
            continue;
        }
        REQUIRE(task["status"] == "completed");
        const auto file = body(api.Get("/api/v1/files/" + task["result"]["file_id"].get<std::string>()))["data"];
        CHECK(file["metadata_status"] == "complete");
        const auto root = p.destination == "dst_b" ? second : g.d.dir / "下载";
        const auto path = root / infrastructure::filesystem::path_from_utf8(file["display_name"].get<std::string>());
        REQUIRE(fs::is_regular_file(path));
        published.insert(path);
        const auto container = p.id.substr(0, p.id.find('-')) == "m4a" ? std::string{"mp4"} : p.id.substr(0, p.id.find('-'));
        const auto source_bytes = cdn->content(p.id);
        const auto scratch = g.d.dir / "out" / (p.id + ".source");
        { std::ofstream{scratch, std::ios::binary} << source_bytes; }
        const auto got = payload_of(path, container);
        const auto expected = payload_of(scratch, container);
        CHECK(got.bytes == expected.bytes); // nothing mixed, nothing missing
        CHECK(got.media_bytes == expected.media_bytes);
        CHECK(got.duration_ms == expected.duration_ms);
        if (p.id == "mp3-changed-1") {
            CHECK(got.bytes != cdn->original(p.id)); // the new resource, never the old prefix
        }
    }
    CHECK(final["flac-broken-1"]["error"]["code"] == "AUDIO_INVALID");
    for (const auto* id : {"mp3-503-1", "mp3-429-1"}) {
        CHECK(final[id]["actual_track"]["source"] == "s3b");
        CHECK(final[id]["source_switched"] == true);
    }
    const auto statistics = body(api.Get("/api/v1/source-statistics"))["data"]["items"];
    std::uint64_t successes = 0, failures = 0;
    for (const auto& item : statistics) {
        successes += item["successes"].get<std::uint64_t>();
        failures += item["failures"].get<std::uint64_t>();
    }
    CHECK(successes == 16);
    CHECK(failures == 6); // 503、429 各一次，损坏和消失在两个平台各失败一次
    // Nothing else was published anywhere in the destinations, and no work file is left
    // for completed tasks.
    for (const auto& root : {g.d.dir / "下载", second}) {
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            const auto relative = infrastructure::filesystem::path_to_utf8(fs::relative(entry.path(), root));
            if (!entry.is_regular_file() || relative.rfind(".music-service-work", 0) == 0) {
                continue;
            }
            INFO(relative);
            CHECK(published.count(entry.path()) == 1);
        }
    }

    // Resumes asked for the bytes already kept; the changed and the misplaced 206 were
    // restarted from zero.
    std::map<std::string, std::vector<Served>> by_id;
    for (const auto& s : cdn->served()) {
        by_id[s.id].push_back(s);
    }
    for (const auto* id : {"mp3-cut-1", "flac-cut-1"}) {
        INFO(id);
        REQUIRE(by_id[id].size() >= 2);
        REQUIRE(by_id[id][1].range);
        CHECK(*by_id[id][1].range != "bytes=0-");
        CHECK(by_id[id][1].status == 206);
    }
    CHECK(by_id["mp3-bad206-1"].size() >= 3); // cut, wrong 206, then from the start
    CHECK(by_id["mp3-gone-1"].size() == 2);    // one request per source

    // 429 with Retry-After: 1 kept that task's next request a second away, while the
    // other source went on meanwhile.
    const auto& limited = by_id["mp3-429-1"];
    REQUIRE(limited.size() >= 2);
    CHECK(limited[1].at - limited[0].at >= std::chrono::milliseconds{950});
    std::size_t other_source = 0;
    for (const auto& s : cdn->served()) {
        if (s.origin == "https://cdn-s3b.s3.test" && s.at > limited[0].at && s.at < limited[1].at) {
            ++other_source;
        }
    }
    CHECK(other_source > 0);

    // The configured bounds held: three workers, two per origin (one origin per source).
    CHECK(cdn->peak_total() <= 3);
    for (const auto& [origin, peak] : cdn->peak_by_origin()) {
        INFO(origin);
        CHECK(peak <= 2);
    }
    CHECK(peak_threads <= idle_threads + 4); // no thread per task or per transfer
    CHECK(percentile(latency_ms, 0.95) < 500.0);

    json metrics{{"tasks", plan.size()},
                 {"completed", std::count_if(final.begin(), final.end(),
                                             [](const auto& f) { return f.second["status"] == "completed"; })},
                 {"elapsed_s", elapsed},
                 {"cdn_requests", cdn->served().size()},
                 {"bytes_sent", cdn->bytes_sent()},
                 {"peak_transfers", cdn->peak_total()},
                 {"peak_per_origin", cdn->peak_by_origin()},
                 {"threads_idle", idle_threads},
                 {"threads_peak", peak_threads},
                 {"peak_resident_mib", static_cast<double>(peak_resident_bytes()) / (1024.0 * 1024.0)},
                 {"control_samples", latency_ms.size()},
                 {"control_p50_ms", percentile(latency_ms, 0.5)},
                 {"control_p95_ms", percentile(latency_ms, 0.95)},
                 {"control_max_ms", percentile(latency_ms, 1.0)}};
    std::cout << "T32-METRICS " << metrics.dump() << std::endl;
}

TEST_CASE("T32: a restart under load resumes every transfer without repeating an attempt", "[gate][t32]") {
    Gate g{"t32-restart"};
    auto cdn = std::make_shared<Cdn>();
    const std::vector<std::pair<std::string, std::string>> plan{
        {"s3a", "mp3-ok-11"}, {"s3a", "mp3-ok-12"}, {"s3a", "flac-ok-11"},
        {"s3b", "mp3-ok-13"}, {"s3b", "m4a-ok-11"}, {"s3b", "mp3-ok-14"},
    };
    std::vector<std::string> tasks;
    {
        auto service = g.start(cdn);
        auto api = service->client(g.token);
        cdn->hold(true);
        int n = 0;
        for (const auto& [source, id] : plan) {
            tasks.push_back(accepted(api.Post("/api/v1/downloads",
                                              {{"Idempotency-Key", "t32-restart-" + std::to_string(++n) + "-key"}},
                                              request_body(source, id, "dst_main"), "application/json")));
        }
        const auto until = Clock::now() + std::chrono::seconds{10};
        while (cdn->holding() < 3 && Clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        REQUIRE(cdn->holding() == 3); // as many transfers as workers, the rest queued
    }
    const auto before = cdn->served().size();
    cdn->hold(false);
    auto service = g.start(cdn);
    auto api = service->client(g.token);
    const auto until = Clock::now() + std::chrono::seconds{30};
    std::size_t done = 0;
    json observed = json::object();
    while (done < tasks.size()) {
        INFO("last restart task states: " << observed.dump());
        INFO("CDN requests: " << cdn->served().size() << ", bytes sent: " << cdn->bytes_sent()
                              << ", held transfers: " << cdn->holding());
        REQUIRE(Clock::now() < until);
        done = 0;
        for (const auto& id : tasks) {
            const auto task = body(api.Get("/api/v1/downloads/" + id))["data"];
            observed[id] = task;
            done += terminal.count(task["status"].get<std::string>());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    std::size_t resumed = 0;
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        const auto task = body(api.Get("/api/v1/downloads/" + tasks[i]))["data"];
        INFO(plan[i].second << " " << task.dump());
        REQUIRE(task["status"] == "completed");
        CHECK(task["attempt"] == 1); // a shutdown is not a failed attempt
        const auto file = body(api.Get("/api/v1/files/" + task["result"]["file_id"].get<std::string>()))["data"];
        const auto path = g.d.dir / "下载" / infrastructure::filesystem::path_from_utf8(file["display_name"].get<std::string>());
        const auto container = plan[i].second.rfind("m4a", 0) == 0 ? std::string{"mp4"}
                                                                   : plan[i].second.substr(0, plan[i].second.find('-'));
        const auto scratch = g.d.dir / "out" / (plan[i].second + ".source");
        { std::ofstream{scratch, std::ios::binary} << cdn->content(plan[i].second); }
        CHECK(payload_of(path, container).bytes == payload_of(scratch, container).bytes);
    }
    const auto served = cdn->served();
    for (std::size_t i = before; i < served.size(); ++i) {
        if (served[i].range && *served[i].range != "bytes=0-") {
            ++resumed;
        }
    }
    CHECK(resumed == 3); // the three cut by the shutdown continued where they stopped
}
