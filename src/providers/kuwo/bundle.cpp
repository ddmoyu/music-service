#include "providers/kuwo/bundle.hpp"

#include "providers/kuwo/capabilities.hpp"

#include <utility>

namespace music_service::providers::kuwo {

namespace ports = application::ports;

ports::ProviderBundle make_bundle() {
    auto source = domain::SourceId::parse("kuwo");
    auto bundle = ports::ProviderBundle{
        .source = std::move(source).value(),
        .name = "酷我音乐",
        .implementation_id = "kuwo-native",
        .implementation_version = "0.1.0",
    };
    // Only what T12 verified: anonymous search, details, song links and standard
    // quality audio. No credentials are registered; nothing needs them yet.
    bundle.capabilities = {
        {.key = "search_tracks", .pagination = ports::Pagination::cursor, .catalog_kind = ports::CatalogKind::live},
        {.key = "track_detail"},
        {.key = "parse_links"},
        {.key = "resolve_audio"},
    };
    bundle.bindings = {
        ports::CapabilityBinding::bind<ports::ITrackSearch>(make_search),
        ports::CapabilityBinding::bind<ports::ITrackDetail>(make_detail),
        ports::CapabilityBinding::bind<ports::ILinkParser>(make_link_parser),
        ports::CapabilityBinding::bind<ports::IAssetResolver>(make_resolver),
    };
    bundle.quality_hints = {domain::Quality::standard};
    return bundle;
}

} // namespace music_service::providers::kuwo
