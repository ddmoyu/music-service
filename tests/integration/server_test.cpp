// The music-service process in-process: command line, offline identity management,
// ProcessHost startup and shutdown order, destination bindings and the control HTTP
// API end to end over loopback.

#include "service_harness.hpp"

#include "infrastructure/clock.hpp"
#include "infrastructure/filesystem/files.hpp"
#include "infrastructure/filesystem/paths.hpp"
#include "infrastructure/observability/correlation.hpp"
#include "infrastructure/observability/logger.hpp"
#include "infrastructure/storage/sqlite/migrations.hpp"
#include "infrastructure/storage/sqlite/task_repositories.hpp"
#include "server/commands.hpp"
#include "server/process_host.hpp"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4702)
#endif
#include <httplib.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <future>
#include <sstream>
#include <thread>

using namespace music_service;
using namespace music_service::test;
using namespace std::chrono_literals;
using json = nlohmann::json;
namespace fs = std::filesystem;
using infrastructure::filesystem::path_from_utf8;
using infrastructure::filesystem::path_to_utf8;

namespace {

std::vector<std::string> events(const infrastructure::observability::MemorySink& sink) {
    std::vector<std::string> names;
    for (const auto& record : sink.records()) {
        names.push_back(record.event);
    }
    return names;
}

std::ptrdiff_t index_of(const std::vector<std::string>& names, const std::string& name) {
    const auto it = std::find(names.begin(), names.end(), name);
    return it == names.end() ? -1 : it - names.begin();
}

} // namespace

TEST_CASE("offline commands initialize the instance and manage identities", "[server]") {
    Deployment d{"offline"};

    const auto version = d.run({"version"});
    CHECK(version.code == server::exit_ok);
    CHECK(version.out.find("music-service ") == 0);

    CHECK(d.run({"check-config"}).code == server::exit_ok);

    const auto init = d.run({"init"});
    REQUIRE(init.code == server::exit_ok);
    const auto bootstrap = d.read(d.bootstrap());
    CHECK(bootstrap.rfind("mst_", 0) == 0);
    CHECK(init.out.find("tok_") != std::string::npos);
    CHECK(init.out.find(bootstrap) == std::string::npos); // plaintext only in the file
    CHECK(init.err.find(bootstrap) == std::string::npos);

    const auto again = d.run({"init"});
    CHECK(again.code == server::exit_failed);
    CHECK(again.err.find("CONFLICT") != std::string::npos);

    const auto created = d.run({"principal", "create", "--name", "客户端"});
    REQUIRE(created.code == server::exit_ok);
    const auto principal = created.out.substr(0, created.out.find('\t'));
    CHECK(principal.rfind("prn_", 0) == 0);
    CHECK(d.run({"principal", "list"}).out.find(principal) != std::string::npos);

    const auto output = d.dir / "out" / "client-token.txt";
    const auto issued = d.run({"token", "issue", "--principal", principal, "--scopes", "catalog:read,downloads:read",
                               "--output", path_to_utf8(output)});
    REQUIRE(issued.code == server::exit_ok);
    const auto token = d.read(output);
    CHECK(issued.out.find(token) == std::string::npos);
    const auto token_id = issued.out.substr(6, issued.out.find(' ', 6) - 6);
    CHECK(token_id.rfind("tok_", 0) == 0);

    // Never overwrites an existing file.
    const auto clash = d.run({"token", "issue", "--principal", principal, "--scopes", "catalog:read", "--output",
                              path_to_utf8(output)});
    CHECK(clash.code == server::exit_failed);
    CHECK(d.read(output) == token);

    const auto listed = d.run({"token", "list", "--principal", principal});
    CHECK(listed.out.find(token_id) != std::string::npos);
    CHECK(listed.out.find(token) == std::string::npos);
    CHECK(d.run({"token", "revoke", "--id", token_id}).code == server::exit_ok);
    CHECK(d.run({"token", "revoke", "--id", token_id}).code == server::exit_ok);
    CHECK(d.run({"token", "revoke", "--id", "tok_unknown"}).code == server::exit_failed);
    CHECK(d.run({"principal", "disable", "--id", principal}).out.find("disabled") != std::string::npos);
    CHECK(d.run({"principal", "enable", "--id", "prn_missing"}).code == server::exit_failed);

    CHECK(d.run({"principal", "create"}).code == server::exit_usage);
    CHECK(d.run({"token", "issue", "--principal", principal}).code == server::exit_usage);
    CHECK(d.run({"frobnicate"}).code == server::exit_usage);
    CHECK(d.run({"principal", "list", "--bogus", "1"}).code == server::exit_usage);
}

TEST_CASE("the service serves the control API and stops in order", "[server]") {
    Deployment d{"serve"};
    REQUIRE(d.run({"init"}).code == server::exit_ok);
    const auto admin_token = d.read(d.bootstrap());
    const auto created = d.run({"principal", "create", "--name", "reader"});
    const auto reader = created.out.substr(0, created.out.find('\t'));
    REQUIRE(d.run({"token", "issue", "--principal", reader, "--scopes", "catalog:read,downloads:read", "--output",
                   path_to_utf8(d.dir / "out" / "reader.txt")})
                .code == server::exit_ok);
    const auto reader_token = d.read(d.dir / "out" / "reader.txt");

    auto sink = std::make_shared<infrastructure::observability::MemorySink>();
    {
        RunningService service{d, sink};
        CHECK(service.report.instance_id.rfind("inst_", 0) == 0);

        // Management is offline only while the service holds the instance.
        const auto locked = d.run({"token", "list", "--principal", "prn_local"});
        CHECK(locked.code == server::exit_failed);
        CHECK(locked.err.find("INSTANCE_LOCKED") != std::string::npos);
        CHECK(d.run({"healthcheck"}).code == server::exit_failed); // config says port 0; nothing listens there

        auto anonymous = service.client();
        CHECK(anonymous.Get("/health/live")->status == 200);
        CHECK(anonymous.Get("/health/ready")->status == 200);

        const auto unauthenticated = anonymous.Get("/api/v1/info");
        REQUIRE(unauthenticated);
        CHECK(unauthenticated->status == 401);
        CHECK(unauthenticated->get_header_value("WWW-Authenticate") == "Bearer");
        CHECK(error_code(unauthenticated) == "UNAUTHENTICATED");
        CHECK_FALSE(unauthenticated->get_header_value("X-Request-Id").empty());

        auto admin = service.client(admin_token);
        const auto info = admin.Get("/api/v1/info", httplib::Headers{{"X-Request-Id", "client.req-1"}});
        REQUIRE(info);
        CHECK(info->status == 200);
        CHECK(info->get_header_value("X-Request-Id") == "client.req-1");
        CHECK(info->get_header_value("Cache-Control") == "no-store");
        const auto info_body = body(info);
        CHECK(info_body["meta"]["request_id"] == "client.req-1");
        CHECK(info_body["data"]["service"] == "music-service");
        CHECK(info_body["data"]["instance_id"] == service.report.instance_id);
        CHECK(info_body["data"]["features"]["downloads"] == false); // nothing undelivered is declared
        CHECK(admin.Get("/api/v1/info", httplib::Headers{{"X-Request-Id", "bad id!"}})->get_header_value("X-Request-Id").rfind("req_", 0) == 0);

        // Scopes.
        auto reader_client = service.client(reader_token);
        CHECK(reader_client.Get("/api/v1/info")->status == 200);
        const auto forbidden = reader_client.Get("/api/v1/credentials");
        CHECK(forbidden->status == 403);
        CHECK(error_code(forbidden) == "FORBIDDEN");
        CHECK(reader_client.Get("/api/v1/admin/status")->status == 403);
        CHECK(service.client("mst_not-a-token").Get("/api/v1/info")->status == 401);

        // Providers: paged, with the caller's credential summary and no probing.
        const auto providers = body(admin.Get("/api/v1/providers?limit=1"));
        REQUIRE(providers["data"]["items"].size() == 1);
        CHECK(providers["data"]["items"][0]["source"] == "aud");
        CHECK(providers["data"]["items"][0]["capabilities"]["resolve_audio"]["availability"]["state"] == "unknown");
        CHECK(providers["data"]["items"][0]["auth_methods"][0]["supports_verify"] == true);
        const auto cursor = providers["data"]["page"]["next_cursor"].get<std::string>();
        const auto second = body(admin.Get("/api/v1/providers?limit=1&cursor=" + cursor));
        CHECK(second["data"]["items"][0]["source"] == "cat");
        CHECK(error_code(reader_client.Get("/api/v1/providers?limit=1&cursor=" + cursor)) == "CURSOR_MISMATCH");
        CHECK(admin.Get("/api/v1/providers?colour=blue")->status == 400);
        CHECK(admin.Get("/api/v1/providers?limit=0")->status == 400);
        CHECK(body(admin.Get("/api/v1/limits"))["data"]["catalog"]["max_sources_per_request"] == 8);

        // Destinations follow the configured ACL and never show the directory.
        const auto mine = body(admin.Get("/api/v1/destinations"))["data"]["items"];
        REQUIRE(mine.size() == 1);
        CHECK(mine[0]["destination_id"] == "dst_main");
        CHECK(mine[0]["available"] == true);
        CHECK_FALSE(mine[0].contains("root"));
        CHECK(body(reader_client.Get("/api/v1/destinations"))["data"]["items"].empty());

        // Credentials and defaults (A47 through the API).
        const auto made = admin.Post("/api/v1/credentials", R"({"source":"aud","label":"主账号","cookie":"good"})",
                                     "application/json");
        REQUIRE(made);
        CHECK(made->status == 201);
        CHECK(made->body.find("good") == std::string::npos);
        const auto credential = body(made)["data"];
        const auto id = credential["credential_id"].get<std::string>();
        CHECK(credential["auth_profile"] == "web_cookie");
        CHECK(credential["version"] == 1);

        const auto full = admin.Post(
            "/api/v1/credentials",
            R"({"source":"aud","kind":"cookie","auth_profile":"web_cookie","secret":{"cookie":"other=1"}})",
            "application/json");
        CHECK(full->status == 201);
        CHECK(error_code(admin.Post("/api/v1/credentials", R"({"source":"aud","kind":"oauth2","auth_profile":"x","secret":{}})",
                                    "application/json")) == "CAPABILITY_UNSUPPORTED");
        CHECK(error_code(admin.Post("/api/v1/credentials", R"({"source":"ghost","cookie":"a"})", "application/json")) ==
              "UNKNOWN_PROVIDER");
        CHECK(error_code(admin.Post("/api/v1/credentials", R"({"source":"aud","cookie":"a","extra":1})",
                                    "application/json")) == "VALIDATION_FAILED");
        const auto wrong_type = admin.Post("/api/v1/credentials", "source=aud", "text/plain");
        CHECK(wrong_type->status == 415);
        CHECK(error_code(wrong_type) == "UNSUPPORTED_CONTENT_TYPE");
        const auto huge = admin.Post("/api/v1/credentials", std::string(300 * 1024, 'x'), "application/json");
        REQUIRE(huge);
        CHECK(huge->status == 413);

        CHECK(body(admin.Get("/api/v1/credentials?source=aud"))["data"]["items"].size() == 2);
        const auto verified = body(admin.Post("/api/v1/credentials/" + id + "/verify", "", "application/json"));
        CHECK(verified["data"]["result"] == "valid");
        CHECK(verified["data"]["credential"]["last_verify_result"] == "valid");
        const auto renamed = body(admin.Put("/api/v1/credentials/" + id, R"({"label":"改名"})", "application/json"));
        CHECK(renamed["data"]["version"] == 2);

        const auto set = body(admin.Put("/api/v1/credential-defaults/aud", json{{"credential_id", id}}.dump(),
                                        "application/json"));
        CHECK(set["data"]["credential_id"] == id);
        CHECK(set["data"]["selection_version"] == 1);
        CHECK(body(admin.Get("/api/v1/credential-defaults/aud"))["data"]["selection_version"] == 1);
        CHECK(body(admin.Get("/api/v1/providers?capability=resolve_audio"))["data"]["items"][0]["credentials"]
                  ["default_credential_id"] == id);
        CHECK(error_code(admin.Put("/api/v1/credential-defaults/cat", json{{"credential_id", id}}.dump(),
                                   "application/json")) == "VALIDATION_FAILED");
        CHECK(admin.Put("/api/v1/credential-defaults/aud", R"({"credential_id":"cre_nobody"})", "application/json")
                  ->status == 404);
        const auto revoked = body(admin.Delete("/api/v1/credentials/" + id));
        CHECK(revoked["data"]["status"] == "revoked");
        const auto cleared = body(admin.Get("/api/v1/credential-defaults/aud"))["data"];
        CHECK(cleared["credential_id"].is_null());
        CHECK(cleared["selection_version"] == 2);
        CHECK(body(admin.Delete("/api/v1/credential-defaults/aud"))["data"]["selection_version"] == 2);

        // Search, details, links and resolve over HTTP.
        CHECK(info_body["data"]["features"]["search"] == true);
        const auto found = body(reader_client.Get("/api/v1/search/tracks?q=hello&sources=cat&limit=5"));
        CHECK(found["data"]["items"][0]["title"] == "hello");
        CHECK(found["data"]["items"][0]["ref"]["source"] == "cat");
        CHECK(found["data"]["items"][0]["context_ref"].is_null());
        CHECK(found["meta"]["partial"] == false);
        CHECK(found["meta"]["source_results"][0]["status"] == "ok");
        CHECK(found["meta"]["source_results"][0]["cache"]["hit"] == false);
        CHECK(found["meta"]["source_results"][0]["page"]["limit"] == 5);
        const auto unsupported = reader_client.Get("/api/v1/search/tracks?q=hello&sources=aud");
        CHECK(unsupported->status == 422);
        CHECK(body(unsupported)["meta"]["source_results"][0]["status"] == "unsupported");
        CHECK(error_code(reader_client.Get("/api/v1/search/tracks?q=hello&sources=ghost")) == "UNKNOWN_PROVIDER");
        CHECK(reader_client.Get("/api/v1/search/tracks?sources=cat")->status == 400);
        CHECK(reader_client.Get("/api/v1/search/tracks?q=x&sources=cat&page=2")->status == 400);
        CHECK(anonymous.Get("/api/v1/search/tracks?q=x")->status == 401);
        CHECK(body(reader_client.Get("/api/v1/tracks/detail?source=cat&id=7"))["data"]["title"] == "detail");
        CHECK(reader_client.Get("/api/v1/tracks/detail?source=cat")->status == 400);
        CHECK(body(reader_client.Post("/api/v1/links/parse", R"({"text":"nothing to see"})", "application/json"))
                  ["data"]["matched"] == false);
        CHECK(error_code(reader_client.Post("/api/v1/assets/resolve", R"({"track":{"source":"cat","id":"1"}})",
                                            "application/json")) == "CAPABILITY_UNSUPPORTED");
        CHECK(error_code(reader_client.Post("/api/v1/assets/resolve",
                                            R"({"track":{"source":"aud","id":"1"},"quality":"best"})",
                                            "application/json")) == "VALIDATION_FAILED");
        CHECK(error_code(reader_client.Post("/api/v1/assets/resolve", R"({"track":{"source":"aud","id":"1"}})",
                                            "application/json")) == "CREDENTIALS_REQUIRED");
        CHECK(reader_client.Post("/api/v1/assets/probe", R"({"asset_id":"ast_missing"})", "application/json")->status ==
              404);

        // Undelivered routes are absent, not empty successes; errors keep the envelope.
        const auto missing = admin.Get("/api/v1/files");
        CHECK(missing->status == 404);
        CHECK(error_code(missing) == "NOT_FOUND");

        // CORS preflight.
        const auto preflight =
            anonymous.Options("/api/v1/info", httplib::Headers{{"Origin", "http://127.0.0.1:5173"},
                                                                  {"Access-Control-Request-Method", "GET"}});
        REQUIRE(preflight);
        CHECK(preflight->status == 204);
        CHECK(preflight->get_header_value("Access-Control-Allow-Origin") == "http://127.0.0.1:5173");
        CHECK(preflight->get_header_value("Access-Control-Allow-Headers").find("Authorization") != std::string::npos);
        CHECK(anonymous.Options("/api/v1/info", httplib::Headers{{"Origin", "http://evil.example"}})->status == 403);

        const auto status = body(admin.Get("/api/v1/admin/status"))["data"];
        CHECK(status["state"] == "ready");
        CHECK(status["providers"]["registered"] == 2);
        CHECK(status["storage"]["available"] == true);

        CHECK(service.stop().code == server::exit_ok);
    }

    // Shutdown order: listeners, services, database, then the lock.
    const auto names = events(*sink);
    const auto listeners = index_of(names, "shutdown.listeners_stopped");
    const auto database = index_of(names, "shutdown.database_closed");
    const auto lock = index_of(names, "shutdown.lock_released");
    CHECK(listeners >= 0);
    CHECK(listeners < database);
    CHECK(database < lock);
    for (const auto& record : sink->records()) {
        for (const auto& field : record.fields) {
            if (const auto* text = std::get_if<std::string>(&field.value)) {
                CHECK(text->find(admin_token) == std::string::npos);
                CHECK(text->find("good") == std::string::npos);
            }
        }
    }
    // The lock is free again.
    CHECK(d.run({"token", "list", "--principal", "prn_local"}).code == server::exit_ok);
}

TEST_CASE("destination bindings cannot move or disappear while in use", "[server]") {
    Deployment d{"destinations"};
    REQUIRE(d.run({"init"}).code == server::exit_ok);
    auto& destinations = d.config["destinations"];
    const auto original_root = destinations[0]["root"];

    // Same id, another root: refused before anything is served (A49).
    destinations[0]["root"] = path_to_utf8(d.dir / "second");
    d.write_config();
    const auto moved = d.run({"serve"});
    CHECK(moved.code == server::exit_failed);
    CHECK(moved.err.find("DESTINATION_ROOT_CHANGED") != std::string::npos);
    CHECK(d.run({"check-config"}).code == server::exit_failed);

    // New id for the new place, the old one closed for new tasks: both stay usable.
    destinations[0]["root"] = original_root;
    destinations[0]["accept_new_tasks"] = false;
    destinations.push_back({{"id", "dst_new"}, {"name", "新目录"}, {"root", path_to_utf8(d.dir / "second")},
                            {"principals", {"prn_local"}}});
    d.write_config();
    {
        auto sink = std::make_shared<infrastructure::observability::MemorySink>();
        RunningService service{d, sink};
        const auto items = body(service.client(d.read(d.bootstrap())).Get("/api/v1/destinations"))["data"]["items"];
        REQUIRE(items.size() == 2);
        CHECK(items[0]["destination_id"] == "dst_main");
        CHECK(items[0]["accept_new_tasks"] == false);
        CHECK(items[1]["destination_id"] == "dst_new");
    }

    // A task now refers to dst_main.
    {
        namespace sqlite = infrastructure::storage::sqlite;
        auto database = sqlite::Database::open(d.dir / "data" / "service.sqlite3", {}).value();
        auto clock = std::make_shared<infrastructure::SystemClock>();
        const sqlite::RepositoryContext ctx{database.get(), clock,
                                           std::make_shared<infrastructure::observability::IdGenerator>(9)};
        sqlite::SqliteTaskRepository tasks{ctx, {}};
        domain::DownloadIntentDraft draft{"prn_local",
                                          test::track("cat", "1"),
                                          domain::SameSource{},
                                          {test::anonymous("prn_local", "cat")},
                                          {domain::Quality::standard, domain::QualityPolicy::strict, false},
                                          domain::MetadataPolicy::best_effort,
                                          {"dst_main", "d1", "n1", "m1"}};
        auto intent = domain::NormalizedDownloadIntent::normalize(std::move(draft)).value();
        REQUIRE(tasks.create({std::move(intent), "key-dst-main-0001", "h1", false}));
    }

    // Removing a destination in use is refused; removing an unused one is fine.
    destinations.erase(destinations.begin());
    d.write_config();
    const auto removed = d.run({"serve"});
    CHECK(removed.code == server::exit_failed);
    CHECK(removed.err.find("DESTINATION_IN_USE") != std::string::npos);

    destinations = json::array({{{"id", "dst_main"}, {"name", "主目录"}, {"root", original_root},
                                 {"principals", {"prn_local"}}}});
    d.write_config();
    {
        RunningService service{d, std::make_shared<infrastructure::observability::MemorySink>()};
        CHECK(service.stop().code == server::exit_ok);
    }

    // A new id whose root does not exist cannot be registered; nothing is created.
    destinations.push_back({{"id", "dst_gone"}, {"name", "x"}, {"root", path_to_utf8(d.dir / "missing")},
                            {"principals", {"prn_local"}}});
    d.write_config();
    const auto gone = d.run({"serve"});
    CHECK(gone.code == server::exit_failed);
    CHECK(gone.err.find("DESTINATION_UNAVAILABLE") != std::string::npos);
    CHECK_FALSE(fs::exists(d.dir / "missing"));
}

TEST_CASE("downloads are accepted, queried, canceled and retried over HTTP", "[server]") {
    Deployment d{"downloads"};
    REQUIRE(d.run({"init"}).code == server::exit_ok);
    const auto token = d.read(d.bootstrap());
    RunningService service{d, std::make_shared<infrastructure::observability::MemorySink>(),
                           std::vector<ports::ProviderBundle>{mock_combo("both")}};
    auto client = service.client(token);
    const auto key = [](std::string value) { return httplib::Headers{{"Idempotency-Key", std::move(value)}}; };
    const std::string request_body = R"({"track":{"source":"both","id":"1"},"quality":"standard","metadata_policy":"skip"})";
    const std::string reordered = R"({"metadata_policy":"skip","quality":"standard","track":{"id":"1","source":"both"}})";

    CHECK(error_code(client.Post("/api/v1/downloads", request_body, "application/json")) == "VALIDATION_FAILED");
    const auto created = client.Post("/api/v1/downloads", key("first-download-key"), request_body, "application/json");
    REQUIRE(created);
    CHECK(created->status == 202);
    const auto task_id = body(created)["data"]["task_id"].get<std::string>();
    CHECK(created->get_header_value("Location") == "/api/v1/downloads/" + task_id);
    CHECK(body(created)["data"]["status"] == "queued");

    // Same key, same request (key order does not matter): the first response again.
    const auto replayed = client.Post("/api/v1/downloads", key("first-download-key"), reordered, "application/json");
    CHECK(replayed->status == 202);
    CHECK(replayed->get_header_value("Idempotency-Replayed") == "true");
    CHECK(body(replayed)["data"]["task_id"] == task_id);
    CHECK(error_code(client.Post("/api/v1/downloads", key("first-download-key"),
                                 R"({"track":{"source":"both","id":"2"}})", "application/json")) ==
          "IDEMPOTENCY_CONFLICT");
    const auto shared = client.Post("/api/v1/downloads", key("second-download-key"), request_body, "application/json");
    CHECK(shared->status == 200);
    CHECK(body(shared)["data"]["reused"] == true);
    CHECK(error_code(client.Post("/api/v1/downloads", key("third-download-key"),
                                 R"({"track":{"source":"both","id":"1"},"surprise":1})", "application/json")) ==
          "VALIDATION_FAILED");
    CHECK(error_code(client.Post("/api/v1/downloads", key("fourth-download-key"),
                                 R"({"track":{"source":"both","id":"9"},"destination_id":"dst_nowhere"})",
                                 "application/json")) == "NOT_FOUND");

    // Task snapshot with a weak ETag.
    const auto snapshot = client.Get("/api/v1/downloads/" + task_id);
    REQUIRE(snapshot);
    const auto task = body(snapshot)["data"];
    CHECK(task["original_track"]["source"] == "both");
    CHECK(task["metadata_policy"] == "skip");
    CHECK(task["attempt"] == 0);
    CHECK(task["progress"].is_null());
    const auto etag = snapshot->get_header_value("ETag");
    CHECK(etag == "W/\"task-" + task_id + "-v1\"");
    CHECK(client.Get("/api/v1/downloads/" + task_id, httplib::Headers{{"If-None-Match", etag}})->status == 304);
    CHECK(client.Get("/api/v1/downloads/tsk_missing")->status == 404);
    CHECK(body(client.Get("/api/v1/downloads?limit=10"))["data"]["items"].size() == 1);

    // Cancel, then retry under a new key.
    const auto canceled = client.Post("/api/v1/downloads/" + task_id + "/cancel", "", "application/json");
    CHECK(canceled->status == 200);
    CHECK(body(canceled)["data"]["status"] == "canceled");
    const auto retried = client.Post("/api/v1/downloads/" + task_id + "/retry", key("retry-download-key"), "",
                                     "application/json");
    REQUIRE(retried);
    CHECK(retried->status == 202);
    CHECK(body(retried)["data"]["retry_of"] == task_id);

    // A batch with a repeated item.
    const auto batch = client.Post("/api/v1/download-batches", key("first-batch-key-0"),
                                   R"({"items":[{"track":{"source":"both","id":"5"}},{"track":{"source":"both","id":"5"}}]})",
                                   "application/json");
    REQUIRE(batch);
    CHECK(batch->status == 202);
    const auto batch_body = body(batch)["data"];
    CHECK(batch_body["created_count"] == 1);
    CHECK(batch_body["reused_count"] == 1);
    const auto batch_view = body(client.Get("/api/v1/download-batches/" + batch_body["batch_id"].get<std::string>()));
    CHECK(batch_view["data"]["items"].size() == 2);
    CHECK(batch_view["data"]["stats"]["queued"] == 2);
    const auto bad_batch = client.Post("/api/v1/download-batches", key("second-batch-key-0"),
                                       R"({"items":[{"track":{"source":"both","id":"6"}},{"track":{"source":"ghost","id":"7"}}]})",
                                       "application/json");
    CHECK(bad_batch->status == 400);
    CHECK(body(bad_batch)["error"]["details"][0]["field"] == "items[1]");
}

