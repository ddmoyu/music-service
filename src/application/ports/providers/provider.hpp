#pragma once

#include "application/ports/runtime/operation_context.hpp"
#include "application/ports/storage/secret_store.hpp"
#include "domain/assets/quality.hpp"
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
inline constexpr std::string_view verify_credentials = "verify_credentials";
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

struct TrackSearchPage {
    domain::SourcePage<domain::Track> page;
    std::optional<std::string> next_cursor;
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

// A recognized link or URI. kind is "track", "playlist", "album" or "artist".
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

struct Lyrics {
    std::optional<std::string> plain;
    std::optional<std::string> synced_lrc;
};

class ILyrics {
public:
    virtual ~ILyrics() = default;
    virtual domain::Result<Lyrics> lyrics(const ProviderContext& context,
                                          const domain::TrackRef& track) = 0;
};
template <> struct capability_traits<ILyrics> {
    static constexpr std::string_view key = capability::lyrics;
};

} // namespace music_service::application::ports
