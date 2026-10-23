#include "providers/qianqian/parsing.hpp"
#include "application/transport/url.hpp"
#include <array>

namespace music_service::providers::qianqian {
namespace {
using parsing::text;
using parsing::number;
class Audio final : public ports::ITrackDetail, public ports::ILyrics, public ports::IAssetResolver {
public:
    explicit Audio(const ports::ProviderSetup& setup) : client_(setup) {}
    domain::Result<domain::Track> track_detail(const ports::ProviderContext& c, const domain::TrackRef& ref) override {
        return checked([&] { return parsing::track(parsing::song(client_, c, ref, "track_detail")); });
    }
    domain::Result<domain::TrackLyrics> lyrics(const ports::ProviderContext& c, const domain::TrackRef& ref) override {
        return checked([&] {
            auto info = parsing::song(client_, c, ref, "lyrics");
            auto url = parsing::resource_url(text(info, "lyric"));
            domain::TrackLyrics out;
            if (!url.empty()) {
                auto lrc = client_.lyrics(std::move(url), c);
                if (!lrc.empty()) out.lrc = std::move(lrc);
            }
            out.unavailable = !out.lrc;
            return out;
        });
    }
    domain::Result<ports::ResolvedAudio> resolve_audio(const ports::ProviderContext& c, const domain::TrackRef& ref,
        const domain::ResourceRequirement& requirement) override {
        return checked([&] {
            check_ref(ref, 'T');
            using domain::Quality;
            if (requirement.requested == Quality::hires && requirement.policy == domain::QualityPolicy::strict)
                throw Error{ErrorCode::quality_unavailable, "Qianqian Hi-Res is not supported by this adapter", false, {}};
            struct Choice { Quality quality; const char* rate; };
            constexpr std::array choices{Choice{Quality::lossless, "3000"}, Choice{Quality::high, "320"}, Choice{Quality::standard, "128"}};
            bool denied = false;
            bool preview_rejected = false;
            std::optional<ports::ResolvedAudio> best;
            for (const auto& choice : choices) {
                if (choice.quality > requirement.requested || (choice.quality != requirement.requested && requirement.policy == domain::QualityPolicy::strict)) continue;
                auto info = client_.get("resolve_audio", "song/tracklink", {{"TSID", ref.id.value()}, {"rate", choice.rate}}, c);
                auto track = parsing::track(info);
                if (track.ref != ref) throw std::invalid_argument{"audio response identity"};
                auto path = text(info, "path");
                bool preview = false;
                Json file = info;
                if (path.empty() && info.contains("trail_audio_info") && info.at("trail_audio_info").is_object()) {
                    file = info.at("trail_audio_info");
                    path = text(file, "path");
                    preview = !path.empty();
                }
                if (path.empty()) {
                    if ((info.contains("isVip") && number(info, "isVip", 1)) ||
                        (info.contains("pay_model") && number(info, "pay_model", 100) == 3)) denied = true;
                    continue;
                }
                if (preview && !requirement.allow_preview) { preview_rejected = true; continue; }
                const auto format = text(file, "format", true);
                const auto rate = number(file, "rate", 100000);
                std::optional<Quality> quality;
                if (format == "flac" && rate > 0) quality = Quality::lossless;
                else if (format == "mp3" && rate >= 320) quality = Quality::high;
                else if (format == "mp3" && rate >= 128) quality = Quality::standard;
                else throw std::invalid_argument{"unsupported audio format or rate"};
                // 以返回字段为准；请求 3000 并不证明收到 FLAC，strict 禁止静默降档。
                if (*quality > requirement.requested || (*quality != requirement.requested && requirement.policy == domain::QualityPolicy::strict)) continue;
                const auto now = std::chrono::system_clock::now();
                std::optional<std::chrono::system_clock::time_point> expires;
                // 试听有独立有效期时必须与选中的 path 配对；未提供才用外层期限。
                const auto& expiry = file.contains("expireTime") && !file.at("expireTime").is_null() ? file : info;
                if (expiry.contains("expireTime") && !expiry.at("expireTime").is_null()) {
                    auto seconds = number(expiry, "expireTime", 4102444800ULL);
                    expires = std::chrono::system_clock::time_point{std::chrono::seconds{seconds}};
                    if (*expires <= now) throw Error{ErrorCode::resource_expired, "Qianqian audio URL has expired", true, {}};
                }
                std::optional<std::int64_t> duration = track.duration_ms;
                if (preview) {
                    duration.reset();
                    if (file.contains("duration")) duration = static_cast<std::int64_t>(number(file, "duration", 86400) * 1000);
                }
                ports::ResolvedAudio candidate{ref, quality, preview ? domain::Completeness::preview : domain::Completeness::full,
                    format, static_cast<std::int64_t>(rate * 1000), parsing::resource_url(path, true),
                    {{"User-Agent", std::string{user_agent}}}, expires, duration, ports::ProcessingPlan{}};
                // 高档端点可能直接回低档；继续核对中间档，不能跳过实际可用的更高候选。
                if (!best || candidate.actual_quality > best->actual_quality) best = std::move(candidate);
                if (*quality == choice.quality) return *best;
            }
            if (best) return *best;
            if (preview_rejected) throw Error{ErrorCode::preview_rejected, "Qianqian only supplied preview audio", false, {}};
            if (denied) throw Error{ErrorCode::credentials_required, "Qianqian account access is required for this audio", false, {}};
            throw Error{ErrorCode::quality_unavailable, "Qianqian did not provide the requested audio quality", false, {}};
        });
    }
private: Client client_;
};
class Links final : public ports::ILinkParser {
public:
    domain::Result<ports::ParsedLink> parse_link(std::string_view link) override {
        return checked([&] {
            const auto url = application::transport::Url::parse(link);
            if (!url || url->host() != "music.91q.com" || url->port() != (url->is_https() ? 443 : 80) ||
                link.find('#') != link.npos || url->target().find('?') != std::string::npos)
                throw Error::validation("link", "not a supported Qianqian link");
            struct Route { std::string_view path; std::string_view kind; char prefix; };
            constexpr std::array routes{Route{"/song/", "track", 'T'}, Route{"/album/", "album", 'P'}, Route{"/songlist/", "playlist", '\0'}};
            for (const auto& route : routes) {
                if (!url->path().starts_with(route.path)) continue;
                auto id = url->path().substr(route.path.size());
                if (id.ends_with('/')) id.remove_suffix(1);
                if (!identifier(id, route.prefix)) break;
                return ports::ParsedLink{std::string{route.kind}, parsing::ref(std::string{id}, route.prefix)};
            }
            throw Error::validation("link", "unsupported Qianqian entity link");
        });
    }
};
}
void add_audio(ports::ProviderBundle& bundle) {
    bind<ports::ITrackDetail, Audio>(bundle);
    bind<ports::ILyrics, Audio>(bundle);
    bind<ports::IAssetResolver, Audio>(bundle);
    bundle.bindings.push_back(ports::CapabilityBinding::bind<ports::ILinkParser>([](const ports::ProviderSetup&) -> domain::Result<std::shared_ptr<ports::ILinkParser>> {
        return std::shared_ptr<ports::ILinkParser>{std::make_shared<Links>()};
    }));
}
}
