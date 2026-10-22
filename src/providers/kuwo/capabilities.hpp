#pragma once

#include "application/ports/providers/bundle.hpp"

#include <memory>

namespace music_service::providers::kuwo {

namespace ports = application::ports;

// Kuwo capability objects. Each request goes through ProviderSetup::upstream, so
// admission, TLS and redirect rules are the shared ones; a hop that cannot be
// admitted is SERVICE_BUSY (retryable, details {"admission", dimension}) for the
// caller to re-plan. Anonymous: no cookie is ever sent.

domain::Result<std::shared_ptr<ports::ITrackSearch>> make_search(const ports::ProviderSetup& setup);
domain::Result<std::shared_ptr<ports::ITrackDetail>> make_detail(const ports::ProviderSetup& setup);
domain::Result<std::shared_ptr<ports::ILinkParser>> make_link_parser(const ports::ProviderSetup& setup);
domain::Result<std::shared_ptr<ports::ILyrics>> make_lyrics(const ports::ProviderSetup& setup);
// standard／high MP3 与 lossless FLAC；替代 rid 的片段始终报告 preview。
domain::Result<std::shared_ptr<ports::IAssetResolver>> make_resolver(const ports::ProviderSetup& setup);

} // namespace music_service::providers::kuwo
