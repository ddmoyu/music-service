#pragma once

// Kuwo and 5sing behind one fake transport, as the recorded fixtures show them, for
// the gates that run both real packages in the real process (T22, T41, T28).

#include "service_harness.hpp"

#include "providers/fivesing/bundle.hpp"
#include "providers/kuwo/bundle.hpp"

#include <algorithm>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <optional>

namespace music_service::test::two_platforms {

namespace fs = std::filesystem;

inline const std::string work = "fc-16394286";
inline const std::string primary_host = "https://wsaudiobssdlbig.kugou.com/";
inline const std::string backup_host = "https://wsaudiobssdlbig.cloud.kugou.com/";
inline const std::string lq_file = "23e71848fb13b3d1aa8ab7c19bace467.mp3";
inline const std::string kuwo_cdn = "https://kw-lv.kuwo.cn/";

inline std::optional<std::string> request_header(const ports::HttpRequest& request, std::string_view name) {
    for (const auto& h : request.headers) {
        if (h.name == name) {
            return h.value;
        }
    }
    return std::nullopt;
}

inline std::string fixture(const char* platform, const char* name) {
    std::ifstream file(fs::path{MUSIC_SERVICE_TEST_FIXTURES} / "providers" / platform / name, std::ios::binary);
    REQUIRE(file);
    return {std::istreambuf_iterator<char>(file), {}};
}

// 128 kbit/s MPEG-1 Layer III frames at 44.1 kHz.
inline std::string mp3(std::size_t frames) {
    std::string bytes;
    for (std::size_t i = 0; i < frames; ++i) {
        const auto padding = i % 3 == 0;
        std::string frame(static_cast<std::size_t>(144 * 128 * 1000 / 44100 + (padding ? 1 : 0)), '\x55');
        frame[0] = '\xFF';
        frame[1] = '\xFB';
        frame[2] = static_cast<char>((9U << 4) | (padding ? 0x02U : 0U));
        frame[3] = '\x40';
        bytes += frame;
    }
    return bytes;
}

inline std::string without_id3v2(const std::string& bytes) {
    if (bytes.size() < 10 || bytes.compare(0, 3, "ID3") != 0) {
        return bytes;
    }
    const auto size = (std::size_t{static_cast<unsigned char>(bytes[6]) & 0x7FU} << 21) |
                      (std::size_t{static_cast<unsigned char>(bytes[7]) & 0x7FU} << 14) |
                      (std::size_t{static_cast<unsigned char>(bytes[8]) & 0x7FU} << 7) |
                      std::size_t{static_cast<unsigned char>(bytes[9]) & 0x7FU};
    return bytes.substr(std::min(bytes.size(), 10 + size));
}

// 5sing and Kuwo as the fixtures show them. The getSongUrl answer for fc-16394286
// declares the lq file with the size of the synthetic MP3; the CDN serves it under that
// file name at both hosts, Last-Modified as the only validator, as the real CDN does.
// fail_primary makes the primary host answer 503. Kuwo answers for rid 78932517 with
// its own MP3 (the 176 s it declares) behind a strong ETag, honoring Range under a
// matching If-Range; fail_kuwo_search makes its search answer 503, and hold() stops a
// Kuwo audio body after its first chunk until released or the operation is stopped.
class Upstream final : public ports::HttpTransport {
public:
    explicit Upstream(std::string audio, std::string kuwo_audio = {})
        : audio_(std::move(audio)), kuwo_audio_(std::move(kuwo_audio)) {}

    // Audio bodies requested so far under a CDN prefix.
    std::size_t audio_gets(const std::string& prefix) const {
        const std::lock_guard lock{mutex_};
        return static_cast<std::size_t>(std::count_if(requests_.begin(), requests_.end(), [&](const auto& r) {
            return r.method == ports::HttpMethod::get && r.url.rfind(prefix, 0) == 0;
        }));
    }
    std::size_t api_requests(const std::string& prefix) const {
        const std::lock_guard lock{mutex_};
        return static_cast<std::size_t>(std::count_if(requests_.begin(), requests_.end(),
                                                      [&](const auto& r) { return r.url.rfind(prefix, 0) == 0; }));
    }

    void fail_primary(bool on) {
        const std::lock_guard lock{mutex_};
        fail_primary_ = on;
    }
    void fail_kuwo_search(bool on) {
        const std::lock_guard lock{mutex_};
        fail_kuwo_search_ = on;
    }
    void hold(bool on) {
        const std::lock_guard lock{mutex_};
        hold_ = on;
        changed_.notify_all();
    }
    bool wait_holding(std::chrono::milliseconds timeout = std::chrono::seconds{10}) {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, timeout, [this] { return holding_; });
    }
    std::vector<ports::HttpRequest> requests() const {
        const std::lock_guard lock{mutex_};
        return requests_;
    }
    void clear() {
        const std::lock_guard lock{mutex_};
        requests_.clear();
    }

    domain::Result<ports::HttpResponse> request(const ports::HttpRequest& request, const ports::UpstreamLease&,
                                                const ports::OperationContext&) override {
        record(request);
        const auto& url = request.url;
        if (url.rfind("http://search.5sing.kugou.com/home/json?", 0) == 0) {
            return answer(200, fixture("fivesing", "search-qiannianjiu-p1.json"), url);
        }
        if (url == "http://mobileapi.5sing.kugou.com/song/newget?songid=16394286&songtype=fc") {
            return answer(200, fixture("fivesing", "newget-fc-16394286.json"), url);
        }
        if (url.rfind("http://mobileapi.5sing.kugou.com/song/newget?", 0) == 0) {
            return answer(200, fixture("fivesing", "newget-missing.json"), url);
        }
        if (url == "http://mobileapi.5sing.kugou.com/song/getSongUrl?songid=16394286&songtype=fc") {
            auto body = fixture("fivesing", "url-fc-16394286.json");
            const std::string declared = R"("lqsize":"5324844")";
            body.replace(body.find(declared), declared.size(), R"("lqsize":")" + std::to_string(audio_.size()) + "\"");
            return answer(200, std::move(body), url);
        }
        if (url.rfind("http://mobileapi.5sing.kugou.com/song/getSongUrl?", 0) == 0) {
            return answer(200, fixture("fivesing", "url-missing.json"), url);
        }
        if (url.rfind("https://www.kuwo.cn/search/searchMusicBykeyWord?", 0) == 0) {
            if (const std::lock_guard lock{mutex_}; fail_kuwo_search_) {
                return answer(503, "busy", url);
            }
            return answer(200, fixture("kuwo", "search-qingtian-pn0-rn3.json"), url);
        }
        if (url == "https://www.kuwo.cn/play_detail/78932517") {
            return answer(200, fixture("kuwo", "play-detail-78932517.html"), url);
        }
        if (url.rfind("https://nplserver.kuwo.cn/pl.svc?op=getlistinfo&pid=2234580349&", 0) == 0) {
            return answer(200, fixture("kuwo", "playlist-2234580349-pn0-rn2.json"), url);
        }
        if (url.rfind("https://wapi.kuwo.cn/api/pc/bang/list?", 0) == 0) {
            return answer(200, fixture("kuwo", "bang-list.json"), url);
        }
        if (url.rfind("https://kbangserver.kuwo.cn/ksong.s?", 0) == 0) {
            return answer(200, fixture("kuwo", "chart-16-pn0-rn2.json"), url);
        }
        if (url.rfind("https://mobi.kuwo.cn/mobi.s?", 0) == 0) {
            const auto found = url.find("&rid=78932517&") != std::string::npos;
            return answer(200, fixture("kuwo", found ? "audio-78932517-standard.json" : "audio-missing.json"), url);
        }
        if (request.method == ports::HttpMethod::head) {
            auto head = cdn_head(url);
            return ports::HttpResponse{std::move(head), {}};
        }
        return answer(404, "not found", url);
    }

    domain::Result<ports::HttpResponseHead> receive(const ports::HttpRequest& request, const ports::UpstreamLease&,
                                                    const ports::OperationContext& context,
                                                    const ports::HeadConsumer& on_head,
                                                    const ports::ChunkConsumer& on_chunk) override {
        record(request);
        const bool kuwo = request.url.rfind(kuwo_cdn, 0) == 0;
        std::size_t from = 0;
        auto head = kuwo ? kuwo_head(request, from) : cdn_head(request.url);
        if (!on_head(head) || (head.status != 200 && head.status != 206)) {
            return head;
        }
        const auto& bytes = kuwo ? kuwo_audio_ : audio_;
        for (auto sent = from; sent < bytes.size();) {
            if (kuwo && sent > from && !wait_released(context)) {
                return domain::Error{domain::ErrorCode::canceled, "the operation was canceled", false, {}};
            }
            const auto size = std::min<std::size_t>(16 * 1024, bytes.size() - sent);
            if (!on_chunk(std::span<const char>{bytes.data() + sent, size})) {
                head.stopped_by_consumer = true;
                return head;
            }
            sent += size;
        }
        return head;
    }

    static constexpr const char* kuwo_etag = "\"kw-78932517\"";

private:
    ports::HttpResponseHead kuwo_head(const ports::HttpRequest& request, std::size_t& from) const {
        ports::HttpResponseHead head{200, {}, request.url};
        const auto range = request_header(request, "Range");
        const auto if_range = request_header(request, "If-Range");
        if (range && range->rfind("bytes=", 0) == 0 && (!if_range || *if_range == kuwo_etag)) {
            from = std::stoull(range->substr(6));
            head.status = 206;
            head.headers.push_back({"Content-Range", "bytes " + std::to_string(from) + "-" +
                                                         std::to_string(kuwo_audio_.size() - 1) + "/" +
                                                         std::to_string(kuwo_audio_.size())});
        }
        head.headers.push_back({"Content-Type", "audio/mpeg"});
        head.headers.push_back({"Content-Length", std::to_string(kuwo_audio_.size() - from)});
        head.headers.push_back({"ETag", kuwo_etag});
        head.headers.push_back({"Accept-Ranges", "bytes"});
        return head;
    }

    // Blocks while held; false when the operation was stopped meanwhile.
    bool wait_released(const ports::OperationContext& context) {
        std::unique_lock lock{mutex_};
        while (hold_ && !context.stop.stop_requested()) {
            holding_ = true;
            changed_.notify_all();
            changed_.wait_for(lock, std::chrono::milliseconds{5});
        }
        holding_ = false;
        return !context.stop.stop_requested();
    }

    ports::HttpResponseHead cdn_head(const std::string& url) {
        const std::lock_guard lock{mutex_};
        const bool primary = url.rfind(primary_host, 0) == 0;
        const bool known = (primary || url.rfind(backup_host, 0) == 0) && url.ends_with("/" + lq_file);
        if (!known) {
            return {404, {{"Content-Length", "0"}}, url};
        }
        if (primary && fail_primary_) {
            return {503, {{"Content-Length", "0"}}, url};
        }
        return {200,
                {{"Content-Type", "audio/mpeg"},
                 {"Content-Length", std::to_string(audio_.size())},
                 {"Last-Modified", "Sun, 18 Nov 2018 03:51:43 GMT"},
                 {"Accept-Ranges", "bytes"}},
                url};
    }

    void record(const ports::HttpRequest& request) {
        const std::lock_guard lock{mutex_};
        requests_.push_back(request);
    }

    static ports::HttpResponse answer(int status, std::string body, const std::string& url) {
        return {{status,
                 {{"Content-Type", "application/json; charset=utf-8"}, {"Content-Length", std::to_string(body.size())}},
                 url},
                std::move(body)};
    }

    std::string audio_;
    std::string kuwo_audio_;
    mutable std::mutex mutex_;
    std::vector<ports::HttpRequest> requests_;
    std::condition_variable changed_;
    bool fail_primary_{false};
    bool fail_kuwo_search_{false};
    bool hold_{false};
    bool holding_{false};
};

inline std::vector<ports::ProviderBundle> both() { return {providers::kuwo::make_bundle(), providers::fivesing::make_bundle()}; }

inline json wait_status(httplib::Client& api, const std::string& task_id, std::string_view status) {
    const auto until = std::chrono::steady_clock::now() + 30s;
    json task;
    while (std::chrono::steady_clock::now() < until) {
        task = body(api.Get("/api/v1/downloads/" + task_id))["data"];
        if (task["status"] == std::string{status} || task["status"] == "failed") {
            return task;
        }
        std::this_thread::sleep_for(20ms);
    }
    FAIL("task " << task_id << " stayed " << task.dump());
    return task;
}

inline std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

inline std::string code_of(const httplib::Result& response) { return body(response)["error"]["code"].get<std::string>(); }

} // namespace music_service::test::two_platforms
