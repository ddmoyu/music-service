#include "infrastructure/clock.hpp"
#include "infrastructure/observability/correlation.hpp"
#include "infrastructure/observability/logger.hpp"
#include "infrastructure/observability/metrics.hpp"
#include "infrastructure/observability/redaction.hpp"

#include "domain/catalog/refs.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <fstream>
#include <filesystem>
#include "infrastructure/filesystem/paths.hpp"

using namespace music_service::infrastructure;
using namespace music_service::infrastructure::observability;

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
    CHECK(redact_text("Bearer  DEMO_TOKEN") == "Bearer  [REDACTED]");
    CHECK(redact_text("Basic \t DEMO_TOKEN ok") == "Basic \t [REDACTED] ok");
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

    out.str("");
    logger.info("track.added", {{"title", "a\r\n2026-01-01T00:00:00Z ERROR forged \\x"}});
    const auto text = out.str();
    CHECK(std::count(text.begin(), text.end(), '\n') == 1);
    CHECK(text.find("title=a\\r\\n2026-01-01T00:00:00Z ERROR forged \\\\x\n") != std::string::npos);
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
    CHECK(music_service::domain::is_valid_service_id("req_", generated));
    CHECK_FALSE(is_valid_client_request_id(std::string(65, 'a')));
    CHECK(music_service::domain::is_valid_service_id(
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

namespace {
namespace ports = music_service::application::ports;
namespace fs = std::filesystem;
struct LogDirectory {
    fs::path path = fs::temp_directory_path() / filesystem::path_from_utf8("music-log-轮转-" + IdGenerator{}.next("tmp_"));
    LogDirectory() { fs::create_directories(path); }
    ~LogDirectory() { std::error_code ec; fs::remove_all(path, ec); }
};
}

TEST_CASE("metrics retain fixed dimensions and exact concurrent totals", "[observability][metrics]") {
    InMemoryMetrics metrics;
    std::vector<std::jthread> workers;
    for (int i = 0; i < 4; ++i) workers.emplace_back([&] {
        for (int n = 0; n < 1000; ++n)
            metrics.record(ports::MetricOperation::upstream, ports::MetricOutcome::success, std::chrono::milliseconds{5});
    });
    workers.clear();
    metrics.record(ports::MetricOperation::upstream, ports::MetricOutcome::failure, std::chrono::milliseconds{8});
    metrics.record(ports::MetricOperation::upstream, ports::MetricOutcome::canceled, std::chrono::nanoseconds{-1});
    metrics.record(ports::MetricOperation::upstream, ports::MetricOutcome::deferred, std::chrono::seconds{400});
    const auto snapshot = metrics.snapshot();
    const auto& item = snapshot.operations[static_cast<std::size_t>(ports::MetricOperation::upstream)];
    CHECK(item.count == 4003);
    CHECK(item.outcomes == std::array<std::uint64_t, 4>{4000, 1, 1, 1});
    CHECK(item.duration_ns == 420008000000ULL);
    CHECK(item.max_duration_ns == 400000000000ULL);
    CHECK(item.durations[0] == 1);
    CHECK(item.durations[1] == 4000);
    CHECK(item.durations[2] == 1);
    CHECK(item.durations.back() == 1);
    CHECK(snapshot.operations[0].count == 0);
}

TEST_CASE("metric timers count exceptions without changing their outcome", "[observability][metrics]") {
    auto metrics = std::make_shared<InMemoryMetrics>();
    CHECK_THROWS_AS(([&] {
        ports::MetricTimer timer{metrics, ports::MetricOperation::processing};
        throw std::runtime_error("failed");
    }()), std::runtime_error);
    CHECK(metrics->snapshot().operations[static_cast<std::size_t>(ports::MetricOperation::processing)].outcomes[1] == 1);
}

TEST_CASE("file logs rotate within limits and retain redacted complete records", "[observability][logger][rotation]") {
    LogDirectory dir;
    const auto file = dir.path / "music-service.log";
    const LogRotation limits{512, 2};
    for (int session = 0; session < 2; ++session) {
        auto target = std::make_shared<RotatingFileSink>(file, false, limits);
        auto sink = std::make_shared<AsyncLogSink>(target);
        Logger logger{sink, LogLevel::info, std::make_shared<ManualClock>()};
        for (int i = 0; i < 20; ++i)
            logger.info("rotation.check", {{"sequence", session * 20 + i}, {"cookie", "private-cookie"}});
        sink->close();
        CHECK(sink->stats().written == 20);
        CHECK(sink->stats().failures == 0);
    }
    int files = 0;
    int last_sequence = -1;
    for (const auto& entry : fs::directory_iterator(dir.path)) {
        ++files;
        CHECK(entry.file_size() <= limits.max_file_bytes);
        std::ifstream in(entry.path(), std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            const auto record = nlohmann::json::parse(line);
            CHECK(record["cookie"] == "[REDACTED]");
            CHECK(line.find("private-cookie") == std::string::npos);
            last_sequence = std::max(last_sequence, record["sequence"].get<int>());
        }
    }
    CHECK(files == 3);
    CHECK(last_sequence == 39);
}

TEST_CASE("rotation failures are counted and cannot grow the active file", "[observability][logger][rotation]") {
    LogDirectory dir;
    const auto file = dir.path / "music-service.log";
    const LogRotation limits{128, 1};
    auto target = std::make_shared<RotatingFileSink>(file, false, limits);
    const LogRecord record{{}, LogLevel::info, "write", {}};
    target->write(record);
    const auto previous_size = fs::file_size(file);
    fs::create_directory(dir.path / "music-service.log.1");
    {
        AsyncLogSink sink{target};
        sink.write(record);
        sink.close();
        CHECK(sink.stats().failures == 1);
        CHECK(sink.stats().written == 0);
    }
    CHECK(fs::file_size(file) == previous_size);
    fs::remove(dir.path / "music-service.log.1");
    CHECK_NOTHROW(target->write(record));
    CHECK(fs::is_regular_file(dir.path / "music-service.log.1"));
    CHECK(fs::file_size(file) <= limits.max_file_bytes);
    const LogRecord huge{{}, LogLevel::info, std::string(200, 'x'), {}};
    CHECK_THROWS(target->write(huge));
    CHECK(fs::file_size(file) <= limits.max_file_bytes);
}

TEST_CASE("text file logs support zero archives and reject non-files", "[observability][logger][rotation]") {
    LogDirectory dir;
    const auto file = dir.path / "music-service.log";
    {
        RotatingFileSink sink{file, true, {96, 0}};
        for (int i = 0; i < 10; ++i) sink.write({{}, LogLevel::info, "a\nforged", {}});
    }
    CHECK_FALSE(fs::exists(dir.path / "music-service.log.1"));
    CHECK(fs::file_size(file) <= 96);
    std::ifstream in(file, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) CHECK(line.find("a\\nforged") != std::string::npos);
    CHECK_THROWS(RotatingFileSink(dir.path, false));
}
