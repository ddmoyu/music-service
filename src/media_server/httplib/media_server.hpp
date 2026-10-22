#pragma once

#include "application/accounts/identity_admin.hpp"
#include "application/config/service_config.hpp"
#include "application/catalog/asset_service.hpp"
#include "application/media/file_access.hpp"
#include "application/media/media_tickets.hpp"
#include "application/ports/runtime/executor.hpp"
#include "domain/result.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace music_service::media_server {

struct MediaServerOptions {
    application::config::ListenerConfig listener;
    application::config::MediaLimits limits;
    application::config::MediaStream stream;
    application::config::Cors cors;
    bool media_proxy{false}; // online asset proxy; assets answer 501 without it
};

struct MediaServerServices {
    std::shared_ptr<const application::accounts::AccessTokenAuthenticator> authenticator;
    std::shared_ptr<application::media::FileAccessService> files;
    std::shared_ptr<application::media::MediaTicketService> tickets;
    // Online assets for the proxy, and the bounded pool whose threads read their upstream
    // (media_upstream.workers); both may be empty without media_proxy.
    std::shared_ptr<application::catalog::AssetService> assets;
    std::shared_ptr<application::ports::WorkExecutor> upstream_pool;
    // Whether a ticket's owner is still enabled and the token that asked for it unrevoked.
    std::function<bool(const std::string& owner_id, const std::string& token_id)> grant_valid;
    std::function<std::string()> new_request_id; // "req_..." ids
    // Called once per finished request; ticket tokens in the path are already redacted.
    std::function<void(const std::string& request_id, const std::string& method, const std::string& path, int status,
                       std::chrono::milliseconds elapsed)>
        on_request;
};

// The media entry (api-contract.md 10.4, 10.5) on its own cpp-httplib listener and worker
// pool: GET/HEAD /media/v1/files/{file_id} with a files:read Bearer token and
// /media/v1/tickets/{token} with a ticket from media-access, and /media/v1/assets/{asset_id}
// with a media:read Bearer token for online audio (media_proxy, else FEATURE_UNAVAILABLE).
// Files are read from their bound destination without touching any platform; online audio
// is forwarded from the upstream as it answers (200 stays 200, a 206 keeps its span, 416
// passes through), through a buffer of media_stream.bridge_buffer_bytes: a slow client
// slows the upstream read and a client that leaves cancels it. Nothing is cached.
//   - 200 with Content-Length, strong ETag and Accept-Ranges; one range (bytes=a-b, a-,
//     -n) is 206 with Content-Range; no overlap with the file is 416 with
//     "Content-Range: bytes */size"; several ranges are 400 MULTIPLE_RANGES_UNSUPPORTED;
//     a non-matching If-Range serves the whole file; If-None-Match answers 304; HEAD
//     ignores Range.
//   - Files are attachments, ticket responses inline; the display name, never a path.
//   - A response holds a slot (media_limits: global, local files, per principal) from
//     before its headers until its content provider ends, which also happens when the
//     client disconnects; no slot is a JSON 429/503 before any byte.
//   - Errors before the headers are the JSON envelope; after them the connection closes.
class MediaServer {
public:
    MediaServer(MediaServerOptions options, MediaServerServices services);
    ~MediaServer();
    MediaServer(const MediaServer&) = delete;
    MediaServer& operator=(const MediaServer&) = delete;

    domain::Result<std::uint16_t> bind();
    void start();
    // Ends responses in flight at their next chunk, stops accepting and joins. Idempotent.
    void stop();

    [[nodiscard]] std::uint16_t port() const noexcept;
    // Responses holding a slot right now.
    [[nodiscard]] std::uint32_t active() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace music_service::media_server
