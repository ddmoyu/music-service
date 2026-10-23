#include "../fixtures/audio/z3d_fixture.hpp"
#include "providers/migu/bundle.hpp"
#include "service_harness.hpp"
#include <tpropertymap.h>
#include <wavfile.h>

using namespace music_service;
using namespace music_service::test;
namespace fs = std::filesystem;

namespace {
std::string read_z3d_bytes(const fs::path& p) {
    std::ifstream in{p, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, {}};
}
class Z3dUpstream final : public ports::HttpTransport {
  public:
    explicit Z3dUpstream(bool damaged) : bytes_(z3d::encrypt(z3d::wav(), true)) {
        if (damaged)
            bytes_.pop_back();
    }
    domain::Result<ports::HttpResponse> request(const ports::HttpRequest& r,
                                                const ports::UpstreamLease&,
                                                const ports::OperationContext&) override {
        json song{{"resourceType", "2"},
                  {"contentId", "600900000000000001"},
                  {"copyrightId", "600000A0001"},
                  {"songId", "123"},
                  {"songName", "Synthetic Z3D"},
                  {"length", "00:00:02"},
                  {"singerList", json::array({{{"id", "45"}, {"name", "Synthetic artist"}}})},
                  {"needEncrypt", "1"},
                  {"auditionsType", "00"},
                  {"chargeAuditions", "0"},
                  {"z3dCode",
                   {{"formatType", "Z3D"},
                    {"androidFileType", "wav"},
                    {"androidFileKey", z3d::file_key}}}};
        json out{{"code", "000000"}};
        if (r.url.find("resourceinfo.do?") != std::string::npos)
            out["resource"] = json::array({song});
        else if (r.url.find("listen-url?") != std::string::npos)
            out["data"] = {{"formatType", "Z3D"},
                           {"url", "https://freetyst.nf.migu.cn/synthetic.z3d"},
                           {"songItem", song}};
        else
            return domain::Error::validation("test", "unexpected upstream request");
        return ports::HttpResponse{{200, {{"Content-Type", "application/json"}}, r.url},
                                   out.dump()};
    }
    domain::Result<ports::HttpResponseHead> receive(const ports::HttpRequest& r,
                                                    const ports::UpstreamLease&,
                                                    const ports::OperationContext&,
                                                    const ports::HeadConsumer& head,
                                                    const ports::ChunkConsumer& chunk) override {
        ports::HttpResponseHead h{200,
                                  {{"Content-Length", std::to_string(bytes_.size())},
                                   {"Content-Type", "application/octet-stream"}},
                                  r.url};
        if (head(h))
            for (std::size_t at = 0; at < bytes_.size(); at += 777)
                if (!chunk(std::string_view{bytes_}.substr(at, 777))) {
                    h.stopped_by_consumer = true;
                    break;
                }
        return h;
    }

  private:
    std::string bytes_;
};
} // namespace

TEST_CASE(
    "Migu HTTP download passes transient Z3D key through processing and publishes verified WAV",
    "[migu][z3d][gate]") {
    bool damaged = false;
    SECTION("valid encrypted download") {}
    SECTION("corrupt download is never published") { damaged = true; }
    Deployment d{"migu-z3d-http"};
    server::CommandHooks hooks;
    hooks.host.bundles = std::vector<ports::ProviderBundle>{providers::migu::make_bundle()};
    REQUIRE(d.run({"init"}, hooks).code == server::exit_ok);
    const auto sink = std::make_shared<infrastructure::observability::MemorySink>();
    RunningService service{d, sink, hooks.host.bundles, false,
                           std::make_shared<Z3dUpstream>(damaged)};
    auto api = service.client(d.read(d.bootstrap()));
    const auto created = api.Post(
        "/api/v1/downloads", {{"Idempotency-Key", "migu-z3d-download-test"}},
        R"({"track":{"source":"migu","id":"600900000000000001"},"metadata_policy":"required"})",
        "application/json");
    REQUIRE(created);
    INFO(created->body);
    REQUIRE(created->status == 202);
    const auto id = body(created)["data"]["task_id"].get<std::string>();
    json task;
    const auto until = std::chrono::steady_clock::now() + 20s;
    do {
        task = body(api.Get("/api/v1/downloads/" + id))["data"];
        CHECK(task.dump().find(z3d::file_key) == std::string::npos);
        if (task["status"] == "completed" || task["status"] == "failed")
            break;
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < until);
    INFO(task.dump());
    if (damaged) {
        REQUIRE(task["status"] == "failed");
        for (const auto& entry : fs::directory_iterator(d.dir / "下载"))
            CHECK(entry.path().extension() != ".wav");
    } else {
        REQUIRE(task["status"] == "completed");
        CHECK(task["result"]["processing"]["processor"] == "audio.migu-z3d");
        const auto file = api.Get("/api/v1/files/" + task["result"]["file_id"].get<std::string>());
        REQUIRE(file);
        REQUIRE(file->status == 200);
        const auto info = body(file)["data"];
        CHECK(info["container"] == "wav");
        CHECK(info["content_type"] == "audio/wav");
        CHECK(info["quality"].is_null());
        CHECK(file->body.find(z3d::file_key) == std::string::npos);
        const auto path =
            d.dir / "下载" /
            infrastructure::filesystem::path_from_utf8(info["display_name"].get<std::string>());
        TagLib::RIFF::WAV::File wav{path.c_str()};
        REQUIRE(wav.isValid());
        CHECK(wav.properties()["TITLE"].toString().to8Bit(true) == "Synthetic Z3D");
        CHECK_FALSE(fs::exists(d.dir / "下载" / ".music-service-work" / id));
    }
    CHECK(service.stop().code == server::exit_ok);
    for (const auto& record : sink->records())
        for (const auto& field : record.fields)
            if (const auto* value = std::get_if<std::string>(&field.value))
                CHECK(value->find(z3d::file_key) == std::string::npos);
    for (const auto& entry : fs::recursive_directory_iterator(d.dir / "data"))
        if (entry.is_regular_file())
            CHECK(read_z3d_bytes(entry.path()).find(z3d::file_key) == std::string::npos);
}
