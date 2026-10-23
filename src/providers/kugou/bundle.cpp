#include "providers/kugou/bundle.hpp"
#include "providers/kugou/artists.hpp"
#include "providers/kugou/client.hpp"
#include <set>
namespace music_service::providers::kugou {
void add_catalog(ports::ProviderBundle&);
void add_audio(ports::ProviderBundle&);
ports::ProviderBundle make_bundle() {
    ports::ProviderBundle b{.source = take(domain::SourceId::parse("kugou")),
                            .name = "酷狗音乐",
                            .implementation_id = "kugou-native",
                            .implementation_version = "0.1.17"};
    const std::set<std::string> paged{"search_tracks",      "search_playlists",    "search_albums",
                                      "playlist_tracks",    "album_tracks",        "chart_tracks",
                                      "category_playlists", "recommend_playlists", "search_artists",
                                      "artist_tracks",      "artist_albums"};
    for (const std::string key : {"search_tracks",
                                  "track_detail",
                                  "parse_links",
                                  "resolve_audio",
                                  "lyrics",
                                  "search_playlists",
                                  "playlist_detail",
                                  "playlist_tracks",
                                  "search_albums",
                                  "album_detail",
                                  "album_tracks",
                                  "categories",
                                  "category_playlists",
                                  "charts",
                                  "chart_detail",
                                  "chart_tracks",
                                  "recommend_playlists",
                                  "search_artists",
                                  "artist_detail",
                                  "artist_tracks",
                                  "artist_albums"}) {
        ports::CapabilitySpec spec{.key = key};
        if (paged.contains(key))
            spec.pagination = ports::Pagination::cursor;
        if (paged.contains(key) || key == "charts" || key == "chart_detail" || key == "categories")
            spec.catalog_kind = ports::CatalogKind::live;
        b.capabilities.push_back(std::move(spec));
    }
    b.plaintext_endpoints = plaintext_endpoints();
    b.quality_hints = {domain::Quality::standard, domain::Quality::high, domain::Quality::lossless};
    add_catalog(b);
    add_audio(b);
    add_artists(b);
    return b;
}
} // namespace music_service::providers::kugou
