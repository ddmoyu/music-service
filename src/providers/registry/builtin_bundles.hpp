#pragma once

#include "application/ports/providers/bundle.hpp"

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace music_service::providers {

// Bundles of the platform packages selected at build time (MUSIC_SERVICE_PROVIDERS).
// Generated from the build manifest; every entry calls a factory function directly.
std::vector<application::ports::ProviderBundle> builtin_bundles();

// The sources builtin_bundles() provides, for configuration validation.
std::set<std::string, std::less<>> builtin_sources();

} // namespace music_service::providers
