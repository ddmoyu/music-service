#include "providers/migu/bundle.hpp"
#include "providers/migu/client.hpp"
#include <set>
namespace music_service::providers::migu {
void add_catalog(ports::ProviderBundle&);
void add_audio(ports::ProviderBundle&);
ports::ProviderBundle make_bundle() {
    ports::ProviderBundle b{.source = take(domain::SourceId::parse("migu")),
                            .name = "咪咕音乐",
                            .implementation_id = "migu-native",
                            .implementation_version = "0.1.12"};
    const std::set<std::string> paged{
        "search_tracks",  "search_playlists",   "search_albums",
        "search_artists", "playlist_tracks",    "album_tracks",
        "chart_tracks",   "category_playlists", "recommend_playlists"};
    for (const std::string key :
         {"search_tracks", "track_detail", "parse_links", "resolve_audio", "lyrics",
          "search_playlists", "playlist_detail", "playlist_tracks", "search_albums", "album_detail",
          "album_tracks", "search_artists", "categories", "category_playlists", "charts",
          "chart_detail", "chart_tracks", "recommend_playlists"}) {
        ports::CapabilitySpec spec{.key = key};
        if (key == "resolve_audio") {
            spec.credentials = ports::CredentialRequirement::optional;
            spec.auth_profiles = {"web_cookie"};
        }
        if (paged.contains(key))
            spec.pagination = ports::Pagination::cursor;
        if (paged.contains(key) || key == "categories" || key == "charts" || key == "chart_detail")
            spec.catalog_kind = ports::CatalogKind::live;
        b.capabilities.push_back(std::move(spec));
    }
    // 尚无可靠的校验协议，不宣称 verify_credentials。
    b.auth_profiles = {ports::AuthProfileSpec{
        .auth_profile = "web_cookie",
        .kind = ports::CredentialKind::cookie,
        .version = 1,
        .input_fields = {{.name = "cookie", .required = true, .secret = true}}}};
    add_catalog(b);
    add_audio(b);
    b.quality_hints = {domain::Quality::standard, domain::Quality::high, domain::Quality::lossless};
    return b;
}
} // namespace music_service::providers::migu
