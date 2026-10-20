// Verifies that the pinned third-party libraries are the expected versions and
// work on this toolchain. No business behavior is tested here.

#include <catch2/catch_test_macros.hpp>

#include <httplib.h>
#include <mbedtls/version.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <string>
#include <string_view>
#include <thread>

TEST_CASE("pinned dependency versions", "[dependencies]") {
    CHECK(std::string_view{CPPHTTPLIB_VERSION} == MUSIC_SERVICE_EXPECTED_HTTPLIB_VERSION);
    CHECK(std::string_view{MBEDTLS_VERSION_STRING} == MUSIC_SERVICE_EXPECTED_MBEDTLS_VERSION);
    CHECK(std::string_view{sqlite3_libversion()} == MUSIC_SERVICE_EXPECTED_SQLITE_VERSION);

    const auto json_version = std::to_string(NLOHMANN_JSON_VERSION_MAJOR) + "." +
                              std::to_string(NLOHMANN_JSON_VERSION_MINOR) + "." +
                              std::to_string(NLOHMANN_JSON_VERSION_PATCH);
    CHECK(json_version == MUSIC_SERVICE_EXPECTED_JSON_VERSION);
}

TEST_CASE("json round-trips UTF-8 text", "[dependencies][json]") {
    const auto input = nlohmann::json{{"title", "示例歌曲"}, {"size_bytes", 8400000}};
    const auto parsed = nlohmann::json::parse(input.dump());
    CHECK(parsed.at("title").get<std::string>() == "示例歌曲");
    CHECK(parsed.at("size_bytes").get<long long>() == 8400000);
}

TEST_CASE("sqlite executes statements in memory", "[dependencies][sqlite]") {
    sqlite3* db = nullptr;
    REQUIRE(sqlite3_open(":memory:", &db) == SQLITE_OK);
    REQUIRE(sqlite3_exec(db,
                         "CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT NOT NULL);"
                         "INSERT INTO t(name) VALUES ('酷我');",
                         nullptr, nullptr, nullptr) == SQLITE_OK);

    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(db, "SELECT name FROM t WHERE id = 1", -1, &stmt, nullptr) ==
            SQLITE_OK);
    REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    CHECK(std::string{text} == "酷我");
    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

TEST_CASE("httplib serves and fetches over loopback", "[dependencies][httplib]") {
    httplib::Server server;
    server.Get("/ping", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("pong", "text/plain");
    });
    const int port = server.bind_to_any_port("127.0.0.1");
    REQUIRE(port > 0);
    std::thread worker{[&server] { server.listen_after_bind(); }};
    server.wait_until_ready();

    httplib::Client client{"127.0.0.1", port};
    const auto res = client.Get("/ping");
    server.stop();
    worker.join();

    REQUIRE(res);
    CHECK(res->status == 200);
    CHECK(res->body == "pong");
}

TEST_CASE("httplib TLS client is built with Mbed TLS", "[dependencies][tls]") {
    httplib::SSLClient client{"localhost", 443};
    CHECK(client.is_valid());
}
