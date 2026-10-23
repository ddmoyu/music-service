#include "providers/bilibili/internal.hpp"

namespace music_service::providers::bilibili {
ports::ProviderBundle make_bundle() {
    ports::ProviderBundle bundle{.source = take(domain::SourceId::parse("bilibili")),
                                 .name = "Bilibili",
                                 .implementation_id = "bilibili-native",
                                 .implementation_version = "0.1.0"};
    for (const std::string key :
         {"search_tracks", "track_detail", "parse_links", "resolve_audio", "verify_credentials"}) {
        ports::CapabilitySpec spec{.key = key};
        if (key != "parse_links") {
            spec.credentials = key == "verify_credentials" ? ports::CredentialRequirement::required
                                                           : ports::CredentialRequirement::optional;
            spec.auth_profiles = {std::string{cookie_profile}};
        }
        if (key == "search_tracks") {
            spec.pagination = ports::Pagination::cursor;
            spec.catalog_kind = ports::CatalogKind::live;
        }
        bundle.capabilities.push_back(std::move(spec));
    }
    add_catalog(bundle);
    add_audio(bundle);
    // AAC 的更高带宽不代表无损。高品质档须另做文件品质验收再开放。
    bundle.quality_hints = {domain::Quality::standard};
    return bundle;
}
} // namespace music_service::providers::bilibili
