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
    domain::ResourceRequirement requirement;  // to resolve the same resource again
    bool secret_used{false};                  // the address came from an account secret
    std::optional<ResourceEvidence> evidence; // fixed by the first proxied answer
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
    Asset asset{binding->binding,   lease->identity.canonical(), request.track, std::move(audio).value(),
                descriptor,         request.requirement,         secret != nullptr, std::nullopt};
    auto id = assets_->put(request.owner_id, std::move(asset), expires_at, now);
    if (!id) {
        return std::move(id).error();
    }
    descriptor.asset_id = std::move(id).value();
    return descriptor;
}

domain::Result<AssetDescriptor> AssetService::describe(const std::string& owner_id, const std::string& asset_id) {
    auto found = assets_->get(owner_id, asset_id, clock_->now());
    if (!found) {
        return std::move(found).error();
    }
    auto descriptor = found->first.descriptor;
    descriptor.asset_id = asset_id;
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

namespace {

bool strong(const std::optional<std::string>& etag) { return etag && etag->rfind("W/", 0) != 0; }

// "bytes 100-199/2000" -> {100, 199}; "bytes */2000" or malformed -> nullopt.
std::optional<std::pair<std::uint64_t, std::uint64_t>> content_range_span(std::string_view value) {
    const auto dash = value.find('-');
    const auto slash = value.rfind('/');
    if (value.substr(0, 6) != "bytes " || dash == std::string_view::npos || slash == std::string_view::npos ||
        dash > slash) {
        return std::nullopt;
    }
    const auto first = parse_int(value.substr(6, dash - 6));
    const auto last = parse_int(value.substr(dash + 1, slash - dash - 1));
    if (!first || !last || *last < *first) {
        return std::nullopt;
    }
    return std::pair{static_cast<std::uint64_t>(*first), static_cast<std::uint64_t>(*last)};
}

std::optional<std::uint64_t> unsigned_of(std::optional<std::int64_t> value) {
    return value ? std::optional<std::uint64_t>{static_cast<std::uint64_t>(*value)} : std::nullopt;
}

} // namespace

domain::Result<DeliveryFacts> AssetService::delivery(const std::string& owner_id, const std::string& asset_id) {
    auto found = assets_->get(owner_id, asset_id, clock_->now());
    if (!found) {
        return std::move(found).error();
    }
    const auto& asset = found->first;
    DeliveryFacts facts{asset.descriptor, asset.audio.shareable,
                        asset.secret_used || !asset.audio.request_headers.empty(),
                        asset.audio.processing.decryption.has_value()};
    facts.descriptor.asset_id = asset_id;
    return facts;
}

domain::Result<std::string> AssetService::direct_url(const std::string& owner_id, const std::string& asset_id) {
    auto facts = delivery(owner_id, asset_id);
    if (!facts) {
        return std::move(facts).error();
    }
    if (!facts->shareable || facts->needs_secrets || facts->needs_processing) {
        return Error{ErrorCode::delivery_unavailable,
                     "this audio address cannot be handed to clients",
                     false,
                     {{"mode", "direct needs a shareable address without secrets or decryption"}}};
    }
    auto found = assets_->get(owner_id, asset_id, clock_->now());
    if (!found) {
        return std::move(found).error();
    }
    return found->first.audio.url;
}

domain::Result<void> AssetService::stream(const StreamRequest& request,
                                          const std::function<bool(const StreamHead&)>& on_head,
                                          const std::function<bool(std::span<const char>)>& on_chunk) {
    const auto& owner = request.owner_id;
    for (int round = 0; round < 2; ++round) {
        auto found = assets_->get(owner, request.asset_id, clock_->now());
        if (!found) {
            return std::move(found).error();
        }
        const auto asset = std::move(found->first);
        // The account that produced the address must still be the one leasable now.
        auto lease = credentials_->lease(asset.binding);
        if (!lease || lease->identity.canonical() != asset.identity) {
            return expired("the account behind this asset changed; resolve again");
        }

        ports::HttpRequest upstream_request;
        upstream_request.url = asset.audio.url;
        upstream_request.operation = "media_proxy";
        for (const auto& [name, value] : asset.audio.request_headers) {
            (application::transport::is_sensitive_header(name) ? upstream_request.secret_headers
                                                               : upstream_request.headers)
                .push_back({name, value});
        }
        if (request.range) {
            upstream_request.headers.push_back({"Range", *request.range});
            if (request.if_range) {
                upstream_request.headers.push_back({"If-Range", *request.if_range});
            }
        }
        const auto& actual = asset.audio.actual_track;
        ports::AdmissionRequest base{owner,           actual.source, "media_proxy", ports::TrafficClass::media,
                                     lease->identity, {},            {},            request.deadline};

        bool refused = false; // the address no longer works: resolve again
        bool changed = false; // other bytes than the asset was fixed to
        bool forwarded = false;
        std::optional<Error> early;
        auto result = upstream_->stream(
            std::move(upstream_request), base, {request.deadline, request.stop},
            [&](const ports::HttpResponseHead& h) {
                if (h.status == 403 || h.status == 404 || h.status == 410) {
                    refused = true;
                    return false;
                }
                StreamHead head;
                head.status = h.status;
                head.container = asset.audio.container;
                if (const auto type = h.header("Content-Type"); type && type->rfind("audio/", 0) == 0) {
                    head.content_type = type;
                }
                if (h.status == 416) {
                    if (const auto range = h.header("Content-Range")) {
                        head.total_bytes = unsigned_of(content_range_total(*range));
                    }
                    forwarded = true;
                    (void)on_head(head);
                    return false;
                }
                if (h.status != 200 && h.status != 206) {
                    Error error{ErrorCode::upstream_failed, "the audio server did not answer with audio", true, {}};
                    error.details.push_back({"http_status", std::to_string(h.status)});
                    early = std::move(error);
                    return false;
                }
                if (const auto length = h.header("Content-Length")) {
                    head.content_length = unsigned_of(parse_int(*length));
                }
                if (h.status == 206) {
                    if (const auto range = h.header("Content-Range")) {
                        head.range = content_range_span(*range);
                        head.total_bytes = unsigned_of(content_range_total(*range));
                    }
                    if (!head.range) {
                        early = Error{ErrorCode::upstream_failed, "the audio server sent an unusable range", true, {}};
                        return false;
                    }
                } else {
                    head.total_bytes = head.content_length;
                }
                head.accept_ranges =
                    h.status == 206 || h.header("Accept-Ranges") == std::optional<std::string>{"bytes"};
                const auto etag = h.header("ETag");
                if (strong(etag)) {
                    head.etag = etag;
                }
                const ResourceEvidence evidence{head.total_bytes, head.etag, h.header("Last-Modified")};
                // The first answer fixes the resource; any later one must be the same bytes.
                auto fixed = assets_->update(owner, request.asset_id, clock_->now(), [&](Asset& a) {
                    if (!a.evidence) {
                        a.evidence = evidence;
                    }
                });
                if (!fixed) {
                    early = std::move(fixed).error();
                    return false;
                }
                const auto& kept = *fixed->evidence;
                const auto differs = [](const auto& a, const auto& b) { return a && b && *a != *b; };
                if (differs(kept.total_bytes, evidence.total_bytes) || differs(kept.etag, evidence.etag) ||
                    differs(kept.last_modified, evidence.last_modified)) {
                    changed = true;
                    return false;
                }
                forwarded = true;
                return on_head(head);
            },
            [&](std::span<const char> chunk) { return on_chunk(chunk); });

        if (changed) {
            assets_->expire(owner, request.asset_id);
            return expired("the audio behind this asset changed; resolve again");
        }
        if (!result) {
            return std::move(result).error();
        }
        if (auto* pending = std::get_if<ports::PendingHop>(&result.value())) {
            Error error{ErrorCode::service_busy, "upstream capacity is not available now", true, {}};
            error.details.push_back({"admission", pending->deferred.blocked_dimension});
            return error;
        }
        if (refused && round == 0) {
            if (auto renewed = refresh(owner, request.asset_id); !renewed) {
                return renewed;
            }
            continue;
        }
        if (refused) {
            assets_->expire(owner, request.asset_id);
            return expired("the audio address is no longer valid; resolve again");
        }
        if (early) {
            return *early;
        }
        (void)forwarded;
        return domain::ok();
    }
    return expired("the audio address is no longer valid; resolve again");
}

// A new address for the same actual track, quality and container; the asset keeps its id,
// expiry and fixed evidence. Anything else ends the asset.
domain::Result<void> AssetService::refresh(const std::string& owner_id, const std::string& asset_id) {
    auto found = assets_->get(owner_id, asset_id, clock_->now());
    if (!found) {
        return std::move(found).error();
    }
    const auto asset = std::move(found->first);
    const auto end = [&](std::string message) -> domain::Result<void> {
        assets_->expire(owner_id, asset_id);
        return expired(std::move(message));
    };
    auto lease = credentials_->lease(asset.binding);
    if (!lease || lease->identity.canonical() != asset.identity) {
        return end("the account behind this asset changed; resolve again");
    }
    auto resolver = providers_->asset_resolver(asset.audio.actual_track.source.value());
    if (!resolver) {
        return end("the source can no longer resolve this audio");
    }
    std::shared_ptr<const ports::SecretBytes> secret;
    if (lease->secret) {
        secret = std::make_shared<const ports::SecretBytes>(std::move(*lease->secret));
    }
    const ports::ProviderContext context{{SteadyClock::now() + budget_, {}}, lease->identity, std::nullopt, secret};
    auto audio = resolver.value()->resolve_audio(context, asset.audio.actual_track, asset.requirement);
    if (!audio) {
        if (audio.error().retryable) {
            return audio.error();
        }
        return end("the audio can no longer be resolved");
    }
    if (audio->actual_track != asset.audio.actual_track || audio->actual_quality != asset.audio.actual_quality ||
        audio->container != asset.audio.container) {
        return end("the platform now returns another recording or quality; resolve again");
    }
    auto updated = assets_->update(owner_id, asset_id, clock_->now(), [&](Asset& a) {
        a.audio.url = audio->url;
        a.audio.request_headers = audio->request_headers;
        a.audio.shareable = audio->shareable;
    });
    if (!updated) {
        return std::move(updated).error();
    }
    return domain::ok();
}

} // namespace music_service::application::catalog
