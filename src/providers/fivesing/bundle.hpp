#pragma once

#include "application/ports/providers/bundle.hpp"

namespace music_service::providers::fivesing {

// The 5sing bundle (source "fivesing"): anonymous search, details, song links, lyrics
// and MP3 audio of original, cover and accompaniment works.
application::ports::ProviderBundle make_bundle();

} // namespace music_service::providers::fivesing
