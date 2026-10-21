#pragma once

#include "application/ports/identity/access_tokens.hpp"
#include "application/ports/runtime/clock.hpp"
#include "domain/result.hpp"

#include <chrono>
#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>

namespace music_service::application::media {

enum class TargetKind { file, asset };

std::string_view to_string(TargetKind kind) noexcept;

// What a ticket grants: one file or asset of one owner, issued through one access token.
struct TicketGrant {
    std::string owner_id;
    std::string token_id; // the access token that asked for it; revoking it ends the ticket
    TargetKind kind{TargetKind::file};
    std::string target_id;
    std::chrono::system_clock::time_point expires_at;
};

struct IssuedTicket {
    std::string token; // only ever placed in the issued URL
    std::chrono::system_clock::time_point expires_at;
};

// MediaTicketService (api-contract.md 10.3): tickets are issued only by media-access,
// map to one target, allow any number of requests until they expire and are kept in
// bounded memory (handles.media_tickets, a per-owner share) as the hash of the token,
// never the token itself. Lost on restart: clients ask media-access again. A full table
// is SERVICE_BUSY; expired entries go first and their hashes are remembered for a while,
// so an expired ticket reads as MEDIA_TICKET_EXPIRED rather than NOT_FOUND.
class MediaTicketService {
public:
    MediaTicketService(std::shared_ptr<ports::AccessTokenCodec> codec, std::shared_ptr<const ports::Clock> clock,
                       std::size_t capacity, std::size_t per_owner);

    domain::Result<IssuedTicket> issue(TicketGrant grant);

    // NOT_FOUND for an unknown token, MEDIA_TICKET_EXPIRED once it expired. Checked when
    // a request starts; a response already being sent may finish.
    domain::Result<TicketGrant> authorize(std::string_view token) const;

    [[nodiscard]] std::size_t size() const;

private:
    void purge(std::chrono::system_clock::time_point now);

    std::shared_ptr<ports::AccessTokenCodec> codec_;
    std::shared_ptr<const ports::Clock> clock_;
    std::size_t capacity_;
    std::size_t per_owner_;
    mutable std::mutex mutex_;
    std::map<std::string, TicketGrant> live_; // by token hash
    std::map<std::string, std::size_t> owned_;
    std::deque<std::string> expired_order_;
    std::set<std::string> expired_; // hashes of expired tickets, at most capacity_
};

} // namespace music_service::application::media
