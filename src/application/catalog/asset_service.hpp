#pragma once

#include "application/accounts/credential_service.hpp"
#include "application/catalog/handle_table.hpp"
#include "application/catalog/source_retry.hpp"
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
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace music_service::application::catalog {

struct ResolveRequest {
    std::string owner_id;
    domain::TrackRef track;
    domain::ResourceRequirement requirement;
    bool switch_source{false};
    std::vector<std::string> attempted_sources{};
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
    std::vector<std::string> attempted_sources{};
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

// One request for an asset's audio through the proxy (api-contract.md 10.4, 10.5).
struct StreamRequest {
    std::string owner_id;
    std::string asset_id;
    // One validated byte range to forward as is ("bytes=0-99", "bytes=100-", "bytes=-50");
    // empty for the whole resource.
    std::optional<std::string> range;
    // The client's If-Range, forwarded with the range: a stale validator makes the
    // upstream answer the whole resource.
    std::optional<std::string> if_range;
    std::chrono::steady_clock::time_point deadline;
    std::stop_token stop;
};

// What the upstream answered, as the media entry may forward it: never its URL or headers
// beyond these. status is 200, 206 or 416.
struct StreamHead {
    int status{200};
    std::optional<std::uint64_t> content_length; // of this body
    std::optional<std::uint64_t> total_bytes;    // of the whole resource, when known
    std::optional<std::pair<std::uint64_t, std::uint64_t>> range; // 206: first and last byte
    std::optional<std::string> content_type;     // the upstream's, when it names audio
    std::optional<std::string> etag;             // the upstream's strong validator, if any
    std::optional<std::string> container;        // declared by the platform
    bool accept_ranges{false};
};

// What makes two upstream answers the same bytes: total size and validators.
struct ResourceEvidence {
    std::optional<std::uint64_t> total_bytes;
    std::optional<std::string> etag;
    std::optional<std::string> last_modified;
    bool operator==(const ResourceEvidence&) const = default;
};

// What a media access description needs to know about an asset, without its URL.
struct DeliveryFacts {
    AssetDescriptor descriptor;
    bool shareable{false};        // the platform allows handing the address to clients
    bool needs_secrets{false};    // request headers or an account secret are required
    bool needs_processing{false}; // decryption is declared
};

// Same-source resolve and bounded probe (feature-modules.md, 音源解析与探测). An
// asset_id is bound to its owner and to the authorization identity that produced
// it; it lives lifetimes.asset seconds, never past a known upstream expiry minus a
// safety margin. 普通 resolve 使用指定平台；显式 switch_source 使用严格匹配、
// 排除列表及下载成功率排序，每个新候选还需通过有界音频头探测。
class AssetService {
public:
    static constexpr std::chrono::seconds expiry_margin{30};
    static constexpr std::size_t probe_bytes = 16;

    AssetService(config::Lifetimes lifetimes, config::Handles handles, std::size_t per_owner_share,
                 std::shared_ptr<const ports::ProviderDirectory> providers,
                 std::shared_ptr<accounts::CredentialService> credentials, std::shared_ptr<ports::UpstreamHttp> upstream,
                 std::shared_ptr<const ports::Clock> clock, std::function<std::string()> new_asset_id,
                 std::chrono::milliseconds budget, std::shared_ptr<ports::MemoryBudget> handle_budget = {},
                 std::shared_ptr<SourceRetryService> source_retry = {});

    domain::Result<AssetDescriptor> resolve(const ResolveRequest& request);
    domain::Result<std::vector<ports::SourceDownloadStatistics>> source_statistics(const std::string& owner) {
        return source_retry_->statistics(owner);
    }
    void collect_expired();

    // RESOURCE_EXPIRED when the asset expired or its account changed since; NOT_FOUND
    // for unknown or foreign ids. Reads at most probe_bytes of the audio.
    domain::Result<ProbeReport> probe(const std::string& owner_id, const std::string& asset_id);

    // The caller's asset while it is valid: NOT_FOUND for unknown or foreign ids,
    // RESOURCE_EXPIRED once it expired. Touches no account and no network.
    domain::Result<AssetDescriptor> describe(const std::string& owner_id, const std::string& asset_id);

    // describe() plus what decides between direct and proxy delivery.
    domain::Result<DeliveryFacts> delivery(const std::string& owner_id, const std::string& asset_id);

    // The upstream address, for a direct media URL; only for a shareable asset without
    // secrets (DELIVERY_UNAVAILABLE otherwise). Never logged by the caller.
    domain::Result<std::string> direct_url(const std::string& owner_id, const std::string& asset_id);

    // Streams the asset's audio for the media proxy. Blocking: on_head is called at most
    // once, before any chunk, with the answer to forward (200, 206 or 416); then on_chunk
    // until the body ends, a consumer returns false or the stop token fires. Errors before
    // on_head are returned for the caller to answer with; after it the result only says
    // how the transfer ended. The first answer fixes the resource's size and validators;
    // a later answer that differs (also after refreshing an expired address) ends the
    // asset with RESOURCE_EXPIRED instead of mixing two resources. An address the
    // upstream refuses (403/404/410) is resolved again once for the same actual track and
    // quality; anything else is RESOURCE_EXPIRED.
    domain::Result<void> stream(const StreamRequest& request,
                                const std::function<bool(const StreamHead&)>& on_head,
                                const std::function<bool(std::span<const char>)>& on_chunk);

private:
    struct Asset;

    domain::Result<AssetDescriptor> resolve_one(const ResolveRequest& request, const domain::TrackRef& actual,
        const domain::AccountBinding& binding, ports::OperationContext operation, const std::vector<std::string>& attempted);
    domain::Result<ProbeReport> probe_until(const std::string& owner, const std::string& asset,
                                          ports::OperationContext operation);

    domain::Result<void> refresh(const std::string& owner_id, const std::string& asset_id);

    std::shared_ptr<const ports::ProviderDirectory> providers_;
    std::shared_ptr<accounts::CredentialService> credentials_;
    std::shared_ptr<ports::UpstreamHttp> upstream_;
    std::shared_ptr<const ports::Clock> clock_;
    config::Lifetimes lifetimes_;
    std::chrono::milliseconds budget_;
    std::shared_ptr<HandleTable<Asset>> assets_;
    std::shared_ptr<SourceRetryService> source_retry_;
};

} // namespace music_service::application::catalog
