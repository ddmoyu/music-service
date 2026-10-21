#include "application/media/media_access.hpp"

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
    // The asset must be the caller's and still valid before any feature answer.
    auto asset = assets_->describe(request.owner_id, request.target_id);
    if (!asset) {
        return std::move(asset).error();
    }
    if (request.mode == DeliveryMode::direct ||
        (request.mode == DeliveryMode::automatic && features_.direct_media_urls)) {
        return unavailable("direct media URLs are not enabled");
    }
    return unavailable("the online media proxy is not enabled");
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
