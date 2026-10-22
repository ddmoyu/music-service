#include "application/media/media_access.hpp"

#include <algorithm>
#include <utility>

namespace music_service::application::media {

namespace {

using domain::Error;
using domain::ErrorCode;

Error unavailable(std::string message) {
    return Error{ErrorCode::feature_unavailable, std::move(message), false, {}};
}

} // namespace

std::string content_type_of(std::string_view container) {
    if (container == "mp3") {
        return "audio/mpeg";
    }
    if (container == "flac") {
        return "audio/flac";
    }
    if (container == "mp4" || container == "m4a") {
        return "audio/mp4";
    }
    if (container == "ogg") {
        return "audio/ogg";
    }
    if (container == "wav") {
        return "audio/wav";
    }
    if (container == "aac" || container == "adts") {
        return "audio/aac";
    }
    return "application/octet-stream";
}

MediaAccessService::MediaAccessService(MediaFeatures features, std::string media_base_url,
                                       std::chrono::seconds default_ttl, std::shared_ptr<FileAccessService> files,
                                       std::shared_ptr<MediaTicketService> tickets,
                                       std::shared_ptr<catalog::AssetService> assets,
                                       std::shared_ptr<const ports::Clock> clock)
    : features_(features), media_base_url_(std::move(media_base_url)), default_ttl_(default_ttl),
      files_(std::move(files)), tickets_(std::move(tickets)), assets_(std::move(assets)), clock_(std::move(clock)) {}

domain::Result<AccessDescription> MediaAccessService::describe(const AccessRequest& request) {
    if (request.kind == TargetKind::file) {
        return describe_file(request);
    }
    return describe_asset(request);
}

domain::Result<AccessDescription> MediaAccessService::describe_asset(const AccessRequest& request) {
    // The asset must be the caller's and still valid before any feature answer.
    auto facts = assets_->delivery(request.owner_id, request.target_id);
    if (!facts) {
        return std::move(facts).error();
    }
    const auto& asset = facts->descriptor;
    if (facts->needs_processing) {
        return Error{ErrorCode::delivery_unavailable,
                     "this audio must be processed before it can be played",
                     false,
                     {{"asset", "decryption is required; download it instead"}}};
    }
    const bool can_direct = facts->shareable && !facts->needs_secrets;
    if (request.mode == DeliveryMode::direct && !features_.direct_media_urls) {
        return unavailable("direct media URLs are not enabled");
    }
    if (request.mode == DeliveryMode::direct && !can_direct) {
        return Error{ErrorCode::delivery_unavailable,
                     "this audio address cannot be handed to clients",
                     false,
                     {{"mode", "direct needs a shareable address without secrets or decryption"}}};
    }
    const bool direct = request.mode == DeliveryMode::direct ||
                        (request.mode == DeliveryMode::automatic && features_.direct_media_urls && can_direct);
    if (!direct && !features_.media_proxy) {
        return unavailable("the online media proxy is not enabled");
    }

    AccessDescription out;
    out.kind = TargetKind::asset;
    out.target_id = asset.asset_id;
    out.content_type = content_type_of(asset.container.value_or(""));
    out.actual_track = asset.actual_track;
    out.actual_quality = asset.actual_quality;
    const auto requested = clock_->now() + request.ttl.value_or(default_ttl_);
    const auto expires_at = std::min(requested, asset.expires_at);
    if (direct) {
        auto url = assets_->direct_url(request.owner_id, asset.asset_id);
        if (!url) {
            return std::move(url).error();
        }
        out.delivery = "direct";
        out.url = std::move(url).value();
        out.auth = "url";
        out.expires_at = expires_at; // when to ask again; the upstream decides its own
        return out;
    }
    out.delivery = "proxy";
    if (request.auth == MediaAuth::bearer) {
        out.url = media_base_url_ + "/media/v1/assets/" + asset.asset_id;
        out.auth = "bearer";
        out.expires_at = asset.expires_at;
        return out;
    }
    auto ticket = tickets_->issue({request.owner_id, request.token_id, TargetKind::asset, asset.asset_id, expires_at});
    if (!ticket) {
        return std::move(ticket).error();
    }
    out.url = media_base_url_ + "/media/v1/tickets/" + ticket->token;
    out.auth = "url";
    out.expires_at = ticket->expires_at;
    return out;
}

domain::Result<AccessDescription> MediaAccessService::describe_file(const AccessRequest& request) {
    if (!features_.file_access) {
        return unavailable("file access is not enabled");
    }
    if (request.mode == DeliveryMode::direct) {
        return Error{ErrorCode::delivery_unavailable,
                     "files are only served by the backend",
                     false,
                     {{"mode", "direct is not available for files"}}};
    }
    auto asset = files_->describe(request.owner_id, request.target_id);
    if (!asset) {
        return std::move(asset).error();
    }
    AccessDescription out;
    out.kind = TargetKind::file;
    out.target_id = asset->id;
    out.delivery = "proxy";
    out.content_type = content_type_of(asset->container.value_or(""));
    out.range_supported = true;
    out.properties_json = asset->properties_json;
    if (request.auth == MediaAuth::bearer) {
        out.url = media_base_url_ + "/media/v1/files/" + asset->id;
        out.auth = "bearer";
        return out; // every request is authorized again; no expiry of its own
    }
    const auto expires_at = clock_->now() + request.ttl.value_or(default_ttl_);
    auto ticket = tickets_->issue({request.owner_id, request.token_id, TargetKind::file, asset->id, expires_at});
    if (!ticket) {
        return std::move(ticket).error();
    }
    out.url = media_base_url_ + "/media/v1/tickets/" + ticket->token;
    out.auth = "url";
    out.expires_at = ticket->expires_at;
    return out;
}

} // namespace music_service::application::media
