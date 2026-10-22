#pragma once

#include "application/ports/providers/auth.hpp"
#include "application/ports/providers/bundle.hpp"

#include <functional>
#include <memory>
#include <string>

namespace music_service::providers::netease {

namespace ports = application::ports;

domain::Result<std::shared_ptr<ports::ArtistSearch>> make_artist_search(const ports::ProviderSetup& setup);
domain::Result<std::shared_ptr<ports::ArtistAlbums>> make_artist_albums(const ports::ProviderSetup& setup);
domain::Result<std::shared_ptr<ports::IUserPlaylists>> make_user_playlists(const ports::ProviderSetup& setup);
domain::Result<std::shared_ptr<ports::ArtistTracks>> make_artist_tracks(const ports::ProviderSetup& setup);
domain::Result<std::shared_ptr<ports::PodcastDetail>> make_podcast_detail(const ports::ProviderSetup& setup);
domain::Result<std::shared_ptr<ports::PodcastEpisodes>> make_podcast_episodes(const ports::ProviderSetup& setup);
domain::Result<std::shared_ptr<ports::EpisodeDetail>> make_episode_detail(const ports::ProviderSetup& setup);

// The one auth profile: the cookie of a signed-in music.163.com session (MUSIC_U).
inline constexpr std::string_view cookie_profile = "web_cookie";

// Produces the weapi secret key for each request; tests pin it.
using SecretKeySource = std::function<std::string()>;

// NetEase capability objects. Each request is a weapi POST through
// ProviderSetup::upstream, so admission, TLS and redirect rules are the shared ones; a
// hop that cannot be admitted is SERVICE_BUSY (retryable, details {"admission",
// dimension}). Search, details, links and lyrics are anonymous and never send a
// cookie for songs. Personal catalogs require an account; playlists and program
// metadata/audio may use the bound user's cookie. CDN requests never carry it.

domain::Result<std::shared_ptr<ports::ITrackSearch>> make_search(const ports::ProviderSetup& setup);
domain::Result<std::shared_ptr<ports::ITrackDetail>> make_detail(const ports::ProviderSetup& setup);
domain::Result<std::shared_ptr<ports::ILinkParser>> make_link_parser(const ports::ProviderSetup& setup);
domain::Result<std::shared_ptr<ports::ILyrics>> make_lyrics(const ports::ProviderSetup& setup);
// Standard (128 kbit/s MP3), high (exhigh/320 kbit/s MP3) and lossless (FLAC).
// The returned tier is evidence, not the requested tier. Anonymous works for songs the platform lets
// everyone play; songs that need a membership or purchase are CREDENTIALS_REQUIRED
// without a suitable account, and a trial clip is reported as a preview, never as the
// song. No copyright (or no such song) is NOT_FOUND.
domain::Result<std::shared_ptr<ports::IAssetResolver>> make_resolver(const ports::ProviderSetup& setup);
// Checks a cookie against w/nuser/account/get: signed in is valid (the nickname as the
// account hint), nobody is invalid. A cookie without MUSIC_U is invalid without a call.
domain::Result<std::shared_ptr<ports::ICredentialVerifier>> make_verifier(const ports::ProviderSetup& setup);

// For tests: the same objects with a fixed weapi secret key.
void set_secret_key_source_for_testing(SecretKeySource source);

domain::Result<std::shared_ptr<ports::PlaylistSearch>> make_playlist_search(const ports::ProviderSetup& setup);
domain::Result<std::shared_ptr<ports::AlbumSearch>> make_album_search(const ports::ProviderSetup& setup);
domain::Result<std::shared_ptr<ports::PlaylistDetail>> make_playlist_detail(const ports::ProviderSetup& setup);
domain::Result<std::shared_ptr<ports::AlbumDetail>> make_album_detail(const ports::ProviderSetup& setup);
domain::Result<std::shared_ptr<ports::PlaylistTracks>> make_playlist_tracks(const ports::ProviderSetup& setup);
domain::Result<std::shared_ptr<ports::AlbumTracks>> make_album_tracks(const ports::ProviderSetup& setup);
domain::Result<std::shared_ptr<ports::ICategories>> make_categories(const ports::ProviderSetup& setup);
domain::Result<std::shared_ptr<ports::CategoryPlaylists>> make_category_playlists(const ports::ProviderSetup& setup);
domain::Result<std::shared_ptr<ports::IRecommendedPlaylists>> make_recommendations(const ports::ProviderSetup& setup);
domain::Result<std::shared_ptr<ports::ICharts>> make_charts(const ports::ProviderSetup& setup);
domain::Result<std::shared_ptr<ports::ChartDetail>> make_chart_detail(const ports::ProviderSetup& setup);
domain::Result<std::shared_ptr<ports::ChartTracks>> make_chart_tracks(const ports::ProviderSetup& setup);

} // namespace music_service::providers::netease
