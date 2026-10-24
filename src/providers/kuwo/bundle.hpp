#pragma once

#include "application/ports/providers/bundle.hpp"

namespace music_service::providers::kuwo {

// The Kuwo bundle. Until the T12 protocol package lands it declares no capabilities,
// so the source is listed but every capability call is CAPABILITY_UNSUPPORTED.
application::ports::ProviderBundle make_bundle();
void add_auth(application::ports::ProviderBundle& bundle);

} // namespace music_service::providers::kuwo
