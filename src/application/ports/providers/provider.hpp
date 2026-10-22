#pragma once

#include "application/ports/processing/processing_plan.hpp"
#include "application/ports/runtime/operation_context.hpp"
#include "application/ports/storage/secret_store.hpp"
#include "domain/assets/quality.hpp"
#include "domain/catalog/collections.hpp"
#include "domain/catalog/track.hpp"
#include "domain/identity/account.hpp"
#include "domain/result.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace music_service::application::ports {

// Capability keys are an open set of strings; these are the ones the API defines. A
// key is only usable once a port interface below is tied to it with
// capability_traits; the registry rejects bundles that declare any other key.
namespace capability {
inline constexpr std::string_view search_tracks = "search_tracks";
inline constexpr std::string_view track_detail = "track_detail";
inline constexpr std::string_view parse_links = "parse_links";
inline constexpr std::string_view resolve_audio = "resolve_audio";
inline constexpr std::string_view lyrics = "lyrics";
inline constexpr std::string_view search_playlists = "search_playlists";
inline constexpr std::string_view playlist_detail = "playlist_detail";
inline constexpr std::string_view playlist_tracks = "playlist_tracks";
inline constexpr std::string_view search_albums = "search_albums";
inline constexpr std::string_view album_detail = "album_detail";
inline constexpr std::string_view album_tracks = "album_tracks";
inline constexpr std::string_view search_artists = "search_artists";
inline constexpr std::string_view artist_tracks = "artist_tracks";
inline constexpr std::string_view artist_albums = "artist_albums";
inline constexpr std::string_view categories = "categories";
inline constexpr std::string_view category_playlists = "category_playlists";
inline constexpr std::string_view charts = "charts";
inline constexpr std::string_view chart_detail = "chart_detail";
inline constexpr std::string_view chart_tracks = "chart_tracks";
inline constexpr std::string_view recommend_playlists = "recommend_playlists";
inline constexpr std::string_view verify_credentials = "verify_credentials";
inline constexpr std::string_view user_playlists = "user_playlists";
inline constexpr std::string_view podcast_detail = "podcast_detail";
inline constexpr std::string_view podcast_episodes = "podcast_episodes";
inline constexpr std::string_view episode_detail = "episode_detail";
} // namespace capability

// Maps a capability port interface to its key. Specialized next to each interface.
template <class Interface> struct capability_traits;

// Per-operation context for one platform call. Holds the identity snapshot but no
// network quota; every request inside still acquires its own lease.
struct ProviderContext {
    OperationContext operation;
    domain::AuthIdentityKey identity;
    std::optional<std::string> region;
    // The leased credential secret for user bindings; empty for anonymous calls.
    std::shared_ptr<const SecretBytes> secret{};
};

struct TrackSearchQuery {
    std::string text;
    std::uint32_t limit{20};
    std::optional<std::string> cursor; // provider-private paging state
};

// One page of a platform list or search, with the platform's own continuation.
template <class Item> struct CatalogPage {
    domain::SourcePage<Item> page;
    std::optional<std::string> next_cursor; // provider-private paging state
};
using TrackSearchPage = CatalogPage<domain::Track>;
using SearchQuery = TrackSearchQuery;

// A page of a list that belongs to one entity (a playlist's tracks, ...).
struct ListQuery {
    std::uint32_t limit{50};
    std::optional<std::string> cursor; // provider-private paging state
};

class ITrackSearch {
public:
    virtual ~ITrackSearch() = default;
    virtual domain::Result<TrackSearchPage> search_tracks(const ProviderContext& context,
                                                          const TrackSearchQuery& query) = 0;
};
template <> struct capability_traits<ITrackSearch> {
    static constexpr std::string_view key = capability::search_tracks;
};

class ITrackDetail {
public:
    virtual ~ITrackDetail() = default;
    virtual domain::Result<domain::Track> track_detail(const ProviderContext& context,
                                                       const domain::TrackRef& track) = 0;
};
template <> struct capability_traits<ITrackDetail> {
    static constexpr std::string_view key = capability::track_detail;
};

// A recognized link or URI: track, playlist, album, artist, podcast or episode.
struct ParsedLink {
    std::string kind;
    domain::PlatformRef ref;
};

class ILinkParser {
public:
    virtual ~ILinkParser() = default;
    // Recognizes only the domains and URI forms the platform declares; anything else is
    // VALIDATION_FAILED. Parsing never needs the network.
    virtual domain::Result<ParsedLink> parse_link(std::string_view link) = 0;
};
template <> struct capability_traits<ILinkParser> {
    static constexpr std::string_view key = capability::parse_links;
};

// Internal transfer plan for audio; contains a URL and secret headers and must never be
// returned to clients.
struct ResolvedAudio {
    domain::TrackRef actual_track;
    std::optional<domain::Quality> actual_quality;
    domain::Completeness completeness{domain::Completeness::unknown};
    std::optional<std::string> container;
    std::optional<std::int64_t> bitrate_bps;
    std::string url;
    std::vector<std::pair<std::string, std::string>> request_headers;
    std::optional<std::chrono::system_clock::time_point> expires_at;
    std::optional<std::int64_t> duration_ms; // length the platform declares for this resource
    ProcessingPlan processing{};             // required decryption and validation
    // The platform allows handing url to clients as is (direct media URLs). Never set for
    // addresses that need request_headers, decryption or account secrets in the URL.
    bool shareable{false};
};

class IAssetResolver {
public:
    virtual ~IAssetResolver() = default;
    virtual domain::Result<ResolvedAudio>
    resolve_audio(const ProviderContext& context, const domain::TrackRef& track,
                  const domain::ResourceRequirement& requirement) = 0;
};
template <> struct capability_traits<IAssetResolver> {
    static constexpr std::string_view key = capability::resolve_audio;
};

// A track without lyrics is a TrackLyrics with unavailable set, not an error.
class ILyrics {
public:
    virtual ~ILyrics() = default;
    virtual domain::Result<domain::TrackLyrics> lyrics(const ProviderContext& context,
                                                       const domain::TrackRef& track) = 0;
};
template <> struct capability_traits<ILyrics> {
    static constexpr std::string_view key = capability::lyrics;
};

// The capability families below share three shapes. Each key has its own tag type so
// that every key binds a distinct port; the tag names the item and the key.
namespace catalog_tag {
struct PodcastDetail {
    using Item = domain::Podcast;
    static constexpr std::string_view key = capability::podcast_detail;
};
struct EpisodeDetail {
    using Item = domain::Episode;
    static constexpr std::string_view key = capability::episode_detail;
};
struct PodcastEpisodes {
    using Item = domain::Episode;
    static constexpr std::string_view key = capability::podcast_episodes;
};
struct SearchPlaylists {
    using Item = domain::Playlist;
    static constexpr std::string_view key = capability::search_playlists;
};
struct SearchAlbums {
    using Item = domain::Album;
    static constexpr std::string_view key = capability::search_albums;
};
struct SearchArtists {
    using Item = domain::Artist;
    static constexpr std::string_view key = capability::search_artists;
};
struct PlaylistDetail {
    using Item = domain::Playlist;
    static constexpr std::string_view key = capability::playlist_detail;
};
struct AlbumDetail {
    using Item = domain::Album;
    static constexpr std::string_view key = capability::album_detail;
};
struct ChartDetail {
    using Item = domain::Chart;
    static constexpr std::string_view key = capability::chart_detail;
};
struct PlaylistTracks {
    using Item = domain::Track;
    static constexpr std::string_view key = capability::playlist_tracks;
};
struct AlbumTracks {
    using Item = domain::Track;
    static constexpr std::string_view key = capability::album_tracks;
};
struct ArtistTracks {
    using Item = domain::Track;
    static constexpr std::string_view key = capability::artist_tracks;
};
struct ChartTracks {
    using Item = domain::Track;
    static constexpr std::string_view key = capability::chart_tracks;
};
struct ArtistAlbums {
    using Item = domain::Album;
    static constexpr std::string_view key = capability::artist_albums;
};
struct CategoryPlaylists {
    using Item = domain::Playlist;
    static constexpr std::string_view key = capability::category_playlists;
};
} // namespace catalog_tag

// Search for playlists, albums or artists; paged like search_tracks.
template <class Tag> class ICatalogSearch {
public:
    virtual ~ICatalogSearch() = default;
    virtual domain::Result<CatalogPage<typename Tag::Item>> search(const ProviderContext& context,
                                                                   const SearchQuery& query) = 0;
};
template <class Tag> struct capability_traits<ICatalogSearch<Tag>> {
    static constexpr std::string_view key = Tag::key;
};

// The details of one collection. NOT_FOUND when the platform has no such entity.
template <class Tag> class IEntityDetail {
public:
    virtual ~IEntityDetail() = default;
    virtual domain::Result<typename Tag::Item> detail(const ProviderContext& context,
                                                      const domain::PlatformRef& entity) = 0;
};
template <class Tag> struct capability_traits<IEntityDetail<Tag>> {
    static constexpr std::string_view key = Tag::key;
};

// One page of what an entity holds: a playlist's, album's, artist's or chart's tracks,
// an artist's albums, a category's playlists (the category id as the entity id). A page
// is fetched only when asked for; nothing walks a whole list in the background.
template <class Tag> class IEntityList {
public:
    virtual ~IEntityList() = default;
    virtual domain::Result<CatalogPage<typename Tag::Item>> list(const ProviderContext& context,
                                                                 const domain::PlatformRef& entity,
                                                                 const ListQuery& query) = 0;
};
template <class Tag> struct capability_traits<IEntityList<Tag>> {
    static constexpr std::string_view key = Tag::key;
};

class ICategories {
public:
    virtual ~ICategories() = default;
    virtual domain::Result<std::vector<domain::Category>> categories(const ProviderContext& context) = 0;
};
template <> struct capability_traits<ICategories> {
    static constexpr std::string_view key = capability::categories;
};

class ICharts {
public:
    virtual ~ICharts() = default;
    virtual domain::Result<std::vector<domain::Chart>> charts(const ProviderContext& context) = 0;
};
template <> struct capability_traits<ICharts> {
    static constexpr std::string_view key = capability::charts;
};

class IRecommendedPlaylists {
public:
    virtual ~IRecommendedPlaylists() = default;
    virtual domain::Result<CatalogPage<domain::Playlist>> recommend_playlists(const ProviderContext& context,
                                                                              const ListQuery& query) = 0;
};
template <> struct capability_traits<IRecommendedPlaylists> {
    static constexpr std::string_view key = capability::recommend_playlists;
};

using PlaylistSearch = ICatalogSearch<catalog_tag::SearchPlaylists>;
using AlbumSearch = ICatalogSearch<catalog_tag::SearchAlbums>;
using ArtistSearch = ICatalogSearch<catalog_tag::SearchArtists>;
using PlaylistDetail = IEntityDetail<catalog_tag::PlaylistDetail>;
using AlbumDetail = IEntityDetail<catalog_tag::AlbumDetail>;
using ChartDetail = IEntityDetail<catalog_tag::ChartDetail>;
using PlaylistTracks = IEntityList<catalog_tag::PlaylistTracks>;
using AlbumTracks = IEntityList<catalog_tag::AlbumTracks>;
using ArtistTracks = IEntityList<catalog_tag::ArtistTracks>;
using ChartTracks = IEntityList<catalog_tag::ChartTracks>;
using ArtistAlbums = IEntityList<catalog_tag::ArtistAlbums>;
using CategoryPlaylists = IEntityList<catalog_tag::CategoryPlaylists>;
using PodcastDetail = IEntityDetail<catalog_tag::PodcastDetail>;
using EpisodeDetail = IEntityDetail<catalog_tag::EpisodeDetail>;
using PodcastEpisodes = IEntityList<catalog_tag::PodcastEpisodes>;

class IUserPlaylists {
public:
    virtual ~IUserPlaylists() = default;
    virtual domain::Result<CatalogPage<domain::Playlist>> user_playlists(const ProviderContext&, const ListQuery&) = 0;
};
template <> struct capability_traits<IUserPlaylists> {
    static constexpr std::string_view key = capability::user_playlists;
};

} // namespace music_service::application::ports
