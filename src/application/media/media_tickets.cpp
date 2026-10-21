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
                                       std::size_t per_owner)
    : codec_(std::move(codec)), clock_(std::move(clock)), capacity_(std::max<std::size_t>(1, capacity)),
      per_owner_(std::max<std::size_t>(1, per_owner)) {}

void MediaTicketService::purge(std::chrono::system_clock::time_point now) {
    for (auto it = live_.begin(); it != live_.end();) {
        if (it->second.expires_at > now) {
            ++it;
            continue;
        }
        if (auto owned = owned_.find(it->second.owner_id); owned != owned_.end() && --owned->second == 0) {
            owned_.erase(owned);
        }
        if (expired_.insert(it->first).second) {
            expired_order_.push_back(it->first);
        }
        it = live_.erase(it);
    }
    while (expired_order_.size() > capacity_) {
        expired_.erase(expired_order_.front());
        expired_order_.pop_front();
    }
}

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

    const std::lock_guard lock{mutex_};
    purge(clock_->now());
    if (live_.size() >= capacity_ || owned_[grant.owner_id] >= per_owner_) {
        return Error{ErrorCode::service_busy, "too many live media tickets", true, {}};
    }
    const auto expires_at = grant.expires_at;
    ++owned_[grant.owner_id];
    live_.emplace(hash, std::move(grant));
    return IssuedTicket{std::move(token), expires_at};
}

domain::Result<TicketGrant> MediaTicketService::authorize(std::string_view token) const {
    if (token.substr(0, ticket_prefix.size()) != ticket_prefix || token.size() > 128) {
        return Error{ErrorCode::not_found, "no such media ticket", false, {}};
    }
    const auto hash = codec_->hash(token);
    const auto now = clock_->now();
    const std::lock_guard lock{mutex_};
    const auto it = live_.find(hash);
    if (it == live_.end()) {
        if (expired_.count(hash) != 0) {
            return Error{ErrorCode::media_ticket_expired, "the media ticket has expired", false, {}};
        }
        return Error{ErrorCode::not_found, "no such media ticket", false, {}};
    }
    if (it->second.expires_at <= now) {
        return Error{ErrorCode::media_ticket_expired, "the media ticket has expired", false, {}};
    }
    return it->second;
}

std::size_t MediaTicketService::size() const {
    const std::lock_guard lock{mutex_};
    return live_.size();
}

} // namespace music_service::application::media
