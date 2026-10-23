#include "providers/kuwo/capabilities.hpp"

#include "providers/kuwo/client.hpp"
#include "providers/kuwo/parsing/parsers.hpp"
#include "providers/kuwo/parsing/artists.hpp"
#include "providers/kuwo/protocol/requests.hpp"

#include <array>
#include <algorithm>
#include <charconv>
#include <random>
#include <string>
#include <utility>

namespace music_service::providers::kuwo {

namespace {

using domain::Error;
using domain::ErrorCode;

constexpr std::size_t max_query_bytes = 200;

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
        auto response = client_.fetch(protocol::track_info(*rid), context);
        if (!response) {
            return std::move(response).error();
        }
        // 歌曲详情和可信封面直接读取 JSON，不解析网页。
        if (response->head.status == 404 || response->head.status == 430) {
            return Error{ErrorCode::not_found, "kuwo has no such song", false, {}};
        }
        if (response->head.status != 200) {
            return http_status_error(response->head);
        }
        return parsing::parse_track_info(response->body, *rid);
    }

private:
    Client client_;
};

class Links final : public ports::ILinkParser {
public:
    explicit Links(domain::SourceId source) : source_(std::move(source)) {}

    domain::Result<ports::ParsedLink> parse_link(std::string_view link) override {
        auto rid = parsing::parse_link(link);
        if (!rid) {
            return std::move(rid).error();
        }
        auto id = domain::PlatformId::parse(rid->id);
        if (!id) {
            return std::move(id).error();
        }
        return ports::ParsedLink{rid->kind, {source_, std::move(id).value()}};
    }

private:
    domain::SourceId source_;
};

class Lyrics final : public ports::ILyrics {
public:
    explicit Lyrics(const ports::ProviderSetup& setup) : client_(setup) {}
    domain::Result<domain::TrackLyrics> lyrics(const ports::ProviderContext& context,
        const domain::TrackRef& track) override {
        auto rid = kuwo_rid(track);
        if (!rid) return rid.error();
        auto body = client_.body(protocol::lyrics(*rid), context);
        if (!body) return body.error();
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
        auto rid = kuwo_rid(track);
        if (!rid) {
            return std::move(rid).error();
        }
        constexpr std::array offered{domain::Quality::standard, domain::Quality::high, domain::Quality::lossless, domain::Quality::hires};
        auto chosen = domain::choose_quality(requirement, offered);
        if (!chosen) return chosen.error();
        auto result = resolve_one(context, track, requirement, *rid, *chosen);
        if (requirement.requested != domain::Quality::hires || requirement.policy != domain::QualityPolicy::allow_lower)
            return result;
        // 黑胶不存在或返回不支持的专有格式时，只有显式允许降级才继续普通档位。
        // 网络、限流和协议损坏错误直接返回，不以换档掩盖故障。
        for (const auto lower : {domain::Quality::lossless, domain::Quality::high, domain::Quality::standard}) {
            if (result) {
                if (result->completeness == domain::Completeness::preview ||
                    (result->actual_quality && *result->actual_quality >= lower)) return result;
            } else if (!quality_missing(result.error())) return result;
            auto next = resolve_one(context, track, requirement, *rid, lower);
            // 已得到完整的低档资源时，后续档位不存在不丢弃该结果。
            if (!next && result && quality_missing(next.error())) continue;
            if (next && result && next->completeness != domain::Completeness::preview &&
                next->actual_quality && result->actual_quality && *next->actual_quality < *result->actual_quality) continue;
            result = std::move(next);
        }
        return result;
    }

private:
    static bool quality_missing(const Error& error) {
        return error.code == ErrorCode::quality_unavailable ||
            (error.code == ErrorCode::upstream_failed && !error.retryable &&
             std::any_of(error.details.begin(), error.details.end(), [](const auto& d) {
                 return d.field == "kuwo_code" && (d.reason == "200" || d.reason == "-1");
             }));
    }
    domain::Result<ports::ResolvedAudio> resolve_one(const ports::ProviderContext& context,
        const domain::TrackRef& track, const domain::ResourceRequirement& requirement,
        std::string_view rid, domain::Quality chosen) {
        const auto br = chosen == domain::Quality::hires ? "23000kflac" : chosen == domain::Quality::lossless ? "2000kflac" :
                        chosen == domain::Quality::high ? "320kmp3" : "128kmp3";
        auto response = client_.fetch(protocol::audio(rid, client_id(), br), context);
        if (!response) {
            return std::move(response).error();
        }
        if (response->head.status != 200) {
            return http_status_error(response->head);
        }
        auto evidence = parsing::parse_audio(response->body, rid);
        if (!evidence) {
            if (chosen == domain::Quality::hires && quality_missing(evidence.error()))
                return Error{ErrorCode::quality_unavailable, "kuwo has no vinyl audio for this track", false, {}};
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
            } else if (evidence->format == "mp3" && evidence->bitrate_kbps == 320) {
                quality = domain::Quality::high;
            } else if (evidence->format == "flac" && evidence->bitrate_kbps && *evidence->bitrate_kbps > 0) {
                quality = *evidence->bitrate_kbps == 23000 ? domain::Quality::hires : domain::Quality::lossless;
            }
            if (!quality && requirement.requested != domain::Quality::standard)
                return Error{ErrorCode::quality_unavailable, "kuwo returned an unverified quality tier", false, {}};
            // Same rid, regular type and a positive duration: the upstream declares the
            // whole song. Without a readable type it stays unknown. Audio verification
            // (T15) checks the bytes.
            if (evidence->regular && evidence->duration_seconds && *evidence->duration_seconds > 0) {
                completeness = domain::Completeness::full;
            }
        }
        std::optional<std::int64_t> bitrate_bps;
        // FLAC 的 2000／23000 是档位编码，不是实际音频码率。
        if (evidence->format == "mp3" && evidence->bitrate_kbps && *evidence->bitrate_kbps >= 8 && *evidence->bitrate_kbps <= 320) {
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
        if (quality == domain::Quality::hires) audio.processing = {"audio.kuwo-flac", 1, {}};
        return audio;
    }

    // The "user" value the Kuwo client sends: a random installation-like id per call.
    static std::string client_id() {
        thread_local std::mt19937_64 engine{std::random_device{}()};
        return "C_APK_guanwang_" + std::to_string(engine() % 1000000000000ULL);
    }

    Client client_;
};

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

domain::Result<std::shared_ptr<ports::ILyrics>> make_lyrics(const ports::ProviderSetup& setup) {
    if (auto ready = require_upstream(setup); !ready) return ready.error();
    return std::shared_ptr<ports::ILyrics>{std::make_shared<Lyrics>(setup)};
}

} // namespace music_service::providers::kuwo
