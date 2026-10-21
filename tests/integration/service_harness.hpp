#pragma once

// An in-process music-service for integration tests: a temporary deployment (config
// file, data_dir, destination roots), "serve" on a thread and loopback clients.

#include "../unit/providers/mock_providers.hpp"

#include "infrastructure/filesystem/files.hpp"
#include "infrastructure/filesystem/paths.hpp"
#include "infrastructure/observability/logger.hpp"
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
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace music_service::test {

using json = nlohmann::json;
using namespace std::chrono_literals;

struct Output {
    int code{-1};
    std::string out;
    std::string err;
};

inline std::vector<ports::ProviderBundle> test_bundles() { return {mock_audio("aud"), mock_catalog("cat")}; }

// A temporary deployment: config file, data_dir and destination roots.
struct Deployment {
    explicit Deployment(std::string_view name)
        : dir(std::filesystem::temp_directory_path() / infrastructure::filesystem::path_from_utf8("music-service-服务-" + std::string{name})) {
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir / "下载");
        std::filesystem::create_directories(dir / "second");
        std::filesystem::create_directories(dir / "out");
        config = {{"config_version", 1},
                  {"data_dir", infrastructure::filesystem::path_to_utf8(dir / "data")},
                  {"control", {{"port", 0}}},
                  {"cors", {{"allowed_origins", {"http://127.0.0.1:5173"}}}},
                  {"destinations",
                   {{{"id", "dst_main"}, {"name", "主目录"}, {"root", infrastructure::filesystem::path_to_utf8(dir / "下载")}, {"principals", {"prn_local"}}}}}};
        write_config();
    }
    ~Deployment() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    void write_config() {
        std::ofstream file(dir / "config.json", std::ios::binary | std::ios::trunc);
        file << config.dump();
    }

    Output run(std::vector<std::string> args, server::CommandHooks hooks = {}) {
        args.insert(args.begin(), {"--config", infrastructure::filesystem::path_to_utf8(dir / "config.json")});
        if (!hooks.host.bundles) {
            hooks.host.bundles = test_bundles();
        }
        std::ostringstream out;
        std::ostringstream err;
        Output result;
        result.code = server::run_command(args, {out, err}, std::move(hooks));
        result.out = out.str();
        result.err = err.str();
        return result;
    }

    std::string read(const std::filesystem::path& path) {
        auto text = test::unwrap(infrastructure::filesystem::read_file(path, 4096));
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
            text.pop_back();
        }
        return text;
    }

    [[nodiscard]] std::filesystem::path bootstrap() const { return dir / "data" / "secrets" / "bootstrap-token.txt"; }

    std::filesystem::path dir;
    json config;
};

// Runs "serve" on a thread until stop().
struct RunningService {
    RunningService(Deployment& deployment, std::shared_ptr<infrastructure::observability::MemorySink> sink,
                   std::optional<std::vector<ports::ProviderBundle>> bundles = std::nullopt)
        : deployment_(deployment) {
        std::promise<server::StartupReport> ready;
        auto started = ready.get_future();
        server::CommandHooks hooks;
        hooks.should_stop = [this] { return stop_flag_.load(); };
        hooks.on_ready = [&ready](const server::StartupReport& started_report) { ready.set_value(started_report); };
        hooks.host.log_sink = std::move(sink);
        hooks.host.bundles = std::move(bundles);
        thread_ = std::thread([this, hooks = std::move(hooks)]() mutable {
            result_ = deployment_.run({"serve"}, std::move(hooks));
        });
        REQUIRE(started.wait_for(20s) == std::future_status::ready);
        report = started.get();
        const auto colon = report.api_base_url.rfind(':');
        port = std::stoi(report.api_base_url.substr(colon + 1));
    }
    ~RunningService() { stop(); }

    Output stop() {
        stop_flag_.store(true);
        if (thread_.joinable()) {
            thread_.join();
        }
        return result_;
    }

    httplib::Client client(const std::string& token = {}) const {
        httplib::Client c("127.0.0.1", port);
        c.set_connection_timeout(3s);
        c.set_read_timeout(10s);
        if (!token.empty()) {
            c.set_bearer_token_auth(token);
        }
        return c;
    }

    server::StartupReport report;
    int port{0};

private:
    Deployment& deployment_;
    std::atomic<bool> stop_flag_{false};
    std::thread thread_;
    Output result_;
};

inline json body(const httplib::Result& result) {
    REQUIRE(result);
    return json::parse(result->body);
}

inline std::string error_code(const httplib::Result& result) { return body(result)["error"]["code"].get<std::string>(); }

} // namespace music_service::test
