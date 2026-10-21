#include "application/catalog/asset_service.hpp"

#include "application/transport/http_rules.hpp"
#include "domain/assets/container.hpp"

#include <algorithm>
#include <charconv>
#include <utility>

namespace music_service::application::catalog {

namespace {

using domain::Error;
using domain::ErrorCode;
using SteadyClock = std::chrono::steady_clock;

Error expired(std::string message) { return Error{ErrorCode::resource_expired, std::move(message), false, {}}; }

std::optional<std::int64_t> parse_int(std::string_view text) {
    std::int64_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size() || value < 0) {
        return std::nullopt;
    }
    return value;
}

// "bytes 0-15/2829338" -> 2829338; "*" or malformed -> nullopt.
std::optional<std::int64_t> content_range_total(std::string_view value) {
    const auto slash = value.rfind('/');
    if (value.substr(0, 6) != "bytes " || slash == std::string_view::npos) {
        return std::nullopt;
    }
    return parse_int(value.substr(slash + 1));
}

// Container from the first bytes; the URL suffix is never evidence.
std::optional<std::string> sniff(std::string_view head) {
    const auto sniffed = domain::sniff_content(head);
    if (!sniffed.container) {
        return std::nullopt;
    }
    return std::string{domain::to_string(*sniffed.container)};
}

} // namespace

struct AssetService::Asset {
    domain::AccountBinding binding;
    std::string identity; // AuthIdentityKey::canonical() at resolve time
    domain::TrackRef original_track;
    ports::ResolvedAudio audio;
    AssetDescriptor descriptor;
};

AssetService::AssetService(config::Lifetimes lifetimes, config::Handles handles, std::size_t per_owner_share,
                           std::shared_ptr<const ports::ProviderDirectory> providers,
                           std::shared_ptr<accounts::CredentialService> credentials,
                           std::shared_ptr<ports::UpstreamHttp> upstream, std::shared_ptr<const ports::Clock> clock,
                           std::function<std::string()> new_asset_id, std::chrono::milliseconds budget)
    : providers_(std::move(providers)), credentials_(std::move(credentials)), upstream_(std::move(upstream)),
      clock_(std::move(clock)), lifetimes_(lifetimes), budget_(budget),
      assets_(std::make_shared<HandleTable<Asset>>(
          handles.assets, std::max<std::size_t>(1, handles.assets / std::max<std::size_t>(1, per_owner_share)),
          std::move(new_asset_id))) {}

domain::Result<AssetDescriptor> AssetService::resolve(const ResolveRequest& request) {
    if (request.match_selection) {
        return Error{ErrorCode::capability_unsupported, "match selection is not available yet", false, {}};
    }
    if (request.automatic_fallback) {
        return Error{ErrorCode::capability_unsupported, "automatic fallback is not enabled", false, {}};
    }
    const auto& source = request.track.source.value();
    if (auto known = providers_->check_source(source); !known) {
        return known.error();
    }
    auto binding = credentials_->resolve_binding(request.owner_id, source, "resolve_audio", std::nullopt);
    if (!binding) {
        return std::move(binding).error();
    }
    auto lease = credentials_->lease(binding->binding);
    if (!lease) {
        return std::move(lease).error();
    }
    auto resolver = providers_->asset_resolver(source);
    if (!resolver) {
        return std::move(resolver).error();
    }
    std::shared_ptr<const ports::SecretBytes> secret;
    if (lease->secret) {
        secret = std::make_shared<const ports::SecretBytes>(std::move(*lease->secret));
    }
    const ports::ProviderContext context{{SteadyClock::now() + budget_, {}}, lease->identity, std::nullopt, secret};
    auto audio = resolver.value()->resolve_audio(context, request.track, request.requirement);
    if (!audio) {
        return std::move(audio).error();
    }
    auto verdict = domain::evaluate_resource(request.requirement, {audio->actual_quality, audio->completeness});
    if (!verdict) {
        return std::move(verdict).error();
    }

    const auto now = clock_->now();
    auto expires_at = now + lifetimes_.asset;
    if (audio->expires_at) {
        expires_at = std::min(expires_at, *audio->expires_at - expiry_margin);
    }
    if (expires_at <= now) {
        // An address that is already (nearly) stale is not a usable result.
        Error error{ErrorCode::upstream_failed, "the platform returned an expired audio address", true, {}};
        error.details.push_back({"asset", "expired on arrival"});
        return error;
    }

    AssetDescriptor descriptor{{},
                               request.track,
                               audio->actual_track,
                               request.requirement.requested,
                               audio->actual_quality,
                               audio->container,
                               audio->bitrate_bps,
                               audio->completeness,
                               expires_at,
                               verdict->warnings};
    Asset asset{binding->binding, lease->identity.canonical(), request.track, std::move(audio).value(), descriptor};
    auto id = assets_->put(request.owner_id, std::move(asset), expires_at, now);
    if (!id) {
        return std::move(id).error();
    }
    descriptor.asset_id = std::move(id).value();
    return descriptor;
}

domain::Result<ProbeReport> AssetService::probe(const std::string& owner_id, const std::string& asset_id) {
    auto found = assets_->get(owner_id, asset_id, clock_->now());
    if (!found) {
        return std::move(found).error();
    }
    const auto& asset = found->first;
    // The account that produced the address must still be the one leasable now.
    auto lease = credentials_->lease(asset.binding);
    if (!lease || lease->identity.canonical() != asset.identity) {
        return expired("the account behind this asset changed; resolve again");
    }

    ports::HttpRequest request;
    request.url = asset.audio.url;
    request.operation = "probe";
    for (const auto& [name, value] : asset.audio.request_headers) {
        (application::transport::is_sensitive_header(name) ? request.secret_headers : request.headers)
            .push_back({name, value});
    }
    request.headers.push_back({"Range", "bytes=0-" + std::to_string(probe_bytes - 1)});

    const auto& actual = asset.audio.actual_track;
    ports::AdmissionRequest base{owner_id,       actual.source, "probe", ports::TrafficClass::short_request,
                                 lease->identity, {},            {},      SteadyClock::now() + budget_};
    ports::HttpResponseHead head;
    std::string leading;
    auto result = upstream_->stream(
        std::move(request), base, {SteadyClock::now() + budget_, {}},
        [&](const ports::HttpResponseHead& h) {
            head = h;
            return h.status == 200 || h.status == 206;
        },
        [&](std::span<const char> chunk) {
            leading.append(chunk.data(), std::min(chunk.size(), probe_bytes - leading.size()));
            return leading.size() < probe_bytes; // never read the whole song
        });
    if (!result) {
        return std::move(result).error();
    }
    if (auto* pending = std::get_if<ports::PendingHop>(&result.value())) {
        Error error{ErrorCode::service_busy, "upstream capacity is not available now", true, {}};
        error.details.push_back({"admission", pending->deferred.blocked_dimension});
        return error;
    }
    if (head.status == 0) {
        head = std::get<ports::HttpResponseHead>(result.value());
    }

    ProbeReport report;
    report.asset_id = asset_id;
    report.http_status = head.status;
    report.accessible = head.status == 200 || head.status == 206;
    report.content_type = head.header("Content-Type");
    report.range_supported = head.status == 206;
    if (head.status == 206) {
        if (const auto range = head.header("Content-Range")) {
            report.size_bytes = content_range_total(*range);
        }
    } else if (head.status == 200) {
        if (const auto length = head.header("Content-Length")) {
            report.size_bytes = parse_int(*length);
        }
    }
    report.format = sniff(leading);
    if (report.size_bytes && asset.audio.bitrate_bps && *asset.audio.bitrate_bps > 0) {
        report.estimated_duration_ms = *report.size_bytes * 8000 / *asset.audio.bitrate_bps;
    }
    report.completeness = asset.descriptor.completeness;
    report.warnings = asset.descriptor.warnings;
    if (report.accessible && report.format && asset.audio.container && *report.format != *asset.audio.container) {
        // The bytes contradict the declared container: no longer a verified resource.
        report.warnings.push_back(domain::ResourceWarning::quality_unverified);
    }
    return report;
}

} // namespace music_service::application::catalog
