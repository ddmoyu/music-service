#include "providers/fivesing/bundle.hpp"

#include "providers/fivesing/capabilities.hpp"
#include "providers/fivesing/protocol/requests.hpp"

#include <utility>

namespace music_service::providers::fivesing {

namespace ports = application::ports;

ports::ProviderBundle make_bundle() {
    auto source = domain::SourceId::parse(protocol::source);
    auto bundle = ports::ProviderBundle{
        .source = std::move(source).value(),
        .name = "5sing",
        .implementation_id = "fivesing-native",
        .implementation_version = "0.1.0",
    };
    // What T22 and T23 verified on 2026-10-22; nothing needs an account.
    bundle.capabilities = {
        {.key = "search_tracks", .pagination = ports::Pagination::cursor, .catalog_kind = ports::CatalogKind::live},
        {.key = "track_detail"},
        {.key = "parse_links"},
        {.key = "lyrics"},
        {.key = "resolve_audio"},
        {.key = "search_playlists", .pagination = ports::Pagination::cursor, .catalog_kind = ports::CatalogKind::live},
        {.key = "playlist_detail"},
        {.key = "playlist_tracks", .pagination = ports::Pagination::cursor},
    };
    bundle.bindings = {
        ports::CapabilityBinding::bind<ports::ITrackSearch>(make_search),
        ports::CapabilityBinding::bind<ports::ITrackDetail>(make_detail),
        ports::CapabilityBinding::bind<ports::ILinkParser>(make_link_parser),
        ports::CapabilityBinding::bind<ports::ILyrics>(make_lyrics),
        ports::CapabilityBinding::bind<ports::IAssetResolver>(make_resolver),
        ports::CapabilityBinding::bind<ports::PlaylistSearch>(make_playlist_search),
        ports::CapabilityBinding::bind<ports::PlaylistDetail>(make_playlist_detail),
        ports::CapabilityBinding::bind<ports::PlaylistTracks>(make_playlist_tracks),
    };
    bundle.quality_hints = {domain::Quality::standard, domain::Quality::high};
    bundle.plaintext_endpoints = protocol::plaintext_endpoints();
    return bundle;
}

} // namespace music_service::providers::fivesing
