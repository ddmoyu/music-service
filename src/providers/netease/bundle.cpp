#include "providers/netease/bundle.hpp"

#include "providers/netease/capabilities.hpp"

#include <utility>

namespace music_service::providers::netease {

namespace ports = application::ports;

ports::ProviderBundle make_bundle() {
    auto source = domain::SourceId::parse("netease");
    auto bundle = ports::ProviderBundle{
        .source = std::move(source).value(),
        .name = "网易云音乐",
        .implementation_id = "netease-native",
        .implementation_version = "0.1.0",
    };
    const std::vector<std::string> cookie{std::string{cookie_profile}};
    // What N07 verified on 2026-10-22. Only audio and verification use an account.
    bundle.capabilities = {
        {.key = "search_tracks", .pagination = ports::Pagination::cursor, .catalog_kind = ports::CatalogKind::live},
        {.key = "track_detail"},
        {.key = "parse_links"},
        {.key = "lyrics"},
        {.key = "resolve_audio", .credentials = ports::CredentialRequirement::optional, .auth_profiles = cookie},
        {.key = "verify_credentials", .credentials = ports::CredentialRequirement::required, .auth_profiles = cookie},
    };
    bundle.bindings = {
        ports::CapabilityBinding::bind<ports::ITrackSearch>(make_search),
        ports::CapabilityBinding::bind<ports::ITrackDetail>(make_detail),
        ports::CapabilityBinding::bind<ports::ILinkParser>(make_link_parser),
        ports::CapabilityBinding::bind<ports::ILyrics>(make_lyrics),
        ports::CapabilityBinding::bind<ports::IAssetResolver>(make_resolver),
    };
    // The whole Cookie header of a signed-in music.163.com session; MUSIC_U is the part
    // the platform checks.
    bundle.auth_profiles = {ports::AuthProfileSpec{
        .auth_profile = std::string{cookie_profile},
        .kind = ports::CredentialKind::cookie,
        .version = 1,
        .input_fields = {{.name = "cookie", .required = true, .secret = true}},
        .scopes = {"vip"},
        .verifier = make_verifier,
    }};
    bundle.quality_hints = {domain::Quality::standard};
    return bundle;
}

} // namespace music_service::providers::netease
