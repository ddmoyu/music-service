#pragma once

#include "domain/result.hpp"
#include "domain/tasks/download_intent.hpp"

#include <string>
#include <string_view>

namespace music_service::infrastructure::storage {

// Persistent JSON form of a NormalizedDownloadIntent (column normalized_intent).
// Decoding re-runs domain normalization, so a stored intent is validated again and
// an incompatible or tampered record fails instead of being used blindly.
std::string encode_intent(const domain::NormalizedDownloadIntent& intent);
domain::Result<domain::NormalizedDownloadIntent> decode_intent(std::string_view json);

} // namespace music_service::infrastructure::storage
