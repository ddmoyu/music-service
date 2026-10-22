#pragma once

#include "application/catalog/asset_service.hpp"
#include "application/media/file_access.hpp"
#include "application/media/media_tickets.hpp"
#include "application/ports/runtime/clock.hpp"
#include "domain/result.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>

namespace music_service::application::media {

enum class DeliveryMode { automatic, direct, proxy };
enum class MediaAuth { ticket, bearer };

struct AccessRequest {
    std::string owner_id;
    std::string token_id; // the caller's access token; its scope was checked for the kind
    TargetKind kind{TargetKind::file};
    std::string target_id;
    DeliveryMode mode{DeliveryMode::automatic};
    MediaAuth auth{MediaAuth::ticket};
    std::optional<std::chrono::seconds> ttl; // 15..300 s, checked by the caller
};

// The access description of api-contract.md 10.2. properties_json is the stored asset
// properties (actual track and quality) for the entry to project.
struct AccessDescription {
    TargetKind kind{TargetKind::file};
    std::string target_id;
    std::string delivery; // direct | proxy
    std::string url;
    std::string auth; // url | bearer
    std::optional<std::chrono::system_clock::time_point> expires_at;
    std::string content_type;
    std::optional<bool> range_supported;
    std::string properties_json{"{}"}; // files: the stored asset properties
    // Assets: the recording and quality the address serves.
    std::optional<domain::TrackRef> actual_track;
    std::optional<domain::Quality> actual_quality;
};

struct MediaFeatures {
    bool file_access{false};
    bool media_proxy{false};
    bool direct_media_urls{false};
};

// The content type of a published container ("mp3" -> "audio/mpeg").
std::string content_type_of(std::string_view container);

// MediaAccessService.describe (internal-contracts.md 3): builds an access description for
// an existing file or asset of the caller without a playback session. Files are always
// read from the backend (direct is DELIVERY_UNAVAILABLE); auth=ticket issues a ticket
// limited to the file and the requested lifetime, auth=bearer returns the Bearer URL with
// no expiry (every request is authorized again).
// Online assets (api-contract.md 10.2): direct hands out the upstream address only with
// direct_media_urls enabled (else FEATURE_UNAVAILABLE) and for a shareable address that
// needs no secret or decryption (else DELIVERY_UNAVAILABLE); auto picks direct only then
// and the proxy otherwise; the proxy needs media_proxy (FEATURE_UNAVAILABLE). Audio that
// must be decrypted cannot be streamed either way. Every expiry is capped by the asset's.
class MediaAccessService {
public:
    MediaAccessService(MediaFeatures features, std::string media_base_url, std::chrono::seconds default_ttl,
                       std::shared_ptr<FileAccessService> files, std::shared_ptr<MediaTicketService> tickets,
                       std::shared_ptr<catalog::AssetService> assets, std::shared_ptr<const ports::Clock> clock);

    domain::Result<AccessDescription> describe(const AccessRequest& request);

private:
    domain::Result<AccessDescription> describe_file(const AccessRequest& request);
    domain::Result<AccessDescription> describe_asset(const AccessRequest& request);

    MediaFeatures features_;
    std::string media_base_url_;
    std::chrono::seconds default_ttl_;
    std::shared_ptr<FileAccessService> files_;
    std::shared_ptr<MediaTicketService> tickets_;
    std::shared_ptr<catalog::AssetService> assets_;
    std::shared_ptr<const ports::Clock> clock_;
};

} // namespace music_service::application::media
