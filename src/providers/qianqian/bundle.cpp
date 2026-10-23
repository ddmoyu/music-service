#include "providers/qianqian/bundle.hpp"
#include "providers/qianqian/client.hpp"
#include <set>

namespace music_service::providers::qianqian {
void add_catalog(ports::ProviderBundle&);
void add_audio(ports::ProviderBundle&);
ports::ProviderBundle make_bundle() {
    ports::ProviderBundle bundle{.source = take(domain::SourceId::parse("qianqian")), .name = "千千音乐",
        .implementation_id = "qianqian-native", .implementation_version = "0.1.11"};
    const std::set<std::string> paged{"search_tracks", "search_albums", "album_tracks", "playlist_tracks", "category_playlists", "recommend_playlists"};
    for (const std::string key : {"search_tracks", "track_detail", "parse_links", "lyrics", "resolve_audio",
        "search_albums", "album_detail", "album_tracks", "playlist_detail", "playlist_tracks", "categories", "category_playlists", "recommend_playlists"}) {
        ports::CapabilitySpec spec{.key = key};
        if (paged.contains(key)) spec.pagination = ports::Pagination::cursor;
        if (paged.contains(key) || key == "categories") spec.catalog_kind = ports::CatalogKind::live;
        bundle.capabilities.push_back(std::move(spec));
    }
    add_catalog(bundle);
    add_audio(bundle);
    bundle.quality_hints = {domain::Quality::standard, domain::Quality::high, domain::Quality::lossless};
    return bundle;
}
}
