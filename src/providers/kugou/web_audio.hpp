#pragma once
#include "providers/kugou/client.hpp"

namespace music_service::providers::kugou {
// Throws a sanitized domain Error; called inside the adapter's checked boundary.
ports::ResolvedAudio resolve_web_audio(const Client&, const ports::ProviderContext&,
                                       const domain::TrackRef&, const domain::ResourceRequirement&);
}
