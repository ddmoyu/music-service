#pragma once

#include "application/ports/providers/bundle.hpp"

#include <memory>

namespace music_service::providers::fivesing {

namespace ports = application::ports;

// 5sing capability objects. Each request goes through ProviderSetup::upstream, so
// admission, TLS and redirect rules are the shared ones; a hop that cannot be admitted
// is SERVICE_BUSY (retryable, details {"admission", dimension}). Anonymous: no cookie is
// ever sent, which the plaintext API endpoints would refuse anyway.

domain::Result<std::shared_ptr<ports::ITrackSearch>> make_search(const ports::ProviderSetup& setup);
domain::Result<std::shared_ptr<ports::ITrackDetail>> make_detail(const ports::ProviderSetup& setup);
domain::Result<std::shared_ptr<ports::ILinkParser>> make_link_parser(const ports::ProviderSetup& setup);
domain::Result<std::shared_ptr<ports::ILyrics>> make_lyrics(const ports::ProviderSetup& setup);
// standard (lq 128 kbit/s, hq 192 kbit/s when lq is missing) and high (sq 320 kbit/s)
// MP3. One tier's primary address is checked with HEAD; its backup is used only when it
// names the same file (same md5 file name) and the primary does not answer.
domain::Result<std::shared_ptr<ports::IAssetResolver>> make_resolver(const ports::ProviderSetup& setup);

} // namespace music_service::providers::fivesing
