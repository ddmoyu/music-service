#include "application/transport/url.hpp"
#include "providers/migu/bundle.hpp"
#include "providers/migu/parsing.hpp"
#include <algorithm>
#include <array>
#include <map>

namespace music_service::providers::migu {
namespace {
using parsing::text;
std::string media_url(std::string value) {
    auto u = application::transport::Url::parse(value);
    if (!u || !u->is_https() || u->port() != 443 || !u->host().ends_with(".migu.cn") ||
        value.find('#') != value.npos)
        throw std::invalid_argument{"media origin"};
    return u->str();
}
class Audio final : public ports::ITrackDetail,
                    public ports::ILyrics,
                    public ports::IAssetResolver {
  public:
    explicit Audio(const ports::ProviderSetup& s) : client_(s) {}
    domain::Result<domain::Track> track_detail(const ports::ProviderContext& c,
                                               const domain::TrackRef& ref) override {
        return checked(
            [&] { return parsing::track(client_.resource("track_detail", ref, "2", c)); });
    }
    domain::Result<domain::TrackLyrics> lyrics(const ports::ProviderContext& c,
                                               const domain::TrackRef& ref) override {
        return checked([&] {
            const auto doc = client_.resource("lyrics", ref, "2", c);
            auto url = parsing::first(doc, {"lrcUrl", "lyricUrl"});
            domain::TrackLyrics out;
            if (!url.empty()) {
                if (url.starts_with("http://"))
                    url.replace(0, 7, "https://");
                auto body = client_.lyrics(media_url(url), c);
                if (!domain::is_valid_utf8(body) ||
                    (!body.empty() && (body.front() == '<' || body.front() == '{')))
                    throw std::invalid_argument{"lyric encoding or error document"};
                if (!body.empty())
                    out.lrc = std::move(body);
            }
            out.unavailable = !out.lrc;
            return out;
        });
    }
    domain::Result<ports::ResolvedAudio>
    resolve_audio(const ports::ProviderContext& c, const domain::TrackRef& ref,
                  const domain::ResourceRequirement& req) override {
        return checked([&] {
            using domain::Quality;
            check_ref(ref);
            if (req.requested == Quality::hires && req.policy == domain::QualityPolicy::strict)
                throw Error{ErrorCode::quality_unavailable,
                            "Migu Hi-Res has not been validated",
                            false,
                            {}};
            const auto info = client_.resource("resolve_audio", ref, "2", c);
            const auto copyright = text(info, "copyrightId", true),
                       song = text(info, "songId", true);
            if (!decimal(song) || copyright.size() > 32 ||
                copyright.find_first_not_of(
                    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz") !=
                    copyright.npos)
                throw std::invalid_argument{"audio identifiers"};
            struct Format {
                Quality quality;
                const char* tone;
                const char* container;
                std::int64_t bitrate;
            };
            constexpr std::array formats{Format{Quality::lossless, "SQ", "flac", 0},
                                         Format{Quality::high, "HQ", "mp3", 320000},
                                         Format{Quality::standard, "PQ", "mp3", 128000}};
            bool denied = false, encrypted = false;
            for (const auto& format : formats) {
                if (format.quality > req.requested ||
                    (req.policy == domain::QualityPolicy::strict &&
                     format.quality != req.requested))
                    continue;
                auto doc = client_
                               .get("resolve_audio",
                                    "https://c.musicapp.migu.cn/MIGUM2.0/v2.1/content/"
                                    "listen-url?netType=01&resourceType=2&contentId=" +
                                        ref.id.value() + "&copyrightId=" + copyright +
                                        "&songId=" + song + "&toneFlag=" + format.tone,
                                    c, true)
                               .at("data");
                if (auto cannot = parsing::number(doc, "cannotCode"); cannot && *cannot) {
                    if (*cannot == 440003) {
                        denied = true;
                        continue;
                    }
                    throw Error{ErrorCode::upstream_failed,
                                "Migu refused playback",
                                false,
                                {{"migu_code", std::to_string(*cannot)}}};
                }
                const auto url = text(doc, "url", true), actual = text(doc, "formatType", true);
                const bool z3d = actual == "Z3D";
                const auto found = std::find_if(formats.begin(), formats.end(),
                                                [&](const auto& f) { return actual == f.tone; });
                if (!z3d && found == formats.end())
                    throw Error{ErrorCode::quality_unavailable,
                                "Migu returned an unsupported audio format",
                                false,
                                {}};
                if (!z3d && (found->quality > req.requested ||
                    (req.policy == domain::QualityPolicy::strict &&
                     found->quality != req.requested)))
                    throw Error{ErrorCode::quality_unavailable,
                                "Migu returned a different audio quality",
                                false,
                                {}};
                const auto& resource = doc.at("songItem");
                if (text(resource, "contentId", true) != ref.id.value() ||
                    text(resource, "copyrightId", true) != copyright ||
                    text(resource, "songId", true) != song)
                    throw std::invalid_argument{"audio identity"};
                std::shared_ptr<const ports::SecretBytes> decryption_key;
                if (z3d) {
                    const auto& code = resource.at("z3dCode");
                    const auto key = text(code, "androidFileKey", true);
                    if (text(code, "formatType", true) != "Z3D" ||
                        text(code, "androidFileType", true) != "wav" || key.size() != 32 ||
                        key.find_first_not_of("0123456789abcdefABCDEF") != key.npos)
                        throw std::invalid_argument{"Z3D Android key or format"};
                    decryption_key = std::make_shared<ports::SecretBytes>(key);
                } else if (text(resource, "needEncrypt", true) != "0") {
                    // strict 只尝试一档；allow_lower 继续查询较低档位的官方权限端点。
                    encrypted = true;
                    continue;
                }
                const auto track = parsing::track(resource);
                const auto length = parsing::number(doc, "auditionsLength"),
                           item_length = parsing::number(resource, "auditionsLength");
                if ((length && *length > 86400) || (item_length && *item_length > 86400))
                    throw std::invalid_argument{"preview duration limit"};
                const auto preview_length = length && *length ? length : item_length;
                auto completeness = domain::Completeness::unknown;
                if ((length && *length) || (item_length && *item_length))
                    completeness = domain::Completeness::preview;
                else if (text(resource, "auditionsType") == "00" &&
                         text(resource, "chargeAuditions") == "0")
                    completeness = domain::Completeness::full;
                else if (text(resource, "auditionsType") == "03" ||
                         text(resource, "auditionsType") == "04")
                    completeness = domain::Completeness::preview;
                auto duration = track.duration_ms;
                if (completeness == domain::Completeness::preview) {
                    // 整曲元数据不能当作片段时长；0 表示没有有效的试听时长证据。
                    duration =
                        preview_length && *preview_length
                            ? std::optional{static_cast<std::int64_t>(*preview_length * 1000)}
                            : std::nullopt;
                }
                // 仅使用权限端点给出的原地址，不改路径尝试更高品质；Cookie 不交给 CDN。
                return ports::ResolvedAudio{
                    ref,
                    z3d ? std::nullopt : std::optional{found->quality},
                    completeness,
                    std::string{z3d ? "wav" : found->container},
                    !z3d && found->bitrate ? std::optional<std::int64_t>{found->bitrate} : std::nullopt,
                    media_url(url),
                    {{"User-Agent", std::string{user_agent}},
                     {"Referer", "https://music.migu.cn/"}},
                    {},
                    duration,
                    z3d ? ports::ProcessingPlan{"audio.migu-z3d", 1, "migu.z3d"}
                        : ports::ProcessingPlan{},
                    false,
                    std::move(decryption_key)};
            }
            if (encrypted && !denied)
                throw Error{ErrorCode::quality_unavailable,
                            "Migu returned an unsupported encrypted audio format",
                            false,
                            {}};
            throw Error{denied ? ErrorCode::credentials_required : ErrorCode::quality_unavailable,
                        "Migu account cannot access the requested audio quality",
                        false,
                        {}};
        });
    }

  private:
    Client client_;
};
class Links final : public ports::ILinkParser {
  public:
    domain::Result<ports::ParsedLink> parse_link(std::string_view link) override {
        return checked([&]() -> ports::ParsedLink {
            auto u = application::transport::Url::parse(link);
            const auto invalid = [] {
                return Error::validation("link", "not a supported unambiguous Migu entity link");
            };
            if (!u || u->host() != "music.migu.cn" || u->port() != (u->is_https() ? 443 : 80) ||
                link.find('#') != link.npos)
                throw invalid();
            std::map<std::string, std::string> query;
            auto rest = std::string_view{u->target()}.substr(u->path().size());
            if (!rest.empty())
                rest.remove_prefix(1);
            while (!rest.empty()) {
                const auto end = rest.find('&');
                const auto pair = rest.substr(0, end);
                const auto equals = pair.find('=');
                if (equals == pair.npos || !query
                                                .emplace(std::string{pair.substr(0, equals)},
                                                         std::string{pair.substr(equals + 1)})
                                                .second)
                    throw invalid();
                rest = end == rest.npos ? std::string_view{} : rest.substr(end + 1);
            }
            const auto path = u->path();
            std::string kind, id;
            // 旧 song 路径通常是版权 ID；只接受明确的 18 位内容 ID，不混用。
            for (auto version : {"/v3/music/", "/v5/music/"})
                for (auto entity : {"song", "playlist", "album"}) {
                    auto prefix = std::string{version} + entity + "/";
                    if (!path.starts_with(prefix))
                        continue;
                    id = path.substr(prefix.size());
                    kind = std::string_view{entity} == "song" ? "track" : entity;
                    if (!query.empty() || ((kind == "track") && id.size() != 18) ||
                        (kind == "album" && id.size() == 18))
                        throw invalid();
                }
            if (path == "/v5/albumDetail") {
                if (query.size() != 2 || !query.contains("albumId") ||
                    !query.contains("playlistType"))
                    throw invalid();
                id = query.at("albumId");
                kind = "album";
                if (query.at("playlistType") == "5")
                    id = "d:" + id;
                else if (query.at("playlistType") != "2003")
                    throw invalid();
            } else if (path == "/v5/playlist") {
                if (!query.contains("playlistId") || query.size() > 2 ||
                    (query.size() == 2 && !query.contains("playlistType")))
                    throw invalid();
                id = query.at("playlistId");
                kind = "playlist";
            }
            std::string_view raw = id;
            if (kind == "album" && raw.starts_with("d:"))
                raw.remove_prefix(2);
            if (kind.empty() || !decimal(raw))
                throw invalid();
            return {kind, parsing::ref(id, kind == "album")};
        });
    }
};
} // namespace
void add_audio(ports::ProviderBundle& b) {
    bind<ports::ITrackDetail, Audio>(b);
    bind<ports::ILyrics, Audio>(b);
    bind<ports::IAssetResolver, Audio>(b);
    b.bindings.push_back(ports::CapabilityBinding::bind<ports::ILinkParser>(
        [](const ports::ProviderSetup&) -> domain::Result<std::shared_ptr<ports::ILinkParser>> {
            return std::shared_ptr<ports::ILinkParser>{std::make_shared<Links>()};
        }));
}
} // namespace music_service::providers::migu
