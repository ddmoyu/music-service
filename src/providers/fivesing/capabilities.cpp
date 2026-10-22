#include "providers/fivesing/capabilities.hpp"

#include "application/transport/url.hpp"
#include "providers/fivesing/parsing/parsers.hpp"
#include "providers/fivesing/protocol/requests.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace music_service::providers::fivesing {

namespace {

using domain::Error;
using domain::ErrorCode;

constexpr std::size_t max_query_bytes = 200;

Error http_status_error(const ports::HttpResponseHead& head) {
    const auto status = head.status;
    if (status == 429) {
        Error error{ErrorCode::upstream_rate_limited, "5sing is limiting requests", true, {}};
        error.details.push_back({"http_status", "429"});
        if (auto wait = head.header("Retry-After"); wait && !wait->empty() &&
                                                    wait->find_first_not_of("0123456789") == std::string::npos) {
            error.details.push_back({"retry_after_seconds", std::move(*wait)});
        }
        return error;
    }
    Error error{ErrorCode::upstream_failed, "5sing answered with an unexpected status", status >= 500, {}};
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

    // A JSON API call: anything but 200 is an error.
    domain::Result<std::string> body(ports::HttpRequest request, const ports::ProviderContext& context) const {
        auto response = fetch(std::move(request), context);
        if (!response) {
            return std::move(response).error();
        }
        if (response->head.status != 200) {
            return http_status_error(response->head);
        }
        return std::move(response->body);
    }

private:
    domain::SourceId source_;
    std::shared_ptr<ports::UpstreamHttp> upstream_;
    std::string rate_limit_group_;
};

domain::Result<parsing::WorkKey> work_key(const domain::TrackRef& track) {
    if (track.source.value() != protocol::source) {
        return Error::validation("track", "is not a 5sing track");
    }
    return parsing::parse_work_key(track.id.value());
}

// Search cursor: "<page>" or "<page>:<offset>" into the upstream's fixed-size page, so a
// limit below the page size continues where the last call stopped.
struct SearchCursor {
    std::uint32_t page{1};
    std::uint32_t offset{0};
};

std::optional<SearchCursor> parse_cursor(std::string_view text) {
    const auto read = [](std::string_view part, std::uint32_t& value) {
        const auto [end, ec] = std::from_chars(part.data(), part.data() + part.size(), value);
        return !part.empty() && ec == std::errc{} && end == part.data() + part.size();
    };
    SearchCursor cursor;
    const auto colon = text.find(':');
    if (!read(text.substr(0, colon), cursor.page) || cursor.page < 1 || cursor.page > 10000) {
        return std::nullopt;
    }
    if (colon != std::string_view::npos &&
        (!read(text.substr(colon + 1), cursor.offset) || cursor.offset < 1 ||
         cursor.offset >= protocol::search_page_size)) {
        return std::nullopt;
    }
    return cursor;
}

class Search final : public ports::ITrackSearch {
public:
    explicit Search(const ports::ProviderSetup& setup) : client_(setup) {}

    domain::Result<ports::TrackSearchPage> search_tracks(const ports::ProviderContext& context,
                                                         const ports::TrackSearchQuery& query) override {
        const auto text = parsing::clean_text(query.text);
        if (text.empty() || query.text.size() > max_query_bytes) {
            return Error::validation("q", "must be 1 to 200 bytes");
        }
        if (query.limit < 1 || query.limit > 50) {
            return Error::validation("limit", "must be between 1 and 50");
        }
        SearchCursor cursor;
        if (query.cursor) {
            auto parsed = parse_cursor(*query.cursor);
            if (!parsed) {
                return Error{ErrorCode::cursor_mismatch, "cursor is not a 5sing search position", false, {}};
            }
            cursor = *parsed;
        }
        auto body = client_.body(protocol::search_tracks(query.text, cursor.page), context);
        if (!body) {
            return std::move(body).error();
        }
        auto parsed = parsing::parse_search(*body);
        if (!parsed) {
            return std::move(parsed).error();
        }
        // An empty result reports page 1 whatever was asked; any other answer must be the
        // requested page, or the cursor would repeat or skip works.
        if (parsed->page != cursor.page && !(parsed->total == 0 && parsed->tracks.empty())) {
            Error error{ErrorCode::upstream_failed, "the 5sing response could not be read", false, {}};
            error.details.push_back({"protocol", "search response is for another page"});
            return error;
        }
        // One upstream page per call: at most limit works from the cursor's offset.
        auto& works = parsed->tracks;
        const auto from = std::min<std::size_t>(cursor.offset, works.size());
        const auto to = std::min<std::size_t>(from + query.limit, works.size());
        ports::TrackSearchPage result;
        result.page.items.assign(std::make_move_iterator(works.begin() + static_cast<std::ptrdiff_t>(from)),
                                 std::make_move_iterator(works.begin() + static_cast<std::ptrdiff_t>(to)));
        if (to < works.size()) {
            result.next_cursor = std::to_string(cursor.page) + ":" + std::to_string(to);
        } else if (!works.empty() && cursor.page < parsed->total_pages) {
            result.next_cursor = std::to_string(cursor.page + 1);
        }
        const auto returned = static_cast<std::uint32_t>(result.page.items.size());
        result.page.page = {query.limit, returned, parsed->total, result.next_cursor.has_value()};
        return result;
    }

private:
    Client client_;
};

class Detail final : public ports::ITrackDetail, public ports::ILyrics {
public:
    explicit Detail(const ports::ProviderSetup& setup) : client_(setup) {}

    domain::Result<domain::Track> track_detail(const ports::ProviderContext& context,
                                               const domain::TrackRef& track) override {
        auto info = song_info(context, track, "track_detail");
        if (!info) {
            return std::move(info).error();
        }
        return std::move(info->track);
    }

    domain::Result<domain::TrackLyrics> lyrics(const ports::ProviderContext& context,
                                               const domain::TrackRef& track) override {
        auto info = song_info(context, track, "lyrics");
        if (!info) {
            return std::move(info).error();
        }
        return std::move(info->lyrics);
    }

private:
    domain::Result<parsing::SongInfo> song_info(const ports::ProviderContext& context, const domain::TrackRef& track,
                                                std::string_view operation) {
        auto key = work_key(track);
        if (!key) {
            return std::move(key).error();
        }
        auto body = client_.body(protocol::song_info(key->type, key->id, operation), context);
        if (!body) {
            return std::move(body).error();
        }
        return parsing::parse_song_info(*body, *key);
    }

    Client client_;
};

class Links final : public ports::ILinkParser {
public:
    explicit Links(domain::SourceId source) : source_(std::move(source)) {}

    domain::Result<ports::ParsedLink> parse_link(std::string_view link) override {
        auto key = parsing::parse_track_link(link);
        if (!key) {
            return std::move(key).error();
        }
        auto id = domain::PlatformId::parse(key->platform_id());
        if (!id) {
            return std::move(id).error();
        }
        return ports::ParsedLink{"track", {source_, std::move(id).value()}};
    }

private:
    domain::SourceId source_;
};

// The tier serving each quality, in order of preference, with its bit rate.
struct TierChoice {
    std::string_view name;
    std::int64_t bitrate_bps;
};

std::vector<TierChoice> tiers_for(domain::Quality quality) {
    if (quality == domain::Quality::high) {
        return {{"sq", 320000}};
    }
    return {{"lq", 128000}, {"hq", 192000}};
}

// An HTTPS address on a kugou.com host and default port whose file name, when the tier
// gives an md5, is that md5: the primary and backup then name the same file.
bool usable_address(std::string_view text, const parsing::AudioTier& tier) {
    auto url = application::transport::Url::parse(text);
    if (!url || !url->is_https() || url->port() != 443) {
        return false;
    }
    constexpr std::string_view domain_suffix = ".kugou.com";
    const auto& host = url->host();
    if (host.size() <= domain_suffix.size() || host.substr(host.size() - domain_suffix.size()) != domain_suffix) {
        return false;
    }
    if (tier.md5.empty()) {
        return true;
    }
    const auto path = url->path();
    const auto file = path.substr(path.rfind('/') + 1);
    auto expected = tier.md5 + "." + tier.ext;
    std::transform(expected.begin(), expected.end(), expected.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::string actual{file};
    std::transform(actual.begin(), actual.end(), actual.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return actual == expected;
}

class Resolver final : public ports::IAssetResolver {
public:
    explicit Resolver(const ports::ProviderSetup& setup) : client_(setup) {}

    domain::Result<ports::ResolvedAudio> resolve_audio(const ports::ProviderContext& context,
                                                       const domain::TrackRef& track,
                                                       const domain::ResourceRequirement& requirement) override {
        auto key = work_key(track);
        if (!key) {
            return std::move(key).error();
        }
        auto body = client_.body(protocol::song_urls(key->type, key->id), context);
        if (!body) {
            return std::move(body).error();
        }
        auto tiers = parsing::parse_song_urls(*body, *key);
        if (!tiers) {
            return std::move(tiers).error();
        }
        const auto find_tier = [&](std::string_view name) -> const parsing::AudioTier* {
            const auto it = std::find_if(tiers->begin(), tiers->end(),
                                         [&](const parsing::AudioTier& tier) { return tier.name == name; });
            return it == tiers->end() ? nullptr : &*it;
        };
        // The qualities this work offers, then the one the requirement settles for.
        std::vector<domain::Quality> offered;
        for (const auto quality : {domain::Quality::standard, domain::Quality::high}) {
            const auto choices = tiers_for(quality);
            if (std::any_of(choices.begin(), choices.end(), [&](const TierChoice& c) { return find_tier(c.name); })) {
                offered.push_back(quality);
            }
        }
        auto chosen = domain::choose_quality(requirement, offered);
        if (!chosen) {
            return std::move(chosen).error();
        }
        for (const auto& choice : tiers_for(*chosen)) {
            if (const auto* tier = find_tier(choice.name)) {
                return audio_of(context, track, *chosen, choice.bitrate_bps, *tier);
            }
        }
        return Error{ErrorCode::quality_unavailable, "requested quality is not available", false, {}};
    }

private:
    domain::Result<ports::ResolvedAudio> audio_of(const ports::ProviderContext& context, const domain::TrackRef& track,
                                                  domain::Quality quality, std::int64_t bitrate_bps,
                                                  const parsing::AudioTier& tier) {
        // Primary first; the backup only when it is the same file.
        std::vector<std::string> candidates;
        if (usable_address(tier.primary, tier)) {
            candidates.push_back(tier.primary);
        }
        if (!tier.md5.empty() && usable_address(tier.backup, tier)) {
            candidates.push_back(tier.backup);
        }
        if (candidates.empty()) {
            Error error{ErrorCode::upstream_failed, "the 5sing response could not be read", false, {}};
            error.details.push_back({"protocol", "audio address is not an HTTPS kugou.com address of the tier's file"});
            return error;
        }
        std::optional<Error> last;
        for (const auto& url : candidates) {
            auto probe = client_.fetch(protocol::audio_probe(url), context);
            if (!probe) {
                // Out of capacity or out of time: the caller re-plans, the backup would not help.
                if (probe.error().code == ErrorCode::service_busy || context.operation.stop.stop_requested() ||
                    context.operation.expired(std::chrono::steady_clock::now())) {
                    return std::move(probe).error();
                }
                last = std::move(probe).error();
                continue;
            }
            if (probe->head.status != 200) {
                last = http_status_error(probe->head);
                continue;
            }
            // A length other than the declared one is another file, never a substitute.
            std::optional<std::uint64_t> length;
            if (auto header = probe->head.header("Content-Length")) {
                std::uint64_t value = 0;
                const auto [end, ec] = std::from_chars(header->data(), header->data() + header->size(), value);
                if (ec == std::errc{} && end == header->data() + header->size()) {
                    length = value;
                }
            }
            if (tier.size && length && *length != *tier.size) {
                Error error{ErrorCode::upstream_failed, "the 5sing audio address serves another file", false, {}};
                error.details.push_back({"protocol", "content length differs from the declared size"});
                last = std::move(error);
                continue;
            }
            const bool mp3 = tier.ext == "mp3";
            ports::ResolvedAudio audio{track,
                                       mp3 ? std::optional<domain::Quality>{quality} : std::nullopt,
                                       domain::Completeness::unknown,
                                       tier.ext.empty() ? std::nullopt : std::optional<std::string>{tier.ext},
                                       mp3 ? std::optional<std::int64_t>{bitrate_bps} : std::nullopt,
                                       url,
                                       {{"User-Agent", std::string{protocol::user_agent}}},
                                       std::nullopt,
                                       std::nullopt,
                                       ports::ProcessingPlan{}};
            // 5sing serves the uploaded work itself and has no preview clips: the address
            // answering with exactly the declared size is the whole file. The duration is
            // never given and stays unknown.
            if (tier.size && length && *length == *tier.size) {
                audio.completeness = domain::Completeness::full;
            }
            return audio;
        }
        return std::move(*last);
    }

    Client client_;
};

domain::Result<void> require_upstream(const ports::ProviderSetup& setup) {
    if (!setup.upstream) {
        return Error{ErrorCode::internal, "5sing needs the upstream client", false, {}};
    }
    return domain::ok();
}

template <class Port, class Impl> domain::Result<std::shared_ptr<Port>> make_networked(const ports::ProviderSetup& setup) {
    if (auto ready = require_upstream(setup); !ready) {
        return ready.error();
    }
    return std::shared_ptr<Port>{std::make_shared<Impl>(setup)};
}

} // namespace

domain::Result<std::shared_ptr<ports::ITrackSearch>> make_search(const ports::ProviderSetup& setup) {
    return make_networked<ports::ITrackSearch, Search>(setup);
}

domain::Result<std::shared_ptr<ports::ITrackDetail>> make_detail(const ports::ProviderSetup& setup) {
    return make_networked<ports::ITrackDetail, Detail>(setup);
}

domain::Result<std::shared_ptr<ports::ILyrics>> make_lyrics(const ports::ProviderSetup& setup) {
    return make_networked<ports::ILyrics, Detail>(setup);
}

domain::Result<std::shared_ptr<ports::ILinkParser>> make_link_parser(const ports::ProviderSetup& setup) {
    return std::shared_ptr<ports::ILinkParser>{std::make_shared<Links>(setup.source)};
}

domain::Result<std::shared_ptr<ports::IAssetResolver>> make_resolver(const ports::ProviderSetup& setup) {
    return make_networked<ports::IAssetResolver, Resolver>(setup);
}

} // namespace music_service::providers::fivesing
