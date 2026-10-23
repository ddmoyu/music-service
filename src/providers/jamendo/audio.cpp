#include "providers/jamendo/parsing.hpp"
#include "application/transport/url.hpp"
#include <array>

namespace music_service::providers::jamendo {
namespace {
class Audio final : public ports::ITrackDetail, public ports::ILyrics, public ports::IAssetResolver {
public:
    explicit Audio(const ports::ProviderSetup& setup) : client_(setup) {}
    domain::Result<domain::Track> track_detail(const ports::ProviderContext& context, const domain::TrackRef& ref) override {
        return checked([&] {
            auto row = client_.one("track_detail", "tracks", ref, context);
            return parsing::tracks(client_, context, "track_detail", {row}).front();
        });
    }
    domain::Result<domain::TrackLyrics> lyrics(const ports::ProviderContext& context, const domain::TrackRef& ref) override {
        return checked([&] {
            auto row = client_.one("lyrics", "tracks", ref, context);
            domain::TrackLyrics result;
            auto value = parsing::text(row, "lyrics");
            if (!value.empty()) result.lrc = std::move(value); // 纯文本保留原样，不伪造时间轴。
            result.unavailable = !result.lrc;
            return result;
        });
    }
    domain::Result<ports::ResolvedAudio> resolve_audio(const ports::ProviderContext& context, const domain::TrackRef& ref,
        const domain::ResourceRequirement& requirement) override {
        return checked([&] {
            check_ref(ref);
            constexpr std::array offered{domain::Quality::standard};
            const auto quality = take(domain::choose_quality(requirement, offered));
            auto row = client_.one("resolve_audio", "tracks", ref, context);
            const auto permission = row.find("isDownloadable");
            const bool allowed = permission != row.end() && (permission->is_boolean() ? permission->get<bool>() :
                parsing::number(row, "isDownloadable") == 1);
            if (!allowed) throw Error{ErrorCode::not_found, "Jamendo has not permitted downloading this track", false,
                                       {{"download", "not_allowed"}}};
            const auto download = row.find("download");
            if (download == row.end() || !download->is_object())
                throw Error{ErrorCode::not_found, "Jamendo did not supply a download resource", false, {}};
            auto address = parsing::text(*download, "mp3");
            if (address.empty()) address = parsing::text(*download, "mp32");
            if (address.empty()) throw Error{ErrorCode::quality_unavailable, "Jamendo MP3 download is unavailable", false, {}};
            auto url = application::transport::Url::parse(address);
            // download 与 stream 权限不同，不拼装高档地址，也不回退到 stream。
            const auto expected = "/download/track/" + ref.id.value() + "/mp32/";
            if (!url || !url->is_https() || url->port() != 443 || !url->host().ends_with(".storage.jamendo.com") ||
                url->path() != expected || url->target() != expected || address.find('#') != address.npos)
                throw std::invalid_argument{"download identity or origin"};
            const auto track = parsing::track(row);
            // mp32 是 VBR 编码，码率保持未知，最终由公共音频验证器逐帧实测。
            return ports::ResolvedAudio{ref, quality, domain::Completeness::full, std::string{"mp3"}, {}, url->str(),
                {{"User-Agent", std::string{user_agent}}}, {}, track.duration_ms, ports::ProcessingPlan{}};
        });
    }
private: Client client_;
};
class Links final : public ports::ILinkParser {
public:
    domain::Result<ports::ParsedLink> parse_link(std::string_view link) override {
        return checked([&] {
            auto url = application::transport::Url::parse(link);
            if (!url || (url->host() != "www.jamendo.com" && url->host() != "jamendo.com" && url->host() != "jamen.do") ||
                url->port() != (url->is_https() ? 443 : 80) || link.find('#') != link.npos || url->target() != url->path())
                throw Error::validation("link", "not a supported Jamendo link");
            auto path = url->path();
            if (url->host() != "jamen.do") {
                for (const auto lang : {"/en/", "/fr/", "/de/", "/es/", "/it/", "/pl/", "/pt/", "/ru/", "/ja/"})
                    if (path.starts_with(lang)) { path.remove_prefix(3); break; }
            }
            for (const auto kind : {"track", "album", "artist", "playlist"}) {
                const auto prefix = url->host() == "jamen.do" ? "/" + std::string(1, kind[0]) + "/" : "/" + std::string{kind} + "/";
                // 短链只认身份明确的 /t/，其余交由调用方使用官网完整地址。
                if (url->host() == "jamen.do" && std::string_view{kind} != "track") continue;
                if (!path.starts_with(prefix)) continue;
                auto tail = path.substr(prefix.size());
                const auto slash = tail.find('/');
                auto value = tail.substr(0, slash);
                if (!decimal(value)) break;
                if (slash != tail.npos) {
                    auto slug = tail.substr(slash + 1);
                    if (slug.find('/') != slug.npos || slug == "." || slug == ".." || slug.find('%') != slug.npos) break;
                }
                return ports::ParsedLink{kind, parsing::ref(std::string{value})};
            }
            throw Error::validation("link", "unsupported Jamendo entity link");
        });
    }
};
}
void add_audio(ports::ProviderBundle& bundle) {
    bind<ports::ITrackDetail, Audio>(bundle);
    bind<ports::ILyrics, Audio>(bundle);
    bind<ports::IAssetResolver, Audio>(bundle);
    bundle.bindings.push_back(ports::CapabilityBinding::bind<ports::ILinkParser>([](const ports::ProviderSetup&)
        -> domain::Result<std::shared_ptr<ports::ILinkParser>> { return std::shared_ptr<ports::ILinkParser>{std::make_shared<Links>()}; }));
}
}
