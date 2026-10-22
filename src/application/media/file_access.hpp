#pragma once

#include "application/ports/storage/publication_repository.hpp"
#include "application/ports/storage/published_files.hpp"
#include "domain/result.hpp"

#include <functional>
#include <memory>
#include <string>

namespace music_service::application::media {

// A published file as its owner may read it right now.
struct OpenFile {
    ports::FileAssetRecord asset;
    std::unique_ptr<ports::ReadableFile> file;
};

// The display name of a published file: its last path component.
std::string display_name(const ports::FileAssetRecord& asset);

// FileAccessService (internal-contracts.md 3): only published, immutable files; never a
// partial download or a server path. Every open checks the owner, the destination's
// current ACL and that the file is still the published one (same size); a file that was
// moved away or changed is marked missing, its history kept, and reads are FILE_MISSING.
class FileAccessService {
public:
    // allowed(principal, destination_id): the destination's current principal list.
    FileAccessService(std::shared_ptr<ports::PublicationRepository> publications,
                      std::shared_ptr<ports::PublishedFiles> files,
                      std::function<bool(const std::string& principal_id, const std::string& destination_id)> allowed);

    // NOT_FOUND for unknown, foreign or no longer permitted files; FILE_MISSING for files
    // gone from their destination; DESTINATION_UNAVAILABLE while the root is offline.
    domain::Result<OpenFile> open(const std::string& owner_id, const std::string& file_id, const std::string& part = "audio");

    // The metadata after the same checks (the file is opened and closed again).
    domain::Result<ports::FileAssetRecord> describe(const std::string& owner_id, const std::string& file_id);

private:
    std::shared_ptr<ports::PublicationRepository> publications_;
    std::shared_ptr<ports::PublishedFiles> files_;
    std::function<bool(const std::string&, const std::string&)> allowed_;
};

} // namespace music_service::application::media
