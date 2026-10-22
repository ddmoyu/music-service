// T26: external helper runner against a real child process (tests/support/fake_helper).
// Covers the handshake, scope re-checks, output limits, timeouts, crashes, cancellation,
// process-tree cleanup and the A45 rule that helper quota is held until the process
// has really exited.

#include "infrastructure/clock.hpp"
#include "infrastructure/observability/logger.hpp"
#include "providers/external/helper_runner.hpp"
#include "runtime/admission/admission_ledger.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

using namespace music_service;
using namespace std::chrono_literals;
namespace ports = application::ports;
namespace fs = std::filesystem;
using providers::external::HelperRunner;
using providers::external::HelperRunnerOptions;

namespace {

bool process_alive(long long pid) {
#ifdef _WIN32
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (process == nullptr) {
        return false;
    }
    const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
#else
    return ::kill(static_cast<pid_t>(pid), 0) == 0;
#endif
}

void set_environment(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    ::setenv(name, value, 1);
#endif
}

std::vector<long long> read_pids(const fs::path& file) {
    std::vector<long long> pids;
    std::ifstream in{file};
    long long pid = 0;
    while (in >> pid) {
        pids.push_back(pid);
    }
    return pids;
}

struct Rig {
    fs::path root;
    std::shared_ptr<infrastructure::SystemClock> clock = std::make_shared<infrastructure::SystemClock>();
    std::shared_ptr<runtime::AdmissionLedger> ledger;
    std::shared_ptr<infrastructure::observability::MemorySink> sink =
        std::make_shared<infrastructure::observability::MemorySink>();
    std::unique_ptr<HelperRunner> runner;
    fs::path pid_file;

    explicit Rig(const std::string& mode, std::uint32_t max_processes = 2,
                 std::chrono::milliseconds operation_timeout = 10s, std::chrono::milliseconds stop_grace = 2s,
                 std::chrono::milliseconds startup_timeout = 10s) {
        static std::atomic<int> counter{0};
        root = fs::temp_directory_path() / "music-service-helper-test" /
               (mode + "-" + std::to_string(++counter) + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root);
        pid_file = root / "pids.txt";

        runtime::AdmissionLimits limits;
        limits.total_connections = 8;
        limits.per_principal = 8;
        limits.per_origin = 4;
        limits.per_source[ports::TrafficClass::short_request] = 4;
        limits.helper_processes = max_processes;
        ledger = std::make_shared<runtime::AdmissionLedger>(limits, clock);

        application::config::ExternalAdapterConfig adapter;
        adapter.source = "helpersrc";
        adapter.program = fs::path{MUSIC_SERVICE_FAKE_HELPER};
        adapter.args = {mode, pid_file.string()};
        adapter.tool_version = "1.0-test";
        adapter.operations = {"extract_audio"};
        adapter.extractors = {"testx"};
        adapter.domains = {"example.com"};
        adapter.max_connections = 2;
        adapter.startup_timeout = startup_timeout;
        adapter.operation_timeout = operation_timeout;
        adapter.stop_grace = stop_grace;
        HelperRunnerOptions options;
        options.adapters = {{"fake", adapter}};
        options.work_root = root / "work";
        options.max_response_bytes = 1024 * 1024;
        runner = std::make_unique<HelperRunner>(
            options, ledger, clock,
            infrastructure::observability::Logger{sink, infrastructure::observability::LogLevel::debug, clock});
    }

    ~Rig() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    static ports::HelperCall call(std::string adapter = "fake", std::string source = "helpersrc") {
        return ports::HelperCall{std::move(adapter),
                                 ports::AdmissionRequest{.principal_id = "prn_helper",
                                                         .source = domain::SourceId::parse(source).value(),
                                                         .operation = "resolve_audio"}};
    }

    domain::Result<ports::HelperOutcome<ports::HelperAudioLocation>>
    extract(std::stop_token stop = {}, std::chrono::milliseconds budget = 30s,
            std::string link = "https://www.example.com/watch?v=abc") {
        return runner->extract_audio(call(), {std::move(link)},
                                     ports::OperationContext{std::chrono::steady_clock::now() + budget, stop});
    }
};

const ports::HelperAudioLocation& location(const domain::Result<ports::HelperOutcome<ports::HelperAudioLocation>>& r) {
    REQUIRE(r);
    REQUIRE(std::holds_alternative<ports::HelperAudioLocation>(r.value()));
    return std::get<ports::HelperAudioLocation>(r.value());
}

const domain::Error& failure(const domain::Result<ports::HelperOutcome<ports::HelperAudioLocation>>& r) {
    REQUIRE_FALSE(r);
    return r.error();
}

bool all_gone(const fs::path& pid_file) {
    const auto pids = read_pids(pid_file);
    return !pids.empty() && std::none_of(pids.begin(), pids.end(), process_alive);
}

} // namespace

TEST_CASE("a helper result passes the scope checks and comes back typed", "[helper]") {
    Rig rig{"ok"};
    const auto result = rig.extract();
    const auto& audio = location(result);
    CHECK(audio.url == "https://media.example.com/audio/a.m4a?sig=1");
    CHECK(audio.extractor == "testx");
    CHECK(audio.media_id == "abc");
    CHECK(audio.container == "m4a");
    CHECK(audio.codec == "aac");
    CHECK(audio.bitrate_kbps == 128U);
    REQUIRE(audio.expires_at);
    CHECK(*audio.expires_at > std::chrono::system_clock::now());
    // Cookie is a secret header, User-Agent an ordinary one.
    REQUIRE(audio.headers.size() == 1);
    CHECK(audio.headers[0].name == "User-Agent");
    REQUIRE(audio.secret_headers.size() == 1);
    CHECK(audio.secret_headers[0].name == "Cookie");

    CHECK(all_gone(rig.pid_file));
    CHECK(rig.ledger->usage().helpers == 0);
    CHECK(rig.ledger->usage().total == 0);
    CHECK(rig.runner->started() == 1);
    // The working directory goes with the call.
    CHECK(fs::is_empty(rig.root / "work"));
}

TEST_CASE("the helper sees only its own minimal environment", "[helper]") {
    set_environment("MUSIC_SERVICE_TEST_SECRET", "do-not-pass");
    Rig rig{"env"};
    const auto result = rig.extract();
    CHECK(location(result).media_id == "clean-protocol");
}

TEST_CASE("calls outside the adapter's declared scope never start a process", "[helper]") {
    Rig rig{"ok"};
    const ports::OperationContext context{std::chrono::steady_clock::now() + 10s, {}};

    auto unknown = rig.runner->extract_audio(Rig::call("missing"), {"https://www.example.com/a"}, context);
    CHECK(failure(unknown).code == domain::ErrorCode::capability_unsupported);

    auto other_source = rig.runner->extract_audio(Rig::call("fake", "kuwo"), {"https://www.example.com/a"}, context);
    CHECK(failure(other_source).code == domain::ErrorCode::capability_unsupported);

    // Links outside the domains, look-alike hosts and plain http are refused.
    auto outside = rig.extract({}, 10s, "https://evil.example.org/watch?v=1");
    CHECK(failure(outside).code == domain::ErrorCode::capability_unsupported);
    auto lookalike = rig.extract({}, 10s, "https://notexample.com/watch?v=1");
    CHECK(failure(lookalike).code == domain::ErrorCode::capability_unsupported);
    auto plain = rig.extract({}, 10s, "http://www.example.com/watch?v=1");
    CHECK(failure(plain).code == domain::ErrorCode::validation_failed);

    CHECK(rig.runner->started() == 0);
}

TEST_CASE("results outside the scope are refused after the helper exits", "[helper]") {
    for (const std::string mode : {"bad_url", "generic"}) {
        Rig rig{mode};
        const auto result = rig.extract();
        const auto& error = failure(result);
        CHECK(error.code == domain::ErrorCode::upstream_failed);
        CHECK_FALSE(error.retryable);
        CHECK(all_gone(rig.pid_file));
        CHECK(rig.ledger->usage().helpers == 0);
    }
}

TEST_CASE("a helper that does not match the configuration is unavailable", "[helper]") {
    SECTION("tool version") {
        Rig rig{"wrong_version"};
        CHECK(failure(rig.extract()).code == domain::ErrorCode::feature_unavailable);
        CHECK(all_gone(rig.pid_file));
    }
    SECTION("protocol version") {
        Rig rig{"wrong_protocol"};
        CHECK(failure(rig.extract()).code == domain::ErrorCode::feature_unavailable);
    }
    SECTION("missing program") {
        Rig rig{"ok"};
        HelperRunnerOptions options;
        application::config::ExternalAdapterConfig adapter;
        adapter.source = "helpersrc";
        adapter.program = rig.root / "no-such-helper.exe";
        adapter.tool_version = "1.0-test";
        adapter.operations = {"extract_audio"};
        adapter.extractors = {"testx"};
        adapter.domains = {"example.com"};
        options.adapters = {{"fake", adapter}};
        options.work_root = rig.root / "work";
        HelperRunner runner{options, rig.ledger, rig.clock};
        auto result = runner.extract_audio(Rig::call(), {"https://www.example.com/a"},
                                           {std::chrono::steady_clock::now() + 10s, {}});
        CHECK(failure(result).code == domain::ErrorCode::feature_unavailable);
        CHECK(rig.ledger->usage().helpers == 0);
    }
}

TEST_CASE("malformed, oversized and missing output fail and are reclaimed", "[helper]") {
    SECTION("not JSON") {
        Rig rig{"garbage"};
        CHECK(failure(rig.extract()).code == domain::ErrorCode::upstream_failed);
        CHECK(all_gone(rig.pid_file));
    }
    SECTION("more than the response budget") {
        Rig rig{"flood"};
        const auto& error = failure(rig.extract());
        CHECK(error.code == domain::ErrorCode::upstream_failed);
        CHECK_FALSE(error.retryable);
        CHECK(all_gone(rig.pid_file));
        CHECK(rig.ledger->usage().helpers == 0);
    }
    SECTION("exits before the hello") {
        Rig rig{"exit_early"};
        const auto& error = failure(rig.extract());
        CHECK(error.code == domain::ErrorCode::upstream_failed);
        CHECK(error.retryable);
    }
    SECTION("crashes after the request") {
        Rig rig{"crash"};
        const auto& error = failure(rig.extract());
        CHECK(error.code == domain::ErrorCode::upstream_failed);
        CHECK(error.retryable);
        // Its stderr tail was logged, scrubbed of nothing secret here but bounded.
        const auto records = rig.sink->records();
        CHECK(std::any_of(records.begin(), records.end(), [](const auto& r) { return r.event == "helper.finished"; }));
    }
    SECTION("a flood on stderr does not block the reply") {
        Rig rig{"stderr_flood"};
        CHECK(location(rig.extract()).media_id == "abc");
    }
}

TEST_CASE("a helper's rate limit cools the source down in the shared ledger", "[helper]") {
    Rig rig{"rate_limited"};
    const auto& error = failure(rig.extract());
    CHECK(error.code == domain::ErrorCode::upstream_rate_limited);
    CHECK(error.retryable);
    const auto cooldowns = rig.ledger->usage().cooldowns;
    REQUIRE(cooldowns.contains("source:helpersrc"));
    // The next call is deferred by the cooldown and starts nothing.
    const auto started = rig.runner->started();
    auto next = rig.extract();
    REQUIRE(next);
    REQUIRE(std::holds_alternative<ports::Deferred>(next.value()));
    CHECK(std::get<ports::Deferred>(next.value()).blocked_dimension == "cooldown");
    CHECK(rig.runner->started() == started);
}

TEST_CASE("A45: a timed-out helper keeps its quota until it has really exited", "[helper][a45]") {
    // stubborn ignores the cancel message, stdin and SIGTERM: only the kill after
    // stop_grace ends it.
    Rig rig{"stubborn", 1, 300ms, 1500ms};
    const auto began = std::chrono::steady_clock::now();
    auto pending = std::async(std::launch::async, [&] { return rig.extract(); });

    // Past the operation timeout, inside the grace period: the process still runs and
    // its process slot and both declared connections are still reserved.
    std::this_thread::sleep_for(900ms);
    REQUIRE(pending.wait_for(0s) == std::future_status::timeout);
    const auto pids = read_pids(rig.pid_file);
    REQUIRE(pids.size() == 1);
    CHECK(process_alive(pids[0]));
    const auto during = rig.ledger->usage();
    CHECK(during.helpers == 1);
    CHECK(during.total == 2);
    CHECK(during.origins.at("helper://fake") == 2);

    // A second call finds no free helper process and is deferred, not queued.
    auto second = rig.extract();
    REQUIRE(second);
    REQUIRE(std::holds_alternative<ports::Deferred>(second.value()));
    CHECK(std::get<ports::Deferred>(second.value()).blocked_dimension == "helper");
    CHECK(rig.runner->started() == 1);

    const auto result = pending.get();
    const auto took = std::chrono::steady_clock::now() - began;
    const auto& error = failure(result);
    CHECK(error.code == domain::ErrorCode::upstream_timeout);
    CHECK(error.retryable);
    CHECK(took >= 300ms + 1500ms);
    // By the time the call returns the process is gone and the quota is back.
    CHECK_FALSE(process_alive(pids[0]));
    const auto after = rig.ledger->usage();
    CHECK(after.helpers == 0);
    CHECK(after.total == 0);
}

TEST_CASE("the whole process tree is ended, grandchildren included", "[helper]") {
    Rig rig{"spawn_child", 2, 300ms, 200ms};
    const auto& error = failure(rig.extract());
    CHECK(error.code == domain::ErrorCode::upstream_timeout);
    const auto pids = read_pids(rig.pid_file);
    REQUIRE(pids.size() == 2); // the helper and its grandchild
    CHECK(std::none_of(pids.begin(), pids.end(), process_alive));
    CHECK(rig.ledger->usage().helpers == 0);
}

TEST_CASE("cancellation stops a cooperative helper without waiting for the timeout", "[helper]") {
    Rig rig{"cooperative", 2, 30s, 5s};
    std::stop_source stop;
    const auto began = std::chrono::steady_clock::now();
    auto pending = std::async(std::launch::async, [&] { return rig.extract(stop.get_token()); });
    std::this_thread::sleep_for(300ms);
    stop.request_stop();
    const auto result = pending.get();
    CHECK(failure(result).code == domain::ErrorCode::canceled);
    // It left on the cancel message, well before stop_grace or the timeout.
    CHECK(std::chrono::steady_clock::now() - began < 4s);
    CHECK(all_gone(rig.pid_file));
    CHECK(rig.ledger->usage().helpers == 0);
}

TEST_CASE("a slow start is a timeout and the call's deadline is respected", "[helper]") {
    SECTION("startup timeout") {
        Rig rig{"slow_hello", 2, 10s, 200ms, 300ms};
        const auto& error = failure(rig.extract());
        CHECK(error.code == domain::ErrorCode::upstream_timeout);
        CHECK(all_gone(rig.pid_file));
    }
    SECTION("call deadline") {
        Rig rig{"stubborn", 2, 30s, 200ms};
        const auto& error = failure(rig.extract({}, 400ms));
        CHECK(error.code == domain::ErrorCode::deadline_exceeded);
        CHECK(all_gone(rig.pid_file));
    }
}
