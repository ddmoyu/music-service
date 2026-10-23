#include "application/transport/url.hpp"
#include "providers/kugou/parsing.hpp"
#include <algorithm>
#include <array>
#include <mbedtls/base64.h>

namespace music_service::providers::kugou {
namespace {
using application::transport::encode_query_component;
using parsing::number;
using parsing::text;
Json detail(const Client& client, const ports::ProviderContext& c, std::string id,
            std::string_view op) {
    auto data =
        client.get(op, "https://m.kugou.com/app/i/getSongInfo.php?cmd=playInfo&hash=" + id, c);
    const auto code = number(data, "errcode");
    if (code == 30001)
        throw Error{ErrorCode::not_found, "Kugou song was not found", false, {}};
    if (code == 1002)
        throw Error{ErrorCode::upstream_rate_limited, "Kugou request rate limited", true, {}};
    if (code != 0)
        throw Error{ErrorCode::upstream_failed,
                    "Kugou song detail failed",
                    false,
                    {{"kugou_code", std::to_string(code)}}};
    if (lower(text(data, "hash", true)) != id || lower(text(data, "req_hash", true)) != id)
        throw std::invalid_argument{"audio hash identity"};
    return data;
}
std::string base64(std::string_view source) {
    if (source.size() > 1024 * 1024)
        throw std::invalid_argument{"lyrics size"};
    std::string out(source.size(), '\0');
    std::size_t size{};
    if (mbedtls_base64_decode(reinterpret_cast<unsigned char*>(out.data()), out.size(), &size,
                              reinterpret_cast<const unsigned char*>(source.data()),
                              source.size()) != 0)
        throw std::invalid_argument{"lyrics encoding"};
    out.resize(size);
    if (!domain::is_valid_utf8(out) || out.find('\0') != out.npos)
        throw std::invalid_argument{"lyrics UTF-8"};
    return out;
}
class Audio final : public ports::ITrackDetail,
                    public ports::ILyrics,
                    public ports::IAssetResolver {
  public:
    explicit Audio(const ports::ProviderSetup& s) : client_(s) {}
    domain::Result<domain::Track> track_detail(const ports::ProviderContext& c,
                                               const domain::TrackRef& ref) override {
        return checked([&] {
            check_ref(ref, true);
            auto doc = detail(client_, c, ref.id.value(), "track_detail");
            auto song = parsing::track(doc, true);
            const auto album_id = text(doc, "albumid");
            if (decimal(album_id)) {
                // 官方详情中 album_img 是专辑图；补充专辑标题，不使用歌手头像。
                try {
                    auto album = parsing::album(client_.api(
                        "track_detail",
                        "album/info?albumid=" + album_id + "&version=9108&area_code=1", c));
                    if (album.ref.id.value() != album_id)
                        throw std::invalid_argument{"album identity"};
                    song.album = domain::AlbumSummary{album.title, album.ref};
                    if (!song.artwork)
                        song.artwork = album.artwork;
                } catch (const Error& e) {
                    if (e.code != ErrorCode::not_found)
                        throw;
                }
            }
            return song;
        });
    }
    domain::Result<domain::TrackLyrics> lyrics(const ports::ProviderContext& c,
                                               const domain::TrackRef& ref) override {
        return checked([&] {
            check_ref(ref, true);
            auto doc = client_.get(
                "lyrics",
                "https://lyrics.kugou.com/search?ver=1&client=mobi&hash=" + ref.id.value(), c);
            if (number(doc, "status") != 200 || number(doc, "errcode") != 200)
                throw std::invalid_argument{"lyrics status"};
            const auto& candidates = doc.at("candidates");
            parsing::array(candidates, 100);
            domain::TrackLyrics out;
            if (candidates.empty()) {
                out.unavailable = true;
                return out;
            }
            const auto id = text(candidates.front(), "id", true),
                       key = text(candidates.front(), "accesskey", true);
            if (!decimal(id) || key.size() > 256)
                throw std::invalid_argument{"lyrics identity"};
            auto data = client_.get("lyrics",
                                    "https://lyrics.kugou.com/download?ver=1&client=pc&id=" + id +
                                        "&accesskey=" + encode_query_component(key) +
                                        "&fmt=lrc&charset=utf8",
                                    c);
            if (number(data, "status") != 200 || number(data, "error_code") != 0 ||
                text(data, "id", true) != id || text(data, "fmt", true) != "lrc")
                throw std::invalid_argument{"lyrics response identity"};
            auto lrc = base64(text(data, "content", true));
            if (!lrc.empty())
                out.lrc = std::move(lrc);
            out.unavailable = !out.lrc;
            return out;
        });
    }
    domain::Result<ports::ResolvedAudio>
    resolve_audio(const ports::ProviderContext& c, const domain::TrackRef& ref,
                  const domain::ResourceRequirement& requirement) override {
        return checked([&] {
            using domain::Quality;
            check_ref(ref, true);
            if (requirement.requested == Quality::hires &&
                requirement.policy == domain::QualityPolicy::strict)
                throw Error{ErrorCode::quality_unavailable,
                            "Kugou Hi-Res is not supported by this adapter",
                            false,
                            {}};
            const auto info = detail(client_, c, ref.id.value(), "resolve_audio");
            const auto& extra = info.at("extra");
            struct Format {
                Quality quality;
                const char* hash_key;
                const char* duration_key;
                const char* extension;
                std::int64_t bitrate;
            };
            constexpr std::array formats{
                Format{Quality::lossless, "sqhash", "sqtimelength", "flac", 0},
                Format{Quality::high, "320hash", "320timelength", "mp3", 320000},
                Format{Quality::standard, "128hash", "128timelength", "mp3", 128000}};
            bool denied = false, preview = false;
            for (const auto& format : formats) {
                if (format.quality > requirement.requested ||
                    (format.quality != requirement.requested &&
                     requirement.policy == domain::QualityPolicy::strict))
                    continue;
                const auto candidate = lower(text(extra, format.hash_key));
                if (candidate.empty() || candidate == "00000000000000000000000000000000")
                    continue;
                if (!hash(candidate))
                    throw std::invalid_argument{"quality hash"};
                auto data = candidate == ref.id.value()
                                ? info
                                : detail(client_, c, candidate, "resolve_audio");
                const auto status = number(data, "status");
                if (status == 0 && text(data, "url").empty() && number(data, "pay_type") > 0) {
                    denied = true;
                    continue;
                }
                if (status != 1)
                    throw Error{ErrorCode::upstream_failed,
                                "Kugou audio resolution was rejected",
                                false,
                                {}};
                if (number(data, "fileHead") != 0) {
                    // 已知不支持的加密候选不应阻断用户明确允许的普通品质回落。
                    if (requirement.policy == domain::QualityPolicy::allow_lower)
                        continue;
                    throw Error{ErrorCode::quality_unavailable,
                                "encrypted Kugou audio is not supported",
                                false,
                                {}};
                }
                auto ext = lower(text(data, "extName", true));
                auto bitrate = number(data, "bitRate");
                if (bitrate < 10000)
                    bitrate *= 1000;
                if (ext != format.extension ||
                    (format.bitrate && bitrate != static_cast<std::uint64_t>(format.bitrate)))
                    throw Error{ErrorCode::quality_unavailable,
                                "Kugou returned a different audio quality",
                                false,
                                {}};
                auto full_ms = number(extra, format.duration_key),
                     resource_seconds = number(data, "timeLength");
                if (!full_ms || full_ms > 86400000 || !resource_seconds ||
                    resource_seconds > 86400 || number(data, "fileSize") == 0)
                    throw std::invalid_argument{"audio duration or size"};
                // 使用独立的整曲时长判定；截取片段不冒充整曲，下载后再由公共处理器测量。
                const bool is_preview = resource_seconds * 1000 + 2000 < full_ms;
                if (is_preview && !requirement.allow_preview) {
                    preview = true;
                    continue;
                }
                auto url = application::transport::Url::parse(text(data, "url", true));
                if (!url || !url->is_https() || url->port() != 443 ||
                    !url->host().ends_with(".kugou.com"))
                    throw std::invalid_argument{"audio origin"};
                ports::ResolvedAudio out{
                    ref,
                    format.quality,
                    is_preview ? domain::Completeness::preview : domain::Completeness::full,
                    ext,
                    bitrate ? std::optional<std::int64_t>{static_cast<std::int64_t>(bitrate)}
                            : std::nullopt,
                    url->str(),
                    {{"User-Agent", std::string{user_agent}}},
                    {},
                    static_cast<std::int64_t>(full_ms),
                    ports::ProcessingPlan{}};
                out.shareable = false;
                return out;
            }
            if (denied)
                throw Error{ErrorCode::credentials_required,
                            "Kugou requires permission to access this audio quality",
                            false,
                            {}};
            if (preview)
                throw Error{
                    ErrorCode::preview_rejected, "Kugou only returned a preview", false, {}};
            throw Error{ErrorCode::quality_unavailable,
                        "Kugou has no file for the requested quality",
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
        auto url = application::transport::Url::parse(link);
        if (!url || (url->host() != "www.kugou.com" && url->host() != "m.kugou.com") ||
            url->port() != (url->is_https() ? 443 : 80))
            return Error::validation("link", "not a supported Kugou link");
        auto path = url->path();
        if (path == "/song/" || path == "/song") {
            std::string id;
            auto collect = [&](std::string_view params) {
                while (!params.empty()) {
                    auto sep = params.find('&');
                    auto pair = params.substr(0, sep);
                    params = sep == params.npos ? std::string_view{} : params.substr(sep + 1);
                    if (pair.starts_with("hash=")) {
                        if (!id.empty() || !hash(pair.substr(5)))
                            return false;
                        id = lower(std::string{pair.substr(5)});
                    } else if (!(pair.starts_with("album_id=") && decimal(pair.substr(9))))
                        return false;
                }
                return true;
            };
            auto query = std::string_view{url->target()}.substr(path.size());
            if (!query.empty())
                query.remove_prefix(1);
            const auto fragment = link.find('#');
            if (!collect(query) || (fragment != link.npos && !collect(link.substr(fragment + 1))) ||
                id.empty())
                return Error::validation("link", "ambiguous or invalid Kugou song hash");
            return ports::ParsedLink{"track", parsing::ref(id, true)};
        }
        if (link.find('#') != link.npos || url->target().find('?') != url->target().npos)
            return Error::validation("link", "ambiguous Kugou collection link");
        struct Route {
            std::string_view prefix;
            std::string_view kind;
            std::string_view suffix;
        };
        constexpr std::array routes{Route{"/yy/special/single/", "playlist", ".html"},
                                    Route{"/album/", "album", ".html"},
                                    Route{"/yy/album/single/", "album", ".html"},
                                    Route{"/album/single/", "album", ".html"},
                                    Route{"/plist/list/", "playlist", ""},
                                    Route{"/singer/info/", "artist", ""},
                                    Route{"/yy/singer/home/", "artist", ".html"}};
        for (const auto& route : routes)
            if (path.starts_with(route.prefix) && path.ends_with(route.suffix)) {
                auto id = path.substr(route.prefix.size(),
                                      path.size() - route.prefix.size() - route.suffix.size());
                if (decimal(id))
                    return ports::ParsedLink{std::string{route.kind},
                                             parsing::ref(std::string{id})};
            }
        return Error::validation("link", "unsupported Kugou entity link");
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
} // namespace music_service::providers::kugou
