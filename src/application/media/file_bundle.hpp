#pragma once
#include "application/ports/storage/destination_files.hpp"
#include "domain/result.hpp"
#include <string_view>
#include <vector>

namespace music_service::application::media {
struct BundleMember {
    std::string name; // single safe filename, relative to the reserved bundle directory
    std::string kind; // audio | lyrics | cover
    std::string container;
    ports::FileFingerprint file;
};
// Empty for legacy single-file assets. Malformed manifests fail closed.
domain::Result<std::vector<BundleMember>> bundle_members(std::string_view properties);
std::string with_bundle(std::string_view properties, const std::vector<BundleMember>& members);
std::string sibling_path(std::string_view audio_path, std::string_view filename);
} // namespace music_service::application::media
