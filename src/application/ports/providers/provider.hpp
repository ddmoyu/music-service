#pragma once

#include "application/ports/runtime/operation_context.hpp"
#include "domain/assets/quality.hpp"
#include "domain/catalog/track.hpp"
#include "domain/identity/account.hpp"
#include "domain/result.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace music_service::application::ports {

// Capability keys are an open set of strings; these are the ones the API defines.
namespace capability {
inline constexpr std::string_view search_tracks = "search_tracks";
inline constexpr std::string_view track_detail = "track_detail";
inline constexpr std::string_view parse_links = "parse_links";
inline constexpr std::string_view resolve_audio = "resolve_audio";
inline constexpr std::string_view lyrics = "lyrics";
inline constexpr std::string_view verify_credentials = "verify_credentials";
} // namespace capability

struct ProviderDescriptor {
    domain::SourceId source;
    std::string name;
    std::string implementation_id;
    std::string implementation_version;
    std::set<std::string, std::less<>> capabilities;

    [[nodiscard]] bool supports(std::string_view key) const {
        return capabilities.find(key) != capabilities.end();
    }
};

// Per-operation context for one platform call. Holds the identity snapshot but no
// network quota; every request inside still acquires its own lease.
struct ProviderContext {
    OperationContext operation;
    domain::AuthIdentityKey identity;
    std::optional<std::string> region;
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

} // namespace music_service::application::ports
