#include "music_dl/infrastructure/clock.hpp"
#include "music_dl/infrastructure/observability/correlation.hpp"
#include "music_dl/infrastructure/observability/logger.hpp"
#include "music_dl/infrastructure/observability/metrics.hpp"
#include "music_dl/infrastructure/observability/redaction.hpp"

#include "music_dl/domain/catalog/refs.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <memory>
#include <sstream>
#include <string>

using namespace music_dl::infrastructure;
using namespace music_dl::infrastructure::observability;

TEST_CASE("sensitive keys are recognized", "[observability][redaction]") {
    for (const auto* key : {"Authorization", "cookie", "Set-Cookie", "access_token", "password",
                            "api_key", "media_ticket", "x-signature", "client_secret"}) {
        CHECK(is_sensitive_key(key));
    }
    for (const auto* key : {"credential_id", "token_id", "credential_version", "title", "source",
                            "request_id", "status"}) {
        CHECK_FALSE(is_sensitive_key(key));
    }
}

TEST_CASE("URLs lose user info, query and ticket paths", "[observability][redaction]") {
    CHECK(redact_url("https://cdn.example/a/b.mp3?sig=abc&e=1") ==
          "https://cdn.example/a/b.mp3?[REDACTED]");
    CHECK(redact_url("http://user:pw@host:8080/p#frag") == "http://host:8080/p#[REDACTED]");
    CHECK(redact_url("http://127.0.0.1:17633/media/v1/tickets/tkt_secret") ==
          "http://127.0.0.1:17633/media/v1/tickets/[REDACTED]");
    CHECK(redact_url("/media/v1/tickets/abc") == "/media/v1/tickets/[REDACTED]");
    CHECK(redact_url("http://search.kuwo.cn/r.s") == "http://search.kuwo.cn/r.s");
}

TEST_CASE("free text loses bearer tokens and URL secrets", "[observability][redaction]") {
    CHECK(redact_text("header Bearer abc.def ok") == "header Bearer [REDACTED] ok");
    CHECK(redact_text("fetch https://x.example/a?token=1 failed") ==
          "fetch https://x.example/a?[REDACTED] failed");
    CHECK(redact_field("cookie", "MUSIC_U=secret") == "[REDACTED]");
    CHECK(redact_field("final_url", "https://x/y?k=v") == "https://x/y?[REDACTED]");
    CHECK(redact_field("title", "示例歌曲") == "示例歌曲");
}

TEST_CASE("the logger redacts, filters and binds context", "[observability][logger]") {
    auto sink = std::make_shared<MemorySink>();
    auto clock = std::make_shared<ManualClock>();
    const Logger root{sink, LogLevel::info, clock};
    const auto request = root.with({{"request_id", "req_1"}, {"authorization", "Bearer x"}});

    request.log(LogLevel::debug, "ignored");
    request.info("upstream.request", {{"source", "kuwo"},
                                      {"url", "https://a.example/p?sig=1"},
                                      {"cookie", "SID=1"},
                                      {"elapsed_ms", 140},
                                      {"retryable", true}});

    const auto records = sink->records();
    REQUIRE(records.size() == 1);
    const auto& record = records[0];
    CHECK(record.event == "upstream.request");
    CHECK(record.level == LogLevel::info);
    CHECK(record.time == clock->now());
    REQUIRE(record.fields.size() == 7);
    CHECK(std::get<std::string>(record.fields[0].value) == "req_1");
    CHECK(std::get<std::string>(record.fields[1].value) == "[REDACTED]");
    CHECK(std::get<std::string>(record.fields[3].value) == "https://a.example/p?[REDACTED]");
    CHECK(std::get<std::string>(record.fields[4].value) == "[REDACTED]");
    CHECK(std::get<std::int64_t>(record.fields[5].value) == 140);
    CHECK(std::get<bool>(record.fields[6].value));
}

TEST_CASE("JSON lines carry UTC time, level and fields", "[observability][logger]") {
    std::ostringstream out;
    auto clock = std::make_shared<ManualClock>(
        std::chrono::system_clock::time_point{std::chrono::milliseconds{1760860800123}});
    const Logger logger{std::make_shared<JsonLineSink>(out), LogLevel::trace, clock};
    logger.warn("download.retry", {{"task_id", "tsk_1"}, {"attempt", 2}, {"note", "\xC3\x28"}});

    const auto line = nlohmann::json::parse(out.str());
    CHECK(line["time"] == "2025-10-19T08:00:00.123Z");
    CHECK(line["level"] == "warn");
    CHECK(line["event"] == "download.retry");
    CHECK(line["task_id"] == "tsk_1");
    CHECK(line["attempt"] == 2);
    CHECK(line.contains("note")); // invalid UTF-8 is replaced, not fatal
}

TEST_CASE("text sink is single-line and readable", "[observability][logger]") {
    std::ostringstream out;
    const Logger logger{std::make_shared<TextSink>(out), LogLevel::info,
                        std::make_shared<ManualClock>()};
    logger.error("db.unavailable", {{"elapsed_ms", 500}});
    CHECK(out.str().find("ERROR db.unavailable elapsed_ms=500\n") != std::string::npos);
}

TEST_CASE("log levels parse", "[observability][logger]") {
    CHECK(*parse_log_level("warn") == LogLevel::warn);
    CHECK_FALSE(parse_log_level("WARN"));
}

TEST_CASE("request ids are accepted or generated", "[observability][correlation]") {
    IdGenerator generator{42};
    CHECK(accept_or_generate_request_id(std::string_view{"client-1.a_b"}, generator) ==
          "client-1.a_b");

    const auto generated = accept_or_generate_request_id(std::string_view{"bad id!"}, generator);
    CHECK(music_dl::domain::is_valid_service_id("req_", generated));
    CHECK_FALSE(is_valid_client_request_id(std::string(65, 'a')));
    CHECK(music_dl::domain::is_valid_service_id(
        "req_", accept_or_generate_request_id(std::nullopt, generator)));

    IdGenerator a{7};
    IdGenerator b{7};
    CHECK(a.next("tsk_") == b.next("tsk_"));
    CHECK(a.next("tsk_") != a.next("tsk_"));
}

TEST_CASE("manual clock moves only when told", "[observability][clock]") {
    ManualClock clock;
    const auto steady = clock.steady_now();
    const auto wall = clock.now();

    clock.advance(std::chrono::seconds{5});
    CHECK(clock.steady_now() - steady == std::chrono::seconds{5});
    CHECK(clock.now() - wall == std::chrono::seconds{5});

    clock.set_wall(wall - std::chrono::hours{1}); // wall clock jumps back
    CHECK(clock.steady_now() - steady == std::chrono::seconds{5});
    CHECK(clock.now() < wall);
}

TEST_CASE("metrics aggregate counters and latencies", "[observability][metrics]") {
    InMemoryMetrics metrics;
    metrics.increment("upstream.requests");
    metrics.increment("upstream.requests", 2);
    metrics.observe("admission.wait", std::chrono::milliseconds{3});
    metrics.observe("admission.wait", std::chrono::milliseconds{7});

    CHECK(metrics.counter("upstream.requests") == 3);
    CHECK(metrics.counter("missing") == 0);
    const auto wait = metrics.latency("admission.wait");
    CHECK(wait.count == 2);
    CHECK(wait.total == std::chrono::milliseconds{10});
    CHECK(wait.max == std::chrono::milliseconds{7});
}
