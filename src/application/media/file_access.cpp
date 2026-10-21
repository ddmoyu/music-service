#include "application/media/file_access.hpp"

#include <utility>

namespace music_service::application::media {

namespace {

using domain::Error;
using domain::ErrorCode;

Error missing() {
    return Error{ErrorCode::file_missing, "the file is no longer available", false, {}};
}

} // namespace

std::string display_name(const ports::FileAssetRecord& asset) {
    const auto slash = asset.relative_path.rfind('/');
    return slash == std::string::npos ? asset.relative_path : asset.relative_path.substr(slash + 1);
}

FileAccessService::FileAccessService(std::shared_ptr<ports::PublicationRepository> publications,
                                     std::shared_ptr<ports::PublishedFiles> files,
                                     std::function<bool(const std::string&, const std::string&)> allowed)
    : publications_(std::move(publications)), files_(std::move(files)), allowed_(std::move(allowed)) {}

domain::Result<OpenFile> FileAccessService::open(const std::string& owner_id, const std::string& file_id) {
    auto asset = publications_->get_asset(owner_id, file_id);
    if (!asset) {
        return std::move(asset).error();
    }
    if (!allowed_(owner_id, asset->destination_id)) {
        // Access always follows the destination's current ACL, like an unknown file.
        return Error{ErrorCode::not_found, "no such file", false, {}};
    }
    if (asset->status == ports::FileAssetStatus::missing) {
        return missing();
    }
    auto file = files_->open_read(asset->destination_id, asset->relative_path);
    if (!file) {
        if (file.error().code != ErrorCode::not_found) {
            return std::move(file).error();
        }
    } else if (file.value()->size() == asset->size_bytes) {
        return OpenFile{std::move(asset).value(), std::move(file).value()};
    }
    // Moved away or rewritten: not the published bytes any more. The record stays.
    (void)publications_->mark_missing(file_id);
    return missing();
}

domain::Result<ports::FileAssetRecord> FileAccessService::describe(const std::string& owner_id,
                                                                   const std::string& file_id) {
    auto opened = open(owner_id, file_id);
    if (!opened) {
        return std::move(opened).error();
    }
    return std::move(opened->asset);
}

} // namespace music_service::application::media
