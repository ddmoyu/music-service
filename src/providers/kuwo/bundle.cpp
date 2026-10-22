#include "providers/kuwo/bundle.hpp"

#include "providers/kuwo/capabilities.hpp"
#include "providers/kuwo/collections.hpp"

#include <utility>

namespace music_service::providers::kuwo {

namespace ports = application::ports;

ports::ProviderBundle make_bundle() {
    auto source = domain::SourceId::parse("kuwo");
    auto bundle = ports::ProviderBundle{
        .source = std::move(source).value(),
        .name = "酷我音乐",
        .implementation_id = "kuwo-native",
        .implementation_version = "0.1.5",
    };
    // 已验证的匿名能力；不登记无实际用途的凭据。
    bundle.capabilities = {
        {.key = "search_tracks", .pagination = ports::Pagination::cursor, .catalog_kind = ports::CatalogKind::live},
        {.key = "track_detail"},
        {.key = "parse_links"},
        {.key = "lyrics"},
        {.key = "resolve_audio"},
    };
    bundle.bindings = {
        ports::CapabilityBinding::bind<ports::ITrackSearch>(make_search),
        ports::CapabilityBinding::bind<ports::ITrackDetail>(make_detail),
        ports::CapabilityBinding::bind<ports::ILinkParser>(make_link_parser),
        ports::CapabilityBinding::bind<ports::ILyrics>(make_lyrics),
        ports::CapabilityBinding::bind<ports::IAssetResolver>(make_resolver),
    };
    // T23: the collection capabilities verified on 2026-10-22.
    for (auto& spec : collection_capabilities()) {
        bundle.capabilities.push_back(std::move(spec));
    }
    for (auto& binding : collection_bindings()) {
        bundle.bindings.push_back(std::move(binding));
    }
    bundle.quality_hints = {domain::Quality::standard, domain::Quality::high, domain::Quality::lossless};
    return bundle;
}

} // namespace music_service::providers::kuwo
