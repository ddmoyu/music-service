#pragma once

#include "application/ports/providers/bundle.hpp"

#include <vector>

namespace music_service::providers::kuwo {

// Kuwo collection capabilities (T23): playlist and album search, playlist, album and
// chart details and track pages, categories with their playlists, charts and recommended
// playlists. Anonymous and paged by page number; each call asks for one upstream page.
// Collection ids are decimal (parsing::is_valid_collection_id); a chart's id is the one
// its tracks are listed by.

// The capability declarations and bindings, added to the bundle.
std::vector<application::ports::CapabilitySpec> collection_capabilities();
std::vector<application::ports::CapabilityBinding> collection_bindings();

} // namespace music_service::providers::kuwo
