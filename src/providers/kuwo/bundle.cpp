#include "providers/kuwo/bundle.hpp"

#include <utility>

namespace music_service::providers::kuwo {

application::ports::ProviderBundle make_bundle() {
    auto source = domain::SourceId::parse("kuwo");
    return application::ports::ProviderBundle{
        .source = std::move(source).value(),
        .name = "酷我音乐",
        .implementation_id = "kuwo-native",
        .implementation_version = "0.0.0",
    };
}

} // namespace music_service::providers::kuwo
