#include "application/media/media_tickets.hpp"

#include <algorithm>
#include <utility>

namespace music_service::application::media {

namespace {

using domain::Error;
using domain::ErrorCode;

constexpr std::string_view ticket_prefix = "mtk_";

} // namespace

std::string_view to_string(TargetKind kind) noexcept {
    return kind == TargetKind::asset ? "asset" : "file";
}

MediaTicketService::MediaTicketService(std::shared_ptr<ports::AccessTokenCodec> codec,
                                       std::shared_ptr<const ports::Clock> clock, std::size_t capacity,
                                       std::size_t per_owner, std::shared_ptr<ports::MemoryBudget> budget)
    : codec_(std::move(codec)), clock_(std::move(clock)),
      tickets_(capacity, per_owner, {}, std::move(budget), [](const TicketGrant& grant) {
          return sizeof(grant) + grant.owner_id.capacity() + grant.token_id.capacity() + grant.target_id.capacity() + 3;
      }) {}

domain::Result<IssuedTicket> MediaTicketService::issue(TicketGrant grant) {
    auto generated = codec_->generate();
    if (!generated) {
        return std::move(generated).error();
    }
    // A random value of the access-token kind, with its own prefix so it is never one.
    const auto random = generated->reveal();
    const auto underscore = random.find('_');
    std::string token{ticket_prefix};
    token += underscore == std::string_view::npos ? random : random.substr(underscore + 1);
    const auto hash = codec_->hash(token);

    const auto expires_at = grant.expires_at;
    const auto owner = grant.owner_id;
    auto saved = tickets_.put_key(hash, owner, std::move(grant), expires_at, clock_->now());
    if (!saved) return saved.error();
    return IssuedTicket{std::move(token), expires_at};
}

domain::Result<TicketGrant> MediaTicketService::authorize(std::string_view token) const {
    if (token.substr(0, ticket_prefix.size()) != ticket_prefix || token.size() > 128) {
        return Error{ErrorCode::not_found, "no such media ticket", false, {}};
    }
    const auto hash = codec_->hash(token);
    auto found = tickets_.get_by_key(hash, clock_->now(), ErrorCode::media_ticket_expired);
    if (!found) return found.error();
    return std::move(found->first);
}

std::size_t MediaTicketService::size() const {
    return tickets_.size();
}

} // namespace music_service::application::media
