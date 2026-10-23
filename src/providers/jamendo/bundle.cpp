#include "providers/jamendo/bundle.hpp"
#include "providers/jamendo/client.hpp"
#include <set>

namespace music_service::providers::jamendo {
void add_catalog(ports::ProviderBundle&);
void add_audio(ports::ProviderBundle&);
void add_discovery(ports::ProviderBundle&);
ports::ProviderBundle make_bundle() {
    ports::ProviderBundle bundle{.source = take(domain::SourceId::parse("jamendo")), .name = "Jamendo",
        .implementation_id = "jamendo-native", .implementation_version = "0.1.15"};
    const std::set<std::string> paged{"search_tracks", "search_albums", "search_artists", "album_tracks",
        "artist_albums", "playlist_tracks", "recommend_playlists", "search_playlists", "category_tracks", "chart_tracks"};
    for (const std::string key : {"search_tracks", "track_detail", "parse_links", "lyrics", "resolve_audio",
         "search_albums", "album_detail", "album_tracks", "search_artists", "artist_detail", "artist_albums",
         "playlist_detail", "playlist_tracks", "recommend_playlists", "search_playlists", "categories", "category_tracks",
         "charts", "chart_detail", "chart_tracks"}) {
        ports::CapabilitySpec spec{.key = key};
        if (paged.contains(key)) { spec.pagination = ports::Pagination::cursor; spec.catalog_kind = ports::CatalogKind::live; }
        if (key == "categories" || key == "charts" || key == "chart_detail") spec.catalog_kind = ports::CatalogKind::live;
        if (key == "search_playlists") spec.catalog_kind = ports::CatalogKind::search_derived;
        bundle.capabilities.push_back(std::move(spec));
    }
    bundle.quality_hints = {domain::Quality::standard};
    add_catalog(bundle);
    add_audio(bundle);
    add_discovery(bundle);
    return bundle;
}
}
