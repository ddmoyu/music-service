#include "providers/qq/bundle.hpp"
#include "providers/qq/client.hpp"
#include <set>
namespace music_service::providers::qq {
void add_catalog(ports::ProviderBundle&);
void add_audio(ports::ProviderBundle&);
ports::ProviderBundle make_bundle() {
    ports::ProviderBundle b{.source = take(domain::SourceId::parse("qq")), .name = "QQ 音乐",
        .implementation_id = "qq-native", .implementation_version = "0.1.7"};
    const std::set<std::string> paged{"search_tracks", "search_playlists", "search_albums", "search_artists",
        "playlist_tracks", "album_tracks", "artist_tracks", "artist_albums", "chart_tracks", "category_playlists", "recommend_playlists"};
    const std::set<std::string> required{"search_tracks", "search_playlists", "search_albums", "search_artists", "verify_credentials"};
    const std::set<std::string> optional{"track_detail", "playlist_detail", "playlist_tracks", "album_detail", "album_tracks", "artist_tracks", "artist_albums", "resolve_audio"};
    for (const std::string key : {"search_tracks", "track_detail", "parse_links", "resolve_audio", "lyrics",
        "search_playlists", "playlist_detail", "playlist_tracks", "search_albums", "album_detail", "album_tracks",
        "search_artists", "artist_tracks", "artist_albums", "categories", "category_playlists", "charts", "chart_detail",
        "chart_tracks", "recommend_playlists", "verify_credentials"}) {
        ports::CapabilitySpec spec{.key = key};
        if (required.contains(key) || optional.contains(key)) {
            spec.credentials = required.contains(key) ? ports::CredentialRequirement::required : ports::CredentialRequirement::optional;
            spec.auth_profiles = {std::string{cookie_profile}};
        }
        if (paged.contains(key)) spec.pagination = ports::Pagination::cursor;
        if (paged.contains(key) || key == "charts" || key == "categories" || key == "chart_detail") spec.catalog_kind = ports::CatalogKind::live;
        b.capabilities.push_back(std::move(spec));
    }
    add_catalog(b); add_audio(b);
    b.quality_hints = {domain::Quality::standard, domain::Quality::high, domain::Quality::lossless};
    return b;
}
}
