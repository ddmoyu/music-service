// Published files over the control and media listeners (T19): file metadata, media-access
// descriptions and tickets, Bearer and ticket reads with single ranges, conditional
// requests, missing files, owner checks (A23), response slots released on
// disconnect (A24, local files) and file access without the online proxy (A33).

#include "../unit/domain/test_support.hpp"
#include "service_harness.hpp"

#include "downloads/finalizer.hpp"
#include "infrastructure/clock.hpp"
#include "infrastructure/filesystem/destination_files.hpp"
#include "infrastructure/observability/correlation.hpp"
#include "infrastructure/storage/sqlite/schema.hpp"
#include "infrastructure/storage/sqlite/task_repositories.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace music_service;
using namespace music_service::test;
namespace fs = std::filesystem;
namespace sqlite = infrastructure::storage::sqlite;
using domain::TaskStatus;
using infrastructure::filesystem::path_from_utf8;

namespace {

std::string pattern(std::size_t size) {
    std::string bytes(size, '\0');
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<char>('a' + i % 26);
    }
    return bytes;
}

// A deployment serving media, with files published for prn_local.
struct MediaDeployment {
    explicit MediaDeployment(std::string_view name, json media_limits = json::object()) : d{name} {
        d.config["media"] = {{"enabled", true}, {"port", 0}};
        d.config["features"] = {{"file_access", true}};
        if (!media_limits.empty()) {
            d.config["media_limits"] = std::move(media_limits);
        }
        d.write_config();
        REQUIRE(d.run({"init"}).code == server::exit_ok);
        owner_token = d.read(d.bootstrap());
        const auto created = d.run({"principal", "create", "--name", "other"});
        other = created.out.substr(0, created.out.find('\t'));
        REQUIRE(d.run({"token", "issue", "--principal", other, "--scopes", "files:read,media:read", "--output",
                       infrastructure::filesystem::path_to_utf8(d.dir / "out" / "other.txt")})
                    .code == server::exit_ok);
        other_token = d.read(d.dir / "out" / "other.txt");
        // The first start binds the destination; files are published while it is stopped.
        RunningService{d, std::make_shared<infrastructure::observability::MemorySink>(), bundles(), true};
        root = unwrap(infrastructure::filesystem::canonical_directory(d.dir / "下载"));
    }

    static std::vector<ports::ProviderBundle> bundles() { return {mock_combo("both")}; }

    // What a completed download leaves: the file, its asset and the completed task.
    std::string publish(const std::string& relative_path, const std::string& bytes, std::string_view track_id) {
        auto clock = std::make_shared<infrastructure::SystemClock>();
        auto database = sqlite::Database::open(d.dir / "data" / "service.sqlite3", {}).value();
        const sqlite::RepositoryContext ctx{database.get(), clock,
                                            std::make_shared<infrastructure::observability::IdGenerator>()};
        sqlite::SqliteTaskRepository tasks{ctx, sqlite::TaskRepositoryOptions{}};
        sqlite::SqlitePublicationRepository publications{ctx};
        domain::DownloadIntentDraft draft{"prn_local",
                                          track("both", track_id),
                                          {anonymous("prn_local", "both")},
                                          {},
                                          domain::MetadataPolicy::best_effort,
                                          {"dst_main", "d1", "n1", "m1"}};
        auto intent = unwrap(domain::NormalizedDownloadIntent::normalize(std::move(draft)));
        REQUIRE(tasks.create({std::move(intent), "media-key-" + std::string{track_id}, "h", false}));
        auto task = *unwrap(tasks.claim_next(clock->now()));
        for (const auto to : {TaskStatus::downloading, TaskStatus::processing}) {
            task = unwrap(tasks.transition({task.task_id, *task.execution_id, task.version, to, {}, {}, {}}));
        }
        const auto path = root / path_from_utf8(relative_path);
        fs::create_directories(path.parent_path());
        std::ofstream{path, std::ios::binary} << bytes;
        infrastructure::filesystem::LocalDestinationFiles files{{{"dst_main", root}}};
        const auto file = *unwrap(files.fingerprint("dst_main", relative_path));
        task = unwrap(publications.begin({task.task_id, *task.execution_id, "dst_main", ".music-service-work/x/in.part",
                                          relative_path, "{}", "moved"},
                                         task.version));
        const auto properties = json{{"actual_track", {{"source", "both"}, {"id", track_id}}},
                                     {"actual_quality", "standard"},
                                     {"metadata_status", "failed"}};
        auto done = unwrap(publications.complete(task.task_id, *task.execution_id, task.version,
                                                 {"prn_local", "dst_main", relative_path, file.size_bytes, "mp3",
                                                  downloads::file_etag(file), properties.dump()},
                                                 "{}"));
        return done.asset.id;
    }

    Deployment d;
    std::string owner_token;
    std::string other;
    std::string other_token;
    fs::path root;
};

httplib::Client media_client(const RunningService& service, const std::string& token = {}) {
    const auto& url = *service.report.media_base_url;
    httplib::Client c("127.0.0.1", std::stoi(url.substr(url.rfind(':') + 1)));
    c.set_connection_timeout(3s);
    c.set_read_timeout(10s);
    if (!token.empty()) {
        c.set_bearer_token_auth(token);
    }
    return c;
}

std::string path_of(const std::string& url) {
    const auto scheme = url.find("://");
    return url.substr(url.find('/', scheme + 3));
}

} // namespace

TEST_CASE("published files are described and read with single ranges", "[media][server]") {
    MediaDeployment m{"media-files"};
    const auto bytes = pattern(1000);
    const auto file_id = m.publish("歌手/歌曲.mp3", bytes, "1");
    auto sink = std::make_shared<infrastructure::observability::MemorySink>();
    RunningService service{m.d, sink, MediaDeployment::bundles(), true};
    REQUIRE(service.report.media_base_url);
    auto api = service.client(m.owner_token);

    const auto info = body(api.Get("/api/v1/info"))["data"];
    CHECK(info["features"]["file_access"] == true);
    CHECK(info["features"]["media_proxy"] == false);
    CHECK(info["media_base_url"] == *service.report.media_base_url);

    // Metadata: no server path, a link to the bytes, a weak ETag for the description.
    const auto described = api.Get("/api/v1/files/" + file_id);
    REQUIRE(described->status == 200);
    const auto file = body(described)["data"];
    CHECK(file["display_name"] == "歌曲.mp3");
    CHECK(file["size_bytes"] == 1000);
    CHECK(file["container"] == "mp3");
    CHECK(file["content_type"] == "audio/mpeg");
    CHECK(file["actual_track"]["id"] == "1");
    CHECK(file["quality"] == "standard");
    CHECK(file["metadata_status"] == "failed");
    CHECK(file["media_url"] == *service.report.media_base_url + "/media/v1/files/" + file_id);
    CHECK(described->body.find(infrastructure::filesystem::path_to_utf8(m.root)) == std::string::npos);
    CHECK(api.Get("/api/v1/files/" + file_id, httplib::Headers{{"If-None-Match", described->get_header_value("ETag")}})
              ->status == 304);

    // The completed task links to it.
    const auto tasks = body(api.Get("/api/v1/downloads?status=completed"))["data"]["items"];
    REQUIRE(tasks.size() == 1);
    CHECK(tasks[0]["result"]["file_id"] == file_id);
    CHECK(tasks[0]["result"]["links"]["file"] == "/api/v1/files/" + file_id);

    // Bearer reads.
    auto media = media_client(service, m.owner_token);
    const auto whole = media.Get("/media/v1/files/" + file_id);
    REQUIRE(whole->status == 200);
    CHECK(whole->body == bytes);
    CHECK(whole->get_header_value("Accept-Ranges") == "bytes");
    CHECK(whole->get_header_value("Content-Type") == "audio/mpeg");
    const auto etag = whole->get_header_value("ETag");
    CHECK(etag == file["etag"].get<std::string>());
    CHECK(whole->get_header_value("Content-Disposition") ==
          "attachment; filename=\"______.mp3\"; filename*=UTF-8''%E6%AD%8C%E6%9B%B2.mp3");

    const auto range = media.Get("/media/v1/files/" + file_id, httplib::Headers{{"Range", "bytes=10-19"}});
    CHECK(range->status == 206);
    CHECK(range->body == bytes.substr(10, 10));
    CHECK(range->get_header_value("Content-Range") == "bytes 10-19/1000");
    const auto open_ended = media.Get("/media/v1/files/" + file_id, httplib::Headers{{"Range", "bytes=990-"}});
    CHECK(open_ended->status == 206);
    CHECK(open_ended->body == bytes.substr(990));
    const auto suffix = media.Get("/media/v1/files/" + file_id, httplib::Headers{{"Range", "bytes=-5"}});
    CHECK(suffix->status == 206);
    CHECK(suffix->body == bytes.substr(995));
    const auto outside = media.Get("/media/v1/files/" + file_id, httplib::Headers{{"Range", "bytes=1000-"}});
    CHECK(outside->status == 416);
    CHECK(outside->get_header_value("Content-Range") == "bytes */1000");
    CHECK(outside->body.empty());
    const auto several = media.Get("/media/v1/files/" + file_id, httplib::Headers{{"Range", "bytes=0-1,5-6"}});
    CHECK(several->status == 400);
    CHECK(error_code(several) == "MULTIPLE_RANGES_UNSUPPORTED");
    const auto stale =
        media.Get("/media/v1/files/" + file_id, httplib::Headers{{"Range", "bytes=0-9"}, {"If-Range", "\"other\""}});
    CHECK(stale->status == 200); // another representation: the whole file, never a piece
    CHECK(stale->body == bytes);
    CHECK(media.Get("/media/v1/files/" + file_id, httplib::Headers{{"If-None-Match", etag}})->status == 304);
    const auto head = media.Head("/media/v1/files/" + file_id, httplib::Headers{{"Range", "bytes=0-9"}});
    CHECK(head->status == 200);
    CHECK(head->get_header_value("Content-Length") == "1000");
    CHECK(head->body.empty());

    // Scopes and authentication.
    CHECK(media_client(service).Get("/media/v1/files/" + file_id)->status == 401);
    CHECK(media_client(service, "mst_not-a-token").Get("/media/v1/files/" + file_id)->status == 401);

    // A ticket: inline, no Bearer, private.
    const auto access =
        api.Post("/api/v1/media-access", R"({"target":{"kind":"file","id":")" + file_id + R"("}})", "application/json");
    REQUIRE(access->status == 200);
    const auto description = body(access)["data"];
    CHECK(description["delivery"] == "proxy");
    CHECK(description["auth"] == "url");
    CHECK(description["range_supported"] == true);
    CHECK(description["actual_track"]["id"] == "1");
    CHECK_FALSE(description["expires_at"].is_null());
    const auto ticket_path = path_of(description["url"].get<std::string>());
    CHECK(ticket_path.rfind("/media/v1/tickets/mtk_", 0) == 0);
    const auto by_ticket = media_client(service).Get(ticket_path, httplib::Headers{{"Range", "bytes=0-3"}});
    CHECK(by_ticket->status == 206);
    CHECK(by_ticket->body == "abcd");
    CHECK(by_ticket->get_header_value("Cache-Control") == "no-store");
    CHECK(by_ticket->get_header_value("Content-Disposition").rfind("inline;", 0) == 0);
    CHECK(media_client(service).Get(ticket_path + "x")->status == 404);
    // A ticket reads only its file.
    CHECK(media_client(service).Get("/media/v1/files/" + file_id)->status == 401);

    // Bearer description; direct is never offered for files; ttl bounds.
    const auto bearer =
        body(api.Post("/api/v1/media-access", R"({"target":{"kind":"file","id":")" + file_id + R"("},"auth":"bearer"})",
                      "application/json"))["data"];
    CHECK(bearer["auth"] == "bearer");
    CHECK(bearer["url"] == file["media_url"]);
    CHECK(bearer["expires_at"].is_null());
    CHECK(error_code(api.Post("/api/v1/media-access",
                              R"({"target":{"kind":"file","id":")" + file_id + R"("},"mode":"direct"})",
                              "application/json")) == "DELIVERY_UNAVAILABLE");
    CHECK(error_code(api.Post("/api/v1/media-access",
                              R"({"target":{"kind":"file","id":")" + file_id + R"("},"ttl_seconds":5})",
                              "application/json")) == "VALIDATION_FAILED");

    // Tickets never reach the logs.
    for (const auto& record : sink->records()) {
        for (const auto& field : record.fields) {
            if (const auto* text = std::get_if<std::string>(&field.value)) {
                CHECK(text->find("mtk_") == std::string::npos);
            }
        }
    }
}

TEST_CASE("files of others, revoked tickets and missing files are refused (A23)", "[media][server]") {
    MediaDeployment m{"media-refusals"};
    const auto file_id = m.publish("a.mp3", pattern(100), "1");
    const auto gone_id = m.publish("b.mp3", pattern(100), "2");
    RunningService service{m.d, std::make_shared<infrastructure::observability::MemorySink>(),
                           MediaDeployment::bundles(), true};
    auto api = service.client(m.owner_token);

    // Another principal sees nothing of it, whatever the route.
    auto other_api = service.client(m.other_token);
    CHECK(other_api.Get("/api/v1/files/" + file_id)->status == 404);
    CHECK(
        other_api
            .Post("/api/v1/media-access", R"({"target":{"kind":"file","id":")" + file_id + R"("}})", "application/json")
            ->status == 404);
    CHECK(media_client(service, m.other_token).Get("/media/v1/files/" + file_id)->status == 404);

    const auto access = body(api.Post("/api/v1/media-access", R"({"target":{"kind":"file","id":")" + file_id + R"("}})",
                                      "application/json"))["data"];
    const auto ticket_path = path_of(access["url"].get<std::string>());
    CHECK(media_client(service).Get(ticket_path)->status == 200);

    // Moved away: 410 and kept as missing, reported again later.
    fs::remove(m.root / "b.mp3");
    const auto missing = api.Get("/api/v1/files/" + gone_id);
    CHECK(missing->status == 410);
    CHECK(error_code(missing) == "FILE_MISSING");
    CHECK(media_client(service, m.owner_token).Get("/media/v1/files/" + gone_id)->status == 410);
    std::ofstream{m.root / "b.mp3", std::ios::binary} << pattern(100); // a copy back does not revive it
    CHECK(api.Get("/api/v1/files/" + gone_id)->status == 410);
    // A rewritten file is not the published one either.
    std::ofstream{m.root / "a.mp3", std::ios::binary | std::ios::trunc} << "short";
    CHECK(media_client(service).Get(ticket_path)->status == 410);
}

TEST_CASE("assets need the online proxy while files stay readable (A33)", "[media][server]") {
    MediaDeployment m{"media-proxy-off"};
    const auto file_id = m.publish("c.mp3", pattern(50), "1");
    RunningService service{m.d, std::make_shared<infrastructure::observability::MemorySink>(),
                           MediaDeployment::bundles(), true};
    auto api = service.client(m.owner_token);
    const auto resolved =
        api.Post("/api/v1/assets/resolve", R"({"track":{"source":"both","id":"1"}})", "application/json");
    REQUIRE(resolved->status == 200);
    const auto asset_id = body(resolved)["data"]["asset_id"].get<std::string>();

    const auto asset_access = api.Post("/api/v1/media-access",
                                       R"({"target":{"kind":"asset","id":")" + asset_id + R"("}})", "application/json");
    CHECK(asset_access->status == 501);
    CHECK(error_code(asset_access) == "FEATURE_UNAVAILABLE");
    CHECK(api.Post("/api/v1/media-access", R"({"target":{"kind":"asset","id":"ast_unknown"}})", "application/json")
              ->status == 404);
    CHECK(media_client(service, m.owner_token).Get("/media/v1/assets/" + asset_id)->status == 501);
    CHECK(media_client(service, m.owner_token).Get("/media/v1/files/" + file_id)->status == 200);
}

TEST_CASE("a response holds its slot until the client is gone (A24)", "[media][server]") {
    MediaDeployment m{"media-slots", json{{"per_principal", 1}}};
    const auto big = m.publish("big.mp3", pattern(32 * 1024 * 1024), "1");
    RunningService service{m.d, std::make_shared<infrastructure::observability::MemorySink>(),
                           MediaDeployment::bundles(), true};
    auto api = service.client(m.owner_token);
    const auto responses = [&] {
        return body(api.Get("/api/v1/admin/status"))["data"]["media"]["responses"].get<int>();
    };

    std::atomic<bool> reading{false};
    std::atomic<bool> release{false};
    std::thread slow{[&] {
        auto client = media_client(service, m.owner_token);
        client.Get("/media/v1/files/" + big, [&](const char*, std::size_t) {
            reading = true;
            while (!release) {
                std::this_thread::sleep_for(10ms);
            }
            return false; // the player went away mid-file
        });
    }};
    const auto until = std::chrono::steady_clock::now() + 10s;
    while (!reading && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(10ms);
    }
    REQUIRE(reading);
    CHECK(responses() == 1);
    const auto refused =
        media_client(service, m.owner_token).Get("/media/v1/files/" + big, httplib::Headers{{"Range", "bytes=0-0"}});
    CHECK(refused->status == 429);
    CHECK(error_code(refused) == "CALLER_LIMIT_EXCEEDED");

    release = true;
    slow.join();
    bool freed = false;
    for (int i = 0; i < 500 && !freed; ++i) {
        freed = responses() == 0;
        std::this_thread::sleep_for(20ms);
    }
    CHECK(freed);
    const auto again =
        media_client(service, m.owner_token).Get("/media/v1/files/" + big, httplib::Headers{{"Range", "bytes=0-2"}});
    CHECK(again->status == 206);
    CHECK(again->body == "abc");
}
