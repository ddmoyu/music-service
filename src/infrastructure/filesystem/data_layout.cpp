#include "infrastructure/filesystem/data_layout.hpp"

#include "infrastructure/filesystem/files.hpp"
#include "infrastructure/filesystem/paths.hpp"
#include "infrastructure/observability/correlation.hpp"

#include "domain/catalog/refs.hpp"

namespace music_service::infrastructure::filesystem {

namespace fs = std::filesystem;

namespace {

// Proves the directory accepts new files without leaving anything behind.
domain::Result<void> check_writable(const fs::path& directory) {
    static observability::IdGenerator ids;
    const auto probe = directory / (".write-probe-" + ids.next(""));
    if (auto written = write_new_file(probe, "", Privacy::normal); !written) {
        return written;
    }
    return remove_file(probe);
}

} // namespace

DataLayout DataLayout::under(const fs::path& data_dir) {
    DataLayout layout;
    layout.root = data_dir;
    layout.lock_file = data_dir / "instance.lock";
    layout.database = data_dir / "service.sqlite3";
    layout.secrets = data_dir / "secrets";
    layout.credentials = layout.secrets / "credentials";
    layout.bootstrap_token = layout.secrets / "bootstrap-token.txt";
    layout.cache = data_dir / "cache";
    layout.logs = data_dir / "logs";
    return layout;
}

domain::Result<DataLayout> prepare_data_dir(const fs::path& data_dir) {
    if (auto created = create_directories(data_dir, Privacy::normal); !created) {
        return created.error();
    }
    auto canonical = canonical_directory(data_dir);
    if (!canonical) {
        return std::move(canonical).error();
    }
    auto layout = DataLayout::under(canonical.value());
    for (const auto& [path, privacy] :
         {std::pair{layout.secrets, Privacy::owner_only},
          std::pair{layout.credentials, Privacy::owner_only},
          std::pair{layout.cache, Privacy::normal}, std::pair{layout.logs, Privacy::normal}}) {
        if (auto created = create_directories(path, privacy); !created) {
            return created.error();
        }
    }
    if (auto writable = check_writable(layout.root); !writable) {
        return writable.error();
    }
    return layout;
}

domain::Result<fs::path> check_destination_root(const fs::path& root,
                                                const fs::path& canonical_data_dir) {
    auto canonical = canonical_directory(root);
    if (!canonical) {
        return std::move(canonical).error();
    }
    const auto& resolved = canonical.value();
    if (is_within(resolved, canonical_data_dir) || is_within(canonical_data_dir, resolved)) {
        return domain::Error{domain::ErrorCode::validation_failed,
                             "destination root '" + path_to_utf8(resolved) +
                                 "' must not contain or be inside data_dir",
                             false,
                             {}};
    }
    if (auto writable = check_writable(resolved); !writable) {
        auto error = writable.error();
        error.code = domain::ErrorCode::destination_unavailable;
        return error;
    }
    return resolved;
}

domain::Result<fs::path> task_workspace(const fs::path& canonical_root, std::string_view task_id) {
    if (!domain::is_valid_service_id("tsk_", task_id)) {
        return domain::Error::validation("task_id", "must be a tsk_ service id");
    }
    const auto workspace =
        canonical_root / fs::path{workspace_directory_name} / path_from_utf8(task_id);
    if (auto created = create_directories(workspace, Privacy::normal); !created) {
        auto error = created.error();
        error.code = domain::ErrorCode::destination_unavailable;
        return error;
    }
    return workspace;
}

} // namespace music_service::infrastructure::filesystem
