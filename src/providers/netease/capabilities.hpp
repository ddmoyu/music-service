#pragma once

#include "application/ports/providers/auth.hpp"
#include "application/ports/providers/bundle.hpp"

#include <functional>
#include <memory>
#include <string>

namespace music_service::providers::netease {

namespace ports = application::ports;

// The one auth profile: the cookie of a signed-in music.163.com session (MUSIC_U).
inline constexpr std::string_view cookie_profile = "web_cookie";

// Produces the weapi secret key for each request; tests pin it.
using SecretKeySource = std::function<std::string()>;

// NetEase capability objects. Each request is a weapi POST through
// ProviderSetup::upstream, so admission, TLS and redirect rules are the shared ones; a
// hop that cannot be admitted is SERVICE_BUSY (retryable, details {"admission",
// dimension}). Search, details, links and lyrics are anonymous and never send a
// cookie. Audio and verification send the bound user's cookie when there is one.

domain::Result<std::shared_ptr<ports::ITrackSearch>> make_search(const ports::ProviderSetup& setup);
domain::Result<std::shared_ptr<ports::ITrackDetail>> make_detail(const ports::ProviderSetup& setup);
domain::Result<std::shared_ptr<ports::ILinkParser>> make_link_parser(const ports::ProviderSetup& setup);
domain::Result<std::shared_ptr<ports::ILyrics>> make_lyrics(const ports::ProviderSetup& setup);
// Standard quality (128 kbit/s MP3) only. Anonymous works for songs the platform lets
// everyone play; songs that need a membership or purchase are CREDENTIALS_REQUIRED
// without a suitable account, and a trial clip is reported as a preview, never as the
// song. No copyright (or no such song) is NOT_FOUND.
domain::Result<std::shared_ptr<ports::IAssetResolver>> make_resolver(const ports::ProviderSetup& setup);
// Checks a cookie against w/nuser/account/get: signed in is valid (the nickname as the
// account hint), nobody is invalid. A cookie without MUSIC_U is invalid without a call.
domain::Result<std::shared_ptr<ports::ICredentialVerifier>> make_verifier(const ports::ProviderSetup& setup);

// For tests: the same objects with a fixed weapi secret key.
void set_secret_key_source_for_testing(SecretKeySource source);

} // namespace music_service::providers::netease
