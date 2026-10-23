// Real HTTP/control host and processing pool; only the upstream is scripted.
#include "service_harness.hpp"
#include <mpegfile.h>
#include <tpropertymap.h>
#include <tvariant.h>
#include <cstdlib>

using namespace music_service;
using namespace music_service::test;
namespace fs = std::filesystem;

namespace {
std::string environment(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0) return {};
    const std::string result = value ? value : "";
    std::free(value);
    return result;
#else
    const auto* value = std::getenv(name);
    return value ? value : "";
#endif
}
std::string read_bytes(const fs::path& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, {}};
}
class NcmUpstream final : public ports::HttpTransport {
public:
    explicit NcmUpstream(std::string bytes) : bytes_(std::move(bytes)) {}
    domain::Result<ports::HttpResponse> request(const ports::HttpRequest&, const ports::UpstreamLease&,
                                               const ports::OperationContext&) override {
        return domain::Error::validation("test", "unexpected metadata request");
    }
    domain::Result<ports::HttpResponseHead> receive(const ports::HttpRequest& request, const ports::UpstreamLease&,
        const ports::OperationContext& context, const ports::HeadConsumer& on_head,
        const ports::ChunkConsumer& on_chunk) override {
        ports::HttpResponseHead head{200, {{"Content-Length", std::to_string(bytes_.size())},
            {"Content-Type", "audio/mpeg"}}, request.url};
        if (!on_head(head)) return head;
        for (std::size_t at = 0; at < bytes_.size(); at += 65536) {
            if (context.stop.stop_requested())
                return domain::Error{domain::ErrorCode::canceled, "canceled", false, {}};
            if (!on_chunk({bytes_.data() + at, std::min<std::size_t>(65536, bytes_.size() - at)})) {
                head.stopped_by_consumer = true;
                break;
            }
        }
        return head;
    }
private:
    std::string bytes_;
};
}

TEST_CASE("HTTP downloads automatically decrypt NCM and export its metadata without forced embedding", "[ncm][gate]") {
    Deployment d{"ncm-http"};
    d.config["downloads"]["output"] = {{"embed_cover", false}, {"embed_lyrics", false},
        {"export_cover", true}, {"export_lyrics", true}, {"sidecars_required", true}};
    d.write_config();
    server::CommandHooks hooks;
    hooks.host.bundles = std::vector<ports::ProviderBundle>{mock_combo("ncm")};
    REQUIRE(d.run({"init"}, hooks).code == server::exit_ok);
    const auto fixtures = fs::path{MUSIC_SERVICE_TEST_FIXTURES} / "audio";
    auto transport = std::make_shared<NcmUpstream>(read_bytes(fixtures / "synthetic-mp3.ncm"));
    RunningService service{d, std::make_shared<infrastructure::observability::MemorySink>(),
        hooks.host.bundles, false, transport};
    auto api = service.client(d.read(d.bootstrap()));
    const auto created = api.Post("/api/v1/downloads", {{"Idempotency-Key", "ncm-auto-download"}},
        R"({"track":{"source":"ncm","id":"t1"},"metadata_policy":"required"})", "application/json");
    REQUIRE(created);
    REQUIRE(created->status == 202);
    const auto id = body(created)["data"]["task_id"].get<std::string>();
    json task;
    const auto until = std::chrono::steady_clock::now() + 20s;
    do {
        task = body(api.Get("/api/v1/downloads/" + id))["data"];
        if (task["status"] == "completed" || task["status"] == "failed") break;
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < until);
    INFO(task.dump());
    REQUIRE(task["status"] == "completed");
    CHECK(task["result"]["processing"]["processor"] == "audio.ncm");
    const auto root = d.dir / "下载" / "detail.files";
    CHECK(read_bytes(root / "detail.png") == read_bytes(fixtures / "cover.png"));
    CHECK(read_bytes(root / "detail.lrc") == "[00:00.00]Synthetic lyric\n");
    {
        TagLib::MPEG::File audio{(root / "detail.mp3").c_str()};
        CHECK(audio.properties()["TITLE"].toString().to8Bit(true) == "detail");
        CHECK(audio.properties()["ALBUM"].toString().to8Bit(true) == "Synthetic album");
        CHECK_FALSE(audio.properties().contains("LYRICS"));
        CHECK(audio.complexProperties("PICTURE").isEmpty());
    }
    const auto search = api.Get("/api/v1/search/tracks?q=test&sources=ncm&limit=1");
    REQUIRE(search);
    CHECK(search->status == 200);
    CHECK_FALSE(fs::exists(d.dir / "下载" / ".music-service-work" / id));
    CHECK(service.stop().code == server::exit_ok);
}

TEST_CASE("a user supplied real NCM completes an ordinary HTTP download with identical verified plaintext", "[.][ncm-real]") {
    // Both paths are explicit opt-in local inputs; no commercial sample enters Git.
    const auto sample = environment("MUSIC_SERVICE_NCM_SAMPLE");
    const auto expected = environment("MUSIC_SERVICE_NCM_EXPECTED");
    if (sample.empty() || expected.empty()) SKIP("set MUSIC_SERVICE_NCM_SAMPLE and MUSIC_SERVICE_NCM_EXPECTED");
    const auto encrypted = read_bytes(infrastructure::filesystem::path_from_utf8(sample));
    const auto plaintext = read_bytes(infrastructure::filesystem::path_from_utf8(expected));
    REQUIRE(encrypted.starts_with("CTENFDAM"));
    REQUIRE_FALSE(plaintext.empty());
    Deployment d{"ncm-real-http"};
    server::CommandHooks hooks;
    hooks.host.bundles = std::vector<ports::ProviderBundle>{mock_combo("ncm")};
    REQUIRE(d.run({"init"}, hooks).code == server::exit_ok);
    RunningService service{d, std::make_shared<infrastructure::observability::MemorySink>(),
        hooks.host.bundles, false, std::make_shared<NcmUpstream>(encrypted)};
    auto api = service.client(d.read(d.bootstrap()));
    const auto created = api.Post("/api/v1/downloads", {{"Idempotency-Key", "ncm-real-download"}},
        R"({"track":{"source":"ncm","id":"real"},"metadata_policy":"skip"})", "application/json");
    REQUIRE(created);
    REQUIRE(created->status == 202);
    const auto id = body(created)["data"]["task_id"].get<std::string>();
    json task;
    const auto until = std::chrono::steady_clock::now() + 30s;
    bool queried_during_processing = false;
    do {
        task = body(api.Get("/api/v1/downloads/" + id))["data"];
        if (task["status"] == "completed" || task["status"] == "failed") break;
        if (task["status"] == "processing" && !queried_during_processing) {
            const auto start = std::chrono::steady_clock::now();
            const auto search = api.Get("/api/v1/search/tracks?q=test&sources=ncm&limit=1");
            REQUIRE(search);
            CHECK(search->status == 200);
            const auto elapsed = std::chrono::steady_clock::now() - start;
            CHECK(elapsed < 2s);
            SUCCEED("search while native NCM processing: " << std::chrono::duration<double, std::milli>(elapsed).count() << " ms");
            queried_during_processing = true;
        }
        std::this_thread::sleep_for(5ms);
    } while (std::chrono::steady_clock::now() < until);
    INFO(task.dump());
    REQUIRE(task["status"] == "completed");
    CHECK(queried_during_processing);
    CHECK(task["result"]["processing"]["processor"] == "audio.ncm");
    const bool identical = read_bytes(d.dir / "下载" / "detail.mp3") == plaintext;
    CHECK(identical); // do not dump the user's audio bytes in assertion diagnostics
    CHECK(service.stop().code == server::exit_ok);
}
