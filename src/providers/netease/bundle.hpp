#pragma once

#include "application/ports/providers/bundle.hpp"

namespace music_service::providers::netease {

// The NetEase Cloud Music bundle (N07): anonymous search, details, links, lyrics and
// standard-quality audio, plus the web_cookie profile that lets a signed-in account
// play what anonymous callers cannot.
application::ports::ProviderBundle make_bundle();

} // namespace music_service::providers::netease
