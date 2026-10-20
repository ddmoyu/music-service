#pragma once

#include "application/config/service_config.hpp"
#include "domain/result.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace music_service::infrastructure::config {

// What this binary was built with. Enabling something the build lacks fails startup.
struct BuildCapabilities {
    bool media_server{false};
    bool event_server{false};
    bool media_proxy{false};
    bool direct_media_urls{false};
    bool automatic_fallback{false};
    bool transcoding{false};
    bool external_adapters{false};
    bool app_profiles{false};
    std::set<std::string, std::less<>> providers;
};

using EnvironmentLookup = std::function<std::optional<std::string>(std::string_view name)>;

// Reads the process environment.
std::optional<std::string> process_environment(std::string_view name);

struct LoadOptions {
    // From --config; wins over MUSIC_SERVICE_CONFIG.
    std::optional<std::filesystem::path> config_file;
    // From --data-dir; wins over MUSIC_SERVICE_DATA_DIR and the file.
    std::optional<std::filesystem::path> data_dir;
    // Base for relative paths given on the command line or in the environment.
    std::filesystem::path working_directory;
    EnvironmentLookup environment{process_environment};
    BuildCapabilities build;
};

// Applies built-in defaults, the JSON file, the listed MUSIC_SERVICE_* variables and the
// command-line overrides, in that order, then validates. Relative paths in the file
// resolve against the file's directory; relative paths from the environment or the
// command line resolve against working_directory. Unknown keys, wrong types and
// out-of-range values are errors; all problems found are reported together in
// Error::details with a JSON-pointer-like field path.
domain::Result<application::config::ServiceConfig> load_service_config(const LoadOptions& options);

// Same as load_service_config but for JSON text already in memory; file_path is used
// only for resolving relative paths and for diagnostics.
domain::Result<application::config::ServiceConfig>
load_service_config_text(std::string_view json_text, const std::filesystem::path& file_path,
                         const LoadOptions& options);

// Cross-field validation of an already loaded configuration.
domain::Result<void> validate_service_config(const application::config::ServiceConfig& config,
                                             const BuildCapabilities& build);

} // namespace music_service::infrastructure::config
