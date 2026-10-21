#include "providers/kuwo/capabilities.hpp"

#include "providers/kuwo/parsing/parsers.hpp"
#include "providers/kuwo/protocol/requests.hpp"

#include <array>
#include <charconv>
#include <random>
#include <string>
#include <utility>

namespace music_service::providers::kuwo {

namespace {

using domain::Error;
using domain::ErrorCode;

constexpr std::size_t max_query_bytes = 200;

Error http_status_error(const ports::HttpResponseHead& head) {
    const auto status = head.status;
    if (status == 429) {
        // The upstream client already fed Retry-After to admission; pass it on as a hint.
        Error error{ErrorCode::upstream_rate_limited, "kuwo is limiting requests", true, {}};
        error.details.push_back({"http_status", "429"});
        if (auto wait = head.header("Retry-After"); wait && !wait->empty() &&
                                                    wait->find_first_not_of("0123456789") == std::string::npos) {
            error.details.push_back({"retry_after_seconds", std::move(*wait)});
        }
        return error;
    }
    Error error{ErrorCode::upstream_failed, "kuwo answered with an unexpected status", status >= 500, {}};
    error.details.push_back({"http_status", std::to_string(status)});
    return error;
}

// Shared request path: the upstream client with the operation's admission request.
class Client {
public:
    explicit Client(const ports::ProviderSetup& setup)
        : source_(setup.source), upstream_(setup.upstream), rate_limit_group_(setup.rate_limit_group) {}

    domain::Result<ports::HttpResponse> fetch(ports::HttpRequest request, const ports::ProviderContext& context) const {
        ports::AdmissionRequest base{context.identity.binding().principal_id(),
                                     source_,
                                     request.operation,
                                     ports::TrafficClass::short_request,
                                     context.identity,
                                     {},
                                     rate_limit_group_,
                                     context.operation.deadline};
        auto result = upstream_->fetch(std::move(request), base, context.operation);
        if (!result) {
            return std::move(result).error();
        }
        if (auto* pending = std::get_if<ports::PendingHop>(&result.value())) {
            Error error{ErrorCode::service_busy, "upstream capacity is not available now", true, {}};
            error.details.push_back({"admission", pending->deferred.blocked_dimension});
            return error;
        }
        return std::get<ports::HttpResponse>(std::move(result).value());
    }

private:
    domain::SourceId source_;
    std::shared_ptr<ports::UpstreamHttp> upstream_;
    std::string rate_limit_group_;
};

domain::Result<std::string> kuwo_rid(const domain::TrackRef& track) {
    if (track.source.value() != protocol::source) {
        return Error::validation("track", "is not a kuwo track");
    }
    if (!parsing::is_valid_rid(track.id.value())) {
        return Error::validation("track", "is not a kuwo song id");
    }
    return track.id.value();
}

class Search final : public ports::ITrackSearch {
public:
    explicit Search(const ports::ProviderSetup& setup) : client_(setup) {}

    domain::Result<ports::TrackSearchPage> search_tracks(const ports::ProviderContext& context,
                                                         const ports::TrackSearchQuery& query) override {
        const auto text = parsing::decode_text(query.text);
        if (text.empty() || text.size() > max_query_bytes) {
            return Error::validation("q", "must be 1 to 200 bytes");
        }
        if (query.limit < 1 || query.limit > protocol::max_search_page_size) {
            return Error::validation("limit", "must be between 1 and 50");
        }
        std::uint32_t page = 0;
        if (query.cursor) {
            const auto& cursor = *query.cursor;
            const auto [end, ec] = std::from_chars(cursor.data(), cursor.data() + cursor.size(), page);
            if (ec != std::errc{} || end != cursor.data() + cursor.size() || page > 10000) {
                return Error{ErrorCode::cursor_mismatch, "cursor is not a kuwo search page", false, {}};
            }
        }
        auto response = client_.fetch(protocol::search_tracks(query.text, page, query.limit), context);
        if (!response) {
            return std::move(response).error();
        }
        if (response->head.status != 200) {
            return http_status_error(response->head);
        }
        auto parsed = parsing::parse_search(response->body);
        if (!parsed) {
            return std::move(parsed).error();
        }
        // Another page would repeat or skip results under this cursor.
        if (parsed->page != page) {
            Error error{ErrorCode::upstream_failed, "the kuwo response could not be read", false, {}};
            error.details.push_back({"protocol", "search response is for another page"});
            return error;
        }
        ports::TrackSearchPage result;
        const auto returned = static_cast<std::uint32_t>(parsed->tracks.size());
        const auto seen = static_cast<std::uint64_t>(page + 1) * query.limit;
        const bool more = returned > 0 && seen < parsed->total;
        result.page.items = std::move(parsed->tracks);
        result.page.page = {query.limit, returned, parsed->total, more};
        if (more) {
            result.next_cursor = std::to_string(page + 1);
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
        auto rid = kuwo_rid(track);
        if (!rid) {
            return std::move(rid).error();
        }
        auto response = client_.fetch(protocol::track_page(*rid), context);
        if (!response) {
            return std::move(response).error();
        }
        // Kuwo answers 430 (or 404) with a page without song data for unknown ids.
        if (response->head.status == 404 || response->head.status == 430) {
            return Error{ErrorCode::not_found, "kuwo has no such song", false, {}};
        }
        if (response->head.status != 200) {
            return http_status_error(response->head);
        }
        return parsing::parse_track_page(response->body, *rid);
    }

private:
    Client client_;
};

class Links final : public ports::ILinkParser {
public:
    explicit Links(domain::SourceId source) : source_(std::move(source)) {}

    domain::Result<ports::ParsedLink> parse_link(std::string_view link) override {
        auto rid = parsing::parse_track_link(link);
        if (!rid) {
            return std::move(rid).error();
        }
        auto id = domain::PlatformId::parse(*rid);
        if (!id) {
            return std::move(id).error();
        }
        return ports::ParsedLink{"track", {source_, std::move(id).value()}};
    }

private:
    domain::SourceId source_;
};

class Resolver final : public ports::IAssetResolver {
public:
    explicit Resolver(const ports::ProviderSetup& setup) : client_(setup) {}

    domain::Result<ports::ResolvedAudio> resolve_audio(const ports::ProviderContext& context,
                                                       const domain::TrackRef& track,
                                                       const domain::ResourceRequirement& requirement) override {
        auto rid = kuwo_rid(track);
        if (!rid) {
            return std::move(rid).error();
        }
        // Only standard quality is implemented: strict higher requests fail here,
        // allow_lower falls back to it.
        constexpr std::array offered{domain::Quality::standard};
        if (auto chosen = domain::choose_quality(requirement, offered); !chosen) {
            return std::move(chosen).error();
        }
        auto response = client_.fetch(protocol::standard_audio(*rid, client_id()), context);
        if (!response) {
            return std::move(response).error();
        }
        if (response->head.status != 200) {
            return http_status_error(response->head);
        }
        auto evidence = parsing::parse_audio(response->body, *rid);
        if (!evidence) {
            return std::move(evidence).error();
        }

        const bool standard_mp3 = evidence->bitrate_kbps == 128 && evidence->format == "mp3";
        std::optional<domain::Quality> quality;
        domain::Completeness completeness = domain::Completeness::unknown;
        if (evidence->substitute) {
            // Another rid or a marked substitute: a short clip, not the song.
            completeness = domain::Completeness::preview;
        } else {
            if (standard_mp3) {
                quality = domain::Quality::standard;
            }
            // Same rid, regular type and a positive duration: the upstream declares the
            // whole song. Without a readable type it stays unknown. Audio verification
            // (T15) checks the bytes.
            if (evidence->regular && evidence->duration_seconds && *evidence->duration_seconds > 0) {
                completeness = domain::Completeness::full;
            }
        }
        std::optional<std::int64_t> bitrate_bps;
        if (evidence->bitrate_kbps && *evidence->bitrate_kbps >= 8) {
            bitrate_bps = *evidence->bitrate_kbps * 1000;
        }
        ports::ResolvedAudio audio{track,
                                   quality,
                                   completeness,
                                   evidence->format.empty() ? std::nullopt
                                                            : std::optional<std::string>{evidence->format},
                                   bitrate_bps,
                                   std::move(evidence->url),
                                   {{"User-Agent", std::string{protocol::user_agent}}},
                                   std::nullopt,
                                   std::nullopt,
                                   ports::ProcessingPlan{}};
        if (evidence->duration_seconds && *evidence->duration_seconds > 0) {
            audio.duration_ms = *evidence->duration_seconds * 1000;
        }
        return audio;
    }

private:
    // The "user" value the Kuwo client sends: a random installation-like id per call.
    static std::string client_id() {
        thread_local std::mt19937_64 engine{std::random_device{}()};
        return "C_APK_guanwang_" + std::to_string(engine() % 1000000000000ULL);
    }

    Client client_;
};

domain::Result<void> require_upstream(const ports::ProviderSetup& setup) {
    if (!setup.upstream) {
        return Error{ErrorCode::internal, "kuwo needs the upstream client", false, {}};
    }
    return domain::ok();
}

} // namespace

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

domain::Result<std::shared_ptr<ports::IAssetResolver>> make_resolver(const ports::ProviderSetup& setup) {
    if (auto ready = require_upstream(setup); !ready) {
        return ready.error();
    }
    return std::shared_ptr<ports::IAssetResolver>{std::make_shared<Resolver>(setup)};
}

} // namespace music_service::providers::kuwo
