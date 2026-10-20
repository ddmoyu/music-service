#include "infrastructure/config/config_loader.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

using namespace music_dl;
using infrastructure::config::BuildCapabilities;
using infrastructure::config::LoadOptions;
using infrastructure::config::load_service_config;
using infrastructure::config::load_service_config_text;

namespace fs = std::filesystem;

namespace {

fs::path test_root() { return fs::temp_directory_path() / "music-dl-config-test"; }

LoadOptions options(std::map<std::string, std::string> env = {}, BuildCapabilities build = {}) {
    LoadOptions o;
    o.working_directory = test_root() / "cwd";
    o.environment = [env = std::move(env)](std::string_view name) -> std::optional<std::string> {
        const auto it = env.find(std::string{name});
        return it == env.end() ? std::nullopt : std::optional<std::string>{it->second};
    };
    o.build = std::move(build);
    return o;
}

fs::path config_path() { return test_root() / "conf" / "music-dl.json"; }

domain::Result<application::config::ServiceConfig> load(std::string_view text,
                                                          const LoadOptions& o = options()) {
    return load_service_config_text(text, config_path(), o);
}

bool has_problem(const domain::Error& error, std::string_view field) {
    return std::any_of(error.details.begin(), error.details.end(),
                       [field](const domain::ErrorDetail& d) { return d.field == field; });
}

BuildCapabilities full_build() {
    BuildCapabilities build;
    build.media_server = true;
    build.event_server = true;
    build.providers = {"kuwo"};
    return build;
}

} // namespace

TEST_CASE("defaults apply without a configuration file", "[config]") {
    const auto config = load_service_config(options());
    REQUIRE(config);
    CHECK(config->control.host == "127.0.0.1");
    CHECK(config->control.port == 17632);
    CHECK(config->control.workers == 16);
    CHECK_FALSE(config->media.enabled);
    CHECK_FALSE(config->events.enabled);
    CHECK(config->events.port == 17634);
    CHECK(config->event_limits.global_subscriptions == 14);
    CHECK(config->media_limits.per_principal == 4);
    CHECK(config->downloads.workers == 3);
    CHECK(config->download_timeouts.absolute == std::chrono::seconds{1800});
    CHECK(config->idempotency.retention == std::chrono::hours{24});
    CHECK(config->data_dir == (test_root() / "cwd" / "data").lexically_normal());
    CHECK_FALSE(config->config_file.has_value());
}

TEST_CASE("relative paths in the file resolve against the file's directory", "[config]") {
    const auto config = load(R"({
        "data_dir": "./state",
        "destinations": [{"id": "dst_default", "name": "默认下载", "root": "../music"}]
    })");
    REQUIRE(config);
    CHECK(config->data_dir == (test_root() / "conf" / "state").lexically_normal());
    REQUIRE(config->destinations.size() == 1);
    CHECK(config->destinations[0].root == (test_root() / "music").lexically_normal());
    CHECK(config->destinations[0].accept_new_tasks);
    CHECK(config->destinations[0].name == "默认下载");
}

TEST_CASE("unknown keys, wrong types and ranges are all reported", "[config]") {
    const auto config = load(R"({
        "control": {"prot": 1, "workers": "16"},
        "downloads": {"max_attempts": 0},
        "upstream": {"redirect_max": -1},
        "extra": true
    })");
    REQUIRE_FALSE(config);
    const auto& error = config.error();
    CHECK(error.code == domain::ErrorCode::validation_failed);
    CHECK(has_problem(error, "/control/prot"));
    CHECK(has_problem(error, "/control/workers"));
    CHECK(has_problem(error, "/downloads/max_attempts"));
    CHECK(has_problem(error, "/upstream/redirect_max"));
    CHECK(has_problem(error, "/extra"));
    CHECK(error.details.size() == 5);
}

TEST_CASE("malformed JSON and unreadable files fail", "[config]") {
    CHECK_FALSE(load("{\"control\": "));
    auto o = options();
    o.config_file = test_root() / "missing.json";
    const auto missing = load_service_config(o);
    REQUIRE_FALSE(missing);
    CHECK(has_problem(missing.error(), "config_file"));
}

TEST_CASE("features the build lacks cannot be enabled", "[config]") {
    const auto config = load(R"({
        "events": {"enabled": true},
        "features": {"media_proxy": true, "transcoding": true},
        "external_adapters": {"enabled": true},
        "app_profiles": {"spotify_app": {}}
    })");
    REQUIRE_FALSE(config);
    const auto& error = config.error();
    CHECK(has_problem(error, "/events/enabled"));
    CHECK(has_problem(error, "/features/media_proxy"));
    CHECK(has_problem(error, "/features/transcoding"));
    CHECK(has_problem(error, "/external_adapters/enabled"));
    CHECK(has_problem(error, "/app_profiles"));
}

TEST_CASE("listener conflicts and capacity rules are checked", "[config]") {
    const auto conflict = load(R"({
        "media": {"enabled": true, "port": 17632},
        "events": {"enabled": true, "workers": 8}
    })",
                               options({}, full_build()));
    REQUIRE_FALSE(conflict);
    CHECK(has_problem(conflict.error(), "/media/port"));
    CHECK(has_problem(conflict.error(), "/event_limits/global_subscriptions"));

    const auto fine = load(R"({"media": {"enabled": true}, "events": {"enabled": true},
                               "features": {"file_access": true}})",
                           options({}, full_build()));
    REQUIRE(fine);
    CHECK(fine->features.file_access);
}

TEST_CASE("providers and default sources must exist in the build", "[config]") {
    const auto unknown = load(R"({"providers": {"kuwo": {}}, "catalog": {"default_sources": ["kuwo"]}})");
    REQUIRE_FALSE(unknown);
    CHECK(has_problem(unknown.error(), "/providers/kuwo"));

    const auto known = load(R"({"providers": {"kuwo": {"options": {"region": "cn"}}},
                                "catalog": {"default_sources": ["kuwo"]}})",
                            options({}, full_build()));
    REQUIRE(known);
    CHECK(known->providers.at("kuwo").enabled);
    CHECK(known->providers.at("kuwo").options_json == R"({"region":"cn"})");

    const auto disabled = load(R"({"providers": {"kuwo": {"enabled": false}},
                                   "catalog": {"default_sources": ["kuwo"]}})",
                               options({}, full_build()));
    REQUIRE_FALSE(disabled);
    CHECK(has_problem(disabled.error(), "/catalog/default_sources"));

    CHECK_FALSE(load(R"({"providers": {"Bad-Name": {}}})", options({}, full_build())));
}

TEST_CASE("environment overrides the file and the command line overrides both", "[config]") {
    const std::string text = R"({"data_dir": "file-data", "control": {"port": 18000}})";

    const auto from_file = load(text);
    REQUIRE(from_file);
    CHECK(from_file->control.port == 18000);
    CHECK(from_file->data_dir == (test_root() / "conf" / "file-data").lexically_normal());

    const auto from_env = load(text, options({{"MUSIC_DL_DATA_DIR", "env-data"},
                                              {"MUSIC_DL_CONTROL_LISTEN", "127.0.0.1:19000"}}));
    REQUIRE(from_env);
    CHECK(from_env->control.port == 19000);
    CHECK(from_env->data_dir == (test_root() / "cwd" / "env-data").lexically_normal());

    auto cli = options({{"MUSIC_DL_DATA_DIR", "env-data"}});
    cli.data_dir = fs::path{"cli-data"};
    const auto from_cli = load(text, cli);
    REQUIRE(from_cli);
    CHECK(from_cli->data_dir == (test_root() / "cwd" / "cli-data").lexically_normal());

    CHECK_FALSE(load(text, options({{"MUSIC_DL_CONTROL_LISTEN", "no-port"}})));
}

TEST_CASE("remote listeners need public https URLs", "[config]") {
    const auto no_url = load("{}", options({{"MUSIC_DL_CONTROL_LISTEN", "0.0.0.0:17632"}}));
    REQUIRE_FALSE(no_url);
    CHECK(has_problem(no_url.error(), "/control/host"));

    const auto plain_http = load("{}", options({{"MUSIC_DL_CONTROL_LISTEN", "0.0.0.0:17632"},
                                                {"MUSIC_DL_PUBLIC_API_URL", "http://nas.lan:17632"}}));
    REQUIRE_FALSE(plain_http);
    CHECK(has_problem(plain_http.error(), "/public_urls/api_base"));

    const auto https = load("{}", options({{"MUSIC_DL_CONTROL_LISTEN", "0.0.0.0:17632"},
                                           {"MUSIC_DL_PUBLIC_API_URL", "https://music.example"}}));
    CHECK(https);

    const auto allowed = load(R"({"security": {"allow_insecure_remote": true}})",
                              options({{"MUSIC_DL_CONTROL_LISTEN", "0.0.0.0:17632"},
                                       {"MUSIC_DL_PUBLIC_API_URL", "http://nas.lan:17632"}}));
    CHECK(allowed);

    CHECK(load(R"({"public_urls": {"api_base": "http://127.0.0.1:17632"}})"));
    CHECK_FALSE(load(R"({"public_urls": {"api_base": "https://user:pw@music.example"}})"));
    CHECK_FALSE(load(R"({"public_urls": {"api_base": "https://music.example/?t=1"}})"));
}

TEST_CASE("port 0 is limited to loopback", "[config]") {
    CHECK(load(R"({"control": {"port": 0}})"));
    const auto remote = load(R"({"control": {"host": "0.0.0.0", "port": 0},
                                 "public_urls": {"api_base": "https://music.example"}})");
    REQUIRE_FALSE(remote);
    CHECK(has_problem(remote.error(), "/control/port"));
}

TEST_CASE("the download directory override needs dst_default", "[config]") {
    const auto missing = load("{}", options({{"MUSIC_DL_DOWNLOAD_DIR", "dl"}}));
    REQUIRE_FALSE(missing);
    CHECK(has_problem(missing.error(), "env:MUSIC_DL_DOWNLOAD_DIR"));

    const auto applied = load(R"({"destinations": [{"id": "dst_default", "root": "music"}]})",
                              options({{"MUSIC_DL_DOWNLOAD_DIR", "dl"}}));
    REQUIRE(applied);
    CHECK(applied->destinations[0].root == (test_root() / "cwd" / "dl").lexically_normal());
}

TEST_CASE("destinations are validated", "[config]") {
    const auto config = load(R"({"destinations": [
        {"id": "dst_a", "root": "a", "principals": ["prn_local", "bob"]},
        {"id": "dst_a", "root": "b"},
        {"id": "music", "root": "c"},
        {"name": "no id"}
    ]})");
    REQUIRE_FALSE(config);
    const auto& error = config.error();
    CHECK(has_problem(error, "/destinations/0/principals"));
    CHECK(has_problem(error, "/destinations/1/id"));
    CHECK(has_problem(error, "/destinations/2/id"));
    CHECK(has_problem(error, "/destinations/3/id"));
    CHECK(has_problem(error, "/destinations/3/root"));
}

TEST_CASE("a real file with BOM and non-ASCII paths loads", "[config]") {
    const auto dir = test_root() / fs::path{u8"配置"};
    fs::create_directories(dir);
    const auto file = dir / "music-dl.json";
    {
        std::ofstream out{file, std::ios::binary};
        out << "\xEF\xBB\xBF" << R"({"data_dir": "数据", "logging": {"format": "text"}})";
    }
    auto o = options();
    o.config_file = file;
    const auto config = load_service_config(o);
    REQUIRE(config);
    CHECK(config->data_dir == (dir / fs::path{u8"数据"}).lexically_normal());
    CHECK(config->logging.format == "text");
    CHECK(config->config_file == file.lexically_normal());
    fs::remove_all(test_root());
}
