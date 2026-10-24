#include "providers/netease/capabilities.hpp"
#include "providers/netease/client.hpp"
#include "providers/netease/membership.hpp"

#include "providers/netease/parsing/parsers.hpp"
#include "providers/netease/protocol/requests.hpp"
#include "providers/netease/protocol/weapi.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <mutex>
#include <string>
#include <utility>

namespace music_service::providers::netease {

namespace {

using domain::Error;
using domain::ErrorCode;

constexpr std::size_t max_query_bytes = 200;
// Deepest offset the service pages to; cloudsearch stops answering far beyond it.
constexpr std::uint32_t max_offset = 10000;

std::mutex key_mutex;
SecretKeySource& key_source() {
    static SecretKeySource source = protocol::random_secret_key;
    return source;
}

std::string secret_key() {
    const std::lock_guard lock{key_mutex};
    return key_source()();
}

// The cookie of the bound user, if this call has one.
std::string_view cookie_of(const ports::ProviderContext& context) {
    return context.secret ? context.secret->reveal() : std::string_view{};
}

domain::Result<void> require_upstream(const ports::ProviderSetup& setup) {
    if (!setup.upstream) {
        return Error{ErrorCode::internal, "netease needs the upstream client", false, {}};
    }
    return domain::ok();
}

domain::Result<std::string> song_id(const domain::TrackRef& track) {
    if (track.source.value() != protocol::source) {
        return Error::validation("track", "is not a netease track");
    }
    if (auto program = parsing::program_audio(track.id.value())) return program->song_id;
    if (!parsing::is_valid_song_id(track.id.value())) {
        return Error::validation("track", "is not a netease song id");
    }
    return track.id.value();
}

domain::Result<domain::Episode> episode_for(const Client& client, const ports::ProviderContext& c,
    const domain::TrackRef& track, std::string_view operation) {
    const auto program = parsing::program_audio(track.id.value());
    if (!program) return Error::validation("track", "invalid program audio reference");
    auto request = protocol::episode(program->program_id, call_for(cookie_of(c)));
    request.operation = operation;
    auto body = client.body(std::move(request), c, cookie_of(c));
    if (!body) return body.error();
    auto episode = parsing::parse_episode(*body, program->program_id);
    if (!episode) return episode.error();
    if (!episode->audio_ref || *episode->audio_ref != track)
        return Error{ErrorCode::not_found, "episode audio was removed or replaced; refresh episode details", false, {}};
    return episode;
}

class Search final : public ports::ITrackSearch {
public:
    explicit Search(const ports::ProviderSetup& setup) : client_(setup) {}

    domain::Result<ports::TrackSearchPage> search_tracks(const ports::ProviderContext& context,
                                                         const ports::TrackSearchQuery& query) override {
        if (query.text.empty() || query.text.size() > max_query_bytes) {
            return Error::validation("q", "must be 1 to 200 bytes");
        }
        if (query.limit < 1 || query.limit > protocol::max_search_page_size) {
            return Error::validation("limit", "must be between 1 and 50");
        }
        // The cursor is the offset of the next page.
        std::uint32_t offset = 0;
        if (query.cursor) {
            const auto& cursor = *query.cursor;
            const auto [end, ec] = std::from_chars(cursor.data(), cursor.data() + cursor.size(), offset);
            if (cursor.empty() || ec != std::errc{} || end != cursor.data() + cursor.size() || offset > max_offset) {
                return Error{ErrorCode::cursor_mismatch, "cursor is not a netease search offset", false, {}};
            }
        }
        auto body = client_.body(protocol::search_tracks(query.text, offset, query.limit, call_for({})), context);
        if (!body) {
            return std::move(body).error();
        }
        auto parsed = parsing::parse_search(*body);
        if (!parsed) {
            return std::move(parsed).error();
        }
        ports::TrackSearchPage result;
        const auto returned = static_cast<std::uint32_t>(parsed->tracks.size());
        const auto next = static_cast<std::uint64_t>(offset) + returned;
        // songCount is an estimate that shrinks on later pages; an empty page ends the list.
        const bool more = returned > 0 && next < parsed->total && next <= max_offset;
        result.page.items = std::move(parsed->tracks);
        result.page.page = {query.limit, returned, parsed->total, more};
        if (more) {
            result.next_cursor = std::to_string(next);
        }
        return result;
    }

private:
    Client client_;
};

class Detail final : public ports::ITrackDetail {
public:
    explicit Detail(const ports::ProviderSetup& setup) : client_(setup) {}

    domain::Result<domain::Track> track_detail(const ports::ProviderContext& context,
                                               const domain::TrackRef& track) override {
        auto id = song_id(track);
        if (!id) {
            return std::move(id).error();
        }
        if (parsing::program_audio(track.id.value())) {
            auto episode = episode_for(client_, context, track, "track_detail");
            if (!episode) return episode.error();
            domain::Track result{track, episode->title};
            result.album = domain::AlbumSummary{episode->podcast_title, episode->podcast};
            result.artwork = episode->artwork;
            result.duration_ms = episode->duration_ms;
            return result;
        }
        auto body = client_.body(protocol::song_detail(*id, call_for({})), context);
        if (!body) {
            return std::move(body).error();
        }
        return parsing::parse_detail(*body, *id);
    }

private:
    Client client_;
};

class Links final : public ports::ILinkParser {
public:
    explicit Links(domain::SourceId source) : source_(std::move(source)) {}

    domain::Result<ports::ParsedLink> parse_link(std::string_view link) override {
        auto id = parsing::parse_link(link);
        if (!id) {
            return std::move(id).error();
        }
        auto platform_id = domain::PlatformId::parse(id->id);
        if (!platform_id) {
            return std::move(platform_id).error();
        }
        return ports::ParsedLink{id->kind, {source_, std::move(platform_id).value()}};
    }

private:
    domain::SourceId source_;
};

class Lyrics final : public ports::ILyrics {
public:
    explicit Lyrics(const ports::ProviderSetup& setup) : client_(setup) {}

    domain::Result<domain::TrackLyrics> lyrics(const ports::ProviderContext& context,
                                               const domain::TrackRef& track) override {
        auto id = song_id(track);
        if (!id) {
            return std::move(id).error();
        }
        if (parsing::program_audio(track.id.value())) return domain::TrackLyrics{.unavailable = true};
        auto body = client_.body(protocol::lyric(*id, call_for({})), context);
        if (!body) {
            return std::move(body).error();
        }
        return parsing::parse_lyrics(*body);
    }

private:
    Client client_;
};

class Resolver final : public ports::IAssetResolver {
public:
    explicit Resolver(const ports::ProviderSetup& setup) : client_(setup) {}

    domain::Result<ports::ResolvedAudio> resolve_audio(const ports::ProviderContext& context,
                                                       const domain::TrackRef& track,
                                                       const domain::ResourceRequirement& requirement) override {
        auto id = song_id(track);
        if (!id) {
            return std::move(id).error();
        }
        std::optional<domain::Episode> episode;
        if (parsing::program_audio(track.id.value())) {
            auto loaded = episode_for(client_, context, track, "resolve_audio");
            if (!loaded) return loaded.error();
            if (loaded->requires_purchase) return Error{ErrorCode::credentials_required,
                "this episode requires a purchase", false, {{"netease_access", "episode_purchase_required"}}};
            episode = std::move(*loaded);
        }
        constexpr std::array offered{domain::Quality::standard, domain::Quality::high, domain::Quality::lossless, domain::Quality::hires};
        auto chosen = domain::choose_quality(requirement, std::span{offered}.first(episode ? 1U : offered.size()));
        if (!chosen) return chosen.error();
        const auto level = *chosen == domain::Quality::hires ? "hires" : *chosen == domain::Quality::lossless ? "lossless" :
                           *chosen == domain::Quality::high ? "exhigh" : "standard";
        const auto cookie = cookie_of(context);
        auto body = client_.body(protocol::song_url(*id, level, call_for(cookie)), context, cookie);
        if (!body) {
            return std::move(body).error();
        }
        auto evidence = parsing::parse_audio(*body, *id);
        if (!evidence) {
            return std::move(evidence).error();
        }
        // Program URLs omit level/time. Preserve the verified program duration for the
        // byte-level completeness check; never grant a higher tier from missing evidence.
        if (episode && !evidence->duration_ms) evidence->duration_ms = episode->duration_ms;
        switch (evidence->code) {
        case 200:
            break;
        case 404:
            return Error{ErrorCode::not_found, "netease has no playable audio for this song", false, {}};
        case -110: {
            Error error{ErrorCode::credentials_required,
                        context.secret ? "this netease account cannot play the song (membership or purchase needed)"
                                       : "the song needs a signed-in netease account with access",
                        false,
                        {}};
            error.details.push_back({"netease_song_code", "-110"});
            if (evidence->fee) error.details.push_back({"netease_fee", std::to_string(*evidence->fee)});
            return error;
        }
        default: {
            Error error{ErrorCode::upstream_failed, "netease refused the audio address", false, {}};
            error.details.push_back({"netease_song_code", std::to_string(evidence->code)});
            return error;
        }
        }

        std::optional<domain::Quality> quality;
        auto completeness = domain::Completeness::unknown;
        if (evidence->trial) {
            completeness = domain::Completeness::preview;
        } else {
            const bool standard_mp3 = (evidence->level == "standard" || (episode && evidence->level.empty())) && evidence->container == "mp3" &&
                                      evidence->bitrate_bps && *evidence->bitrate_bps >= 96000 &&
                                      *evidence->bitrate_bps <= 160000;
            if (standard_mp3) {
                quality = domain::Quality::standard;
            } else if (evidence->level == "exhigh" && evidence->container == "mp3" &&
                       evidence->bitrate_bps && *evidence->bitrate_bps >= 300000 && *evidence->bitrate_bps <= 340000) {
                quality = domain::Quality::high;
            } else if (evidence->level == "lossless" && evidence->container == "flac" &&
                       evidence->bitrate_bps && *evidence->bitrate_bps > 0) {
                quality = domain::Quality::lossless;
            } else if (evidence->level == "hires" && evidence->container == "flac" &&
                       evidence->bitrate_bps && *evidence->bitrate_bps > 0) {
                quality = domain::Quality::hires; // 发布前仍须检查文件的位深与采样率。
            }
            if (!quality && requirement.requested != domain::Quality::standard) {
                return Error{ErrorCode::quality_unavailable, "netease returned an unverified quality tier", false, {}};
            }
            // No trial marker and a length: the platform hands out the whole song. Audio
            // verification checks the bytes.
            if (evidence->duration_ms) {
                completeness = domain::Completeness::full;
            }
        }
        ports::ResolvedAudio audio{track,
                                   quality,
                                   completeness,
                                   evidence->container.empty() ? std::nullopt
                                                               : std::optional<std::string>{evidence->container},
                                   evidence->bitrate_bps,
                                   std::move(evidence->url),
                                   {{"User-Agent", std::string{protocol::user_agent}}},
                                   std::nullopt,
                                   evidence->duration_ms,
                                   ports::ProcessingPlan{}};
        if (evidence->expires_in_seconds) {
            audio.expires_at = std::chrono::system_clock::now() + std::chrono::seconds{*evidence->expires_in_seconds};
        }
        return audio;
    }

private:
    Client client_;
};

class Verifier final : public ports::ICredentialVerifier {
public:
    explicit Verifier(const ports::ProviderSetup& setup) : client_(setup) {}

    domain::Result<ports::CredentialVerification> verify(const ports::CredentialSnapshot& credential,
                                                         const ports::OperationContext& operation) override {
        const auto cookie = credential.secret.reveal();
        ports::CredentialVerification verification;
        if (!protocol::music_u_of(cookie)) {
            verification.status = ports::VerificationStatus::invalid;
            return verification;
        }
        const ports::ProviderContext context{operation, credential.identity, std::nullopt, nullptr};
        auto body = client_.body(protocol::account(call_for(cookie)), context, cookie);
        if (!body) {
            return std::move(body).error();
        }
        auto account = parsing::parse_account(*body);
        if (!account) {
            if (account.error().code == ErrorCode::credentials_required) {
                verification.status = ports::VerificationStatus::invalid;
                return verification;
            }
            return std::move(account).error();
        }
        if (!account->signed_in) {
            verification.status = ports::VerificationStatus::invalid;
            return verification;
        }
        verification.status = ports::VerificationStatus::valid;
        verification.account_hint = account->nickname;
        try {
            auto membership_body = client_.body(protocol::membership_info(call_for(cookie)), context, cookie);
            verification.membership = membership_body ? parse_membership(*membership_body, std::chrono::system_clock::now())
                : membership::unavailable(std::string{domain::to_string(membership_body.error().code)});
        } catch (const std::exception&) { verification.membership = membership::unavailable("UPSTREAM_RESPONSE_INVALID"); }
        return verification;
    }

private:
    Client client_;
};

} // namespace

protocol::Call call_for(std::string_view cookie) {
    return {secret_key(), protocol::csrf_of(cookie).value_or("")};
}

void set_secret_key_source_for_testing(SecretKeySource source) {
    const std::lock_guard lock{key_mutex};
    key_source() = source ? std::move(source) : SecretKeySource{protocol::random_secret_key};
}

domain::Result<std::shared_ptr<ports::ITrackSearch>> make_search(const ports::ProviderSetup& setup) {
    if (auto ready = require_upstream(setup); !ready) {
        return ready.error();
    }
    return std::shared_ptr<ports::ITrackSearch>{std::make_shared<Search>(setup)};
}

domain::Result<std::shared_ptr<ports::ITrackDetail>> make_detail(const ports::ProviderSetup& setup) {
    if (auto ready = require_upstream(setup); !ready) {
        return ready.error();
    }
    return std::shared_ptr<ports::ITrackDetail>{std::make_shared<Detail>(setup)};
}

domain::Result<std::shared_ptr<ports::ILinkParser>> make_link_parser(const ports::ProviderSetup& setup) {
    return std::shared_ptr<ports::ILinkParser>{std::make_shared<Links>(setup.source)};
}

domain::Result<std::shared_ptr<ports::ILyrics>> make_lyrics(const ports::ProviderSetup& setup) {
    if (auto ready = require_upstream(setup); !ready) {
        return ready.error();
    }
    return std::shared_ptr<ports::ILyrics>{std::make_shared<Lyrics>(setup)};
}

domain::Result<std::shared_ptr<ports::IAssetResolver>> make_resolver(const ports::ProviderSetup& setup) {
    if (auto ready = require_upstream(setup); !ready) {
        return ready.error();
    }
    return std::shared_ptr<ports::IAssetResolver>{std::make_shared<Resolver>(setup)};
}

domain::Result<std::shared_ptr<ports::ICredentialVerifier>> make_verifier(const ports::ProviderSetup& setup) {
    if (auto ready = require_upstream(setup); !ready) {
        return ready.error();
    }
    return std::shared_ptr<ports::ICredentialVerifier>{std::make_shared<Verifier>(setup)};
}

} // namespace music_service::providers::netease
