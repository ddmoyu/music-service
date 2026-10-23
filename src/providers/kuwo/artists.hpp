#pragma once
#include "application/ports/providers/bundle.hpp"

namespace music_service::providers::kuwo {
std::vector<application::ports::CapabilitySpec> artist_capabilities();
std::vector<application::ports::CapabilityBinding> artist_bindings();
}
