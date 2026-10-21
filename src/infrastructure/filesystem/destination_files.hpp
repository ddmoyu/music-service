#pragma once

#include "application/ports/storage/destination_files.hpp"
#include "application/ports/storage/published_files.hpp"
#include "application/ports/storage/work_files.hpp"

#include <filesystem>
#include <map>
#include <string>

namespace music_service::infrastructure::filesystem {

// DestinationFiles, WorkFiles and PublishedFiles on the local file system. Every path is checked as a
// portable relative path and resolved inside the bound root (symlinks and reparse points
// included) before it is touched; moves use rename_no_replace, so an existing file is
// never replaced.
class LocalDestinationFiles final : public application::ports::DestinationFiles,
                                    public application::ports::WorkFiles,
                                    public application::ports::PublishedFiles {
public:
    // Destination id -> canonical root, as bound at startup. A root that is offline stays
    // listed and is reported unavailable while it is missing.
    explicit LocalDestinationFiles(std::map<std::string, std::filesystem::path> roots);

    domain::Result<bool> exists(const std::string& destination_id, const std::string& relative_path) override;
    domain::Result<std::optional<application::ports::FileFingerprint>>
    fingerprint(const std::string& destination_id, const std::string& relative_path) override;
    domain::Result<void> move(const std::string& destination_id, const std::string& from,
                              const std::string& to) override;
    domain::Result<void> remove_workspace(const std::string& destination_id, const std::string& task_id) override;

    [[nodiscard]] std::string relative_path(const std::string& task_id, const std::string& name) const override;
    domain::Result<std::unique_ptr<application::ports::WorkFile>> open(const std::string& destination_id,
                                                                       const std::string& task_id,
                                                                       const std::string& name,
                                                                       std::uint64_t keep_bytes) override;
    domain::Result<std::filesystem::path> local_path(const std::string& destination_id,
                                                     const std::string& relative_path) override;

    domain::Result<std::unique_ptr<application::ports::ReadableFile>>
    open_read(const std::string& destination_id, const std::string& relative_path) override;

private:
    domain::Result<std::filesystem::path> root(const std::string& destination_id) const;
    domain::Result<std::filesystem::path> resolve(const std::string& destination_id,
                                                  const std::string& relative_path) const;

    std::map<std::string, std::filesystem::path> roots_;
};

} // namespace music_service::infrastructure::filesystem
