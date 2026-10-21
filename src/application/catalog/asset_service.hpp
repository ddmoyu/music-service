#pragma once

#include "application/accounts/credential_service.hpp"
#include "application/catalog/handle_table.hpp"
#include "application/config/service_config.hpp"
#include "application/ports/providers/directory.hpp"
#include "application/ports/runtime/clock.hpp"
#include "application/ports/transport/upstream_http.hpp"
#include "domain/assets/quality.hpp"
#include "domain/result.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace music_service::application::catalog {

struct ResolveRequest {
    std::string owner_id;
    domain::TrackRef track;
    domain::ResourceRequirement requirement;
    bool automatic_fallback{false}; // fallback.mode=auto
    bool match_selection{false};    // a match_selection was given
};

// The public AssetDescriptor: never the upstream URL or request headers.
struct AssetDescriptor {
    std::string asset_id;
    domain::TrackRef original_track;
    domain::TrackRef actual_track;
    domain::Quality requested_quality{domain::Quality::standard};
    std::optional<domain::Quality> actual_quality;
    std::optional<std::string> container;
    std::optional<std::int64_t> bitrate_bps;
    domain::Completeness completeness{domain::Completeness::unknown};
    std::chrono::system_clock::time_point expires_at;
    std::vector<domain::ResourceWarning> warnings;
};

struct ProbeReport {
    std::string asset_id;
    bool accessible{false};
    int http_status{0};
    std::optional<std::string> content_type;
    std::optional<std::int64_t> size_bytes;
    bool range_supported{false};
    std::optional<std::string> format; // from the leading bytes, never from the URL
    std::optional<std::int64_t> estimated_duration_ms; // size and declared bitrate; an estimate
    domain::Completeness completeness{domain::Completeness::unknown};
    std::vector<domain::ResourceWarning> warnings;
};

// Same-source resolve and bounded probe (feature-modules.md, 音源解析与探测). An
// asset_id is bound to its owner and to the authorization identity that produced
// it; it lives lifetimes.asset seconds, never past a known upstream expiry minus a
// safety margin. No automatic fallback: a track whose source cannot resolve audio is
// CAPABILITY_UNSUPPORTED, and matching (T33) is not available yet.
class AssetService {
public:
    static constexpr std::chrono::seconds expiry_margin{30};
    static constexpr std::size_t probe_bytes = 16;

    AssetService(config::Lifetimes lifetimes, config::Handles handles, std::size_t per_owner_share,
                 std::shared_ptr<const ports::ProviderDirectory> providers,
                 std::shared_ptr<accounts::CredentialService> credentials, std::shared_ptr<ports::UpstreamHttp> upstream,
                 std::shared_ptr<const ports::Clock> clock, std::function<std::string()> new_asset_id,
                 std::chrono::milliseconds budget);

    domain::Result<AssetDescriptor> resolve(const ResolveRequest& request);

    // RESOURCE_EXPIRED when the asset expired or its account changed since; NOT_FOUND
    // for unknown or foreign ids. Reads at most probe_bytes of the audio.
    domain::Result<ProbeReport> probe(const std::string& owner_id, const std::string& asset_id);

    // The caller's asset while it is valid: NOT_FOUND for unknown or foreign ids,
    // RESOURCE_EXPIRED once it expired. Touches no account and no network.
    domain::Result<AssetDescriptor> describe(const std::string& owner_id, const std::string& asset_id);

private:
    struct Asset;

    std::shared_ptr<const ports::ProviderDirectory> providers_;
    std::shared_ptr<accounts::CredentialService> credentials_;
    std::shared_ptr<ports::UpstreamHttp> upstream_;
    std::shared_ptr<const ports::Clock> clock_;
    config::Lifetimes lifetimes_;
    std::chrono::milliseconds budget_;
    std::shared_ptr<HandleTable<Asset>> assets_;
};

} // namespace music_service::application::catalog
