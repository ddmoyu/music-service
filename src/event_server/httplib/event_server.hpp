#pragma once

#include "application/accounts/identity_admin.hpp"
#include "application/config/service_config.hpp"
#include "application/events/task_event_hub.hpp"
#include "domain/result.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace music_service::event_server {

// "event: task.updated" with {task_id, version, status, progress, updated_at}
// (api-contract.md 9.4). The encoder the hub uses, so queued bytes are exact.
std::string encode_task_event(const application::events::TaskEvent& event);

struct EventServerOptions {
    application::config::ListenerConfig listener;
    application::config::EventStream stream;
    application::config::Cors cors;
    std::string instance_id;
};

struct EventServerServices {
    std::shared_ptr<const application::accounts::AccessTokenAuthenticator> authenticator;
    std::shared_ptr<application::events::TaskEventHub> hub;
    std::function<std::string()> new_request_id; // "req_..." ids
    // Called once per finished request (a stream when it ends); must not throw.
    std::function<void(const std::string& request_id, const std::string& method, const std::string& path, int status,
                       std::chrono::milliseconds elapsed)>
        on_request;
};

// The SSE entry (GET /events/v1/downloads) on its own cpp-httplib listener and worker
// pool. Authentication, scope and the subscription limits are checked before the
// headers; errors are the JSON envelope. Each stream sends stream.ready, then the
// owner's committed task versions, comment heartbeats, and ends at the connection
// lifetime, on token revocation, on overflow (the client resynchronizes) or at stop.
// No event ids and no replay: every connection starts with a resync.
class EventServer {
public:
    EventServer(EventServerOptions options, EventServerServices services);
    ~EventServer();
    EventServer(const EventServer&) = delete;
    EventServer& operator=(const EventServer&) = delete;

    domain::Result<std::uint16_t> bind();
    void start();
    // Closes every stream, stops accepting and joins. Idempotent.
    void stop();

    [[nodiscard]] std::uint16_t port() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace music_service::event_server
