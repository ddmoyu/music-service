#pragma once

#include "domain/result.hpp"

#include <filesystem>
#include <string_view>

namespace music_service::infrastructure::filesystem {

// Fixed layout below data_dir (configuration-and-deployment.md, 运行数据布局).
struct DataLayout {
    std::filesystem::path root;
    std::filesystem::path lock_file;       // instance.lock
    std::filesystem::path database;        // service.sqlite3
    std::filesystem::path secrets;         // secrets/          owner only
    std::filesystem::path credentials;     // secrets/credentials/  owner only
    std::filesystem::path bootstrap_token; // secrets/bootstrap-token.txt
    std::filesystem::path cache;           // cache/            disposable
    std::filesystem::path logs;            // logs/
    std::filesystem::path backups;         // backups/          owner only: database copies

    static DataLayout under(const std::filesystem::path& data_dir);
};

// Creates data_dir and its subdirectories (secrets owner-only), verifies that it can
// be written, and returns the layout rooted at the canonical data_dir.
domain::Result<DataLayout> prepare_data_dir(const std::filesystem::path& data_dir);

// Checks a destination root before it is registered or used: it must already exist as
// a writable directory (it is never created, so an unmounted share cannot be replaced
// by an empty local directory) and must not contain or sit inside data_dir.
// Returns the canonical root, the value stored as the immutable destination binding.
domain::Result<std::filesystem::path>
check_destination_root(const std::filesystem::path& root,
                       const std::filesystem::path& canonical_data_dir);

inline constexpr std::string_view workspace_directory_name = ".music-service-work";

// <root>/.music-service-work/<task_id>, created on demand. Lives on the destination's file
// system so publishing is a same-device rename.
domain::Result<std::filesystem::path> task_workspace(const std::filesystem::path& canonical_root,
                                                     std::string_view task_id);

} // namespace music_service::infrastructure::filesystem
