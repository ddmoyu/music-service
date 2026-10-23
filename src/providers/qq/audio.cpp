#include "providers/qq/bundle.hpp"
#include "providers/qq/parsing.hpp"
#include "application/transport/url.hpp"
#include <array>
#include <random>
#include <set>

namespace music_service::providers::qq {
namespace {
using parsing::text;
using parsing::number;
Json detail(const Client& client, const ports::ProviderContext& c, const domain::TrackRef& ref, std::string_view op) {
    check_ref(ref);
    auto data = client.rpc(op, "music.pf_song_detail_svr", "get_song_detail_yqq", {{"song_mid", ref.id.value()}}, c);
    auto info = data.at("track_info");
    if (text(info, "mid") != ref.id.value()) throw Error{ErrorCode::not_found, "QQ song was not found", false, {}};
    return info;
}
class Audio final : public ports::ITrackDetail, public ports::ILyrics, public ports::IAssetResolver {
public:
    explicit Audio(const ports::ProviderSetup& s) : client_(s) {}
    domain::Result<domain::Track> track_detail(const ports::ProviderContext& c, const domain::TrackRef& ref) override {
        return checked([&] { return parsing::track(detail(client_, c, ref, "track_detail")); });
    }
    domain::Result<domain::TrackLyrics> lyrics(const ports::ProviderContext& c, const domain::TrackRef& ref) override {
        return checked([&] {
            check_ref(ref);
            auto doc = client_.get("lyrics", "/lyric/fcgi-bin/fcg_query_lyric_new.fcg?format=json&nobase64=1&outCharset=utf-8&songmid=" + ref.id.value(), c);
            domain::TrackLyrics out;
            auto lrc = text(doc, "lyric"), translated = text(doc, "trans");
            if (!lrc.empty()) out.lrc = std::move(lrc);
            if (!translated.empty()) out.translated_lrc = std::move(translated);
            out.unavailable = !out.lrc && !out.translated_lrc;
            return out;
        });
    }
    domain::Result<ports::ResolvedAudio> resolve_audio(const ports::ProviderContext& c, const domain::TrackRef& ref,
        const domain::ResourceRequirement& requirement) override {
        return checked([&] {
            using domain::Quality;
            if (requirement.requested == Quality::hires && requirement.policy == domain::QualityPolicy::strict)
                throw Error{ErrorCode::quality_unavailable, "QQ Hi-Res is not supported by this adapter", false, {}};
            auto info = detail(client_, c, ref, "resolve_audio");
            const auto track = parsing::track(info);
            const auto& file = info.at("file");
            const auto media = text(file, "media_mid", true);
            if (!mid(media)) throw std::invalid_argument{"media mid"};
            struct Format { Quality quality; const char* prefix; const char* extension; const char* size; std::int64_t bitrate; };
            constexpr std::array formats{
                Format{Quality::lossless, "F000", "flac", "size_flac", 0},
                Format{Quality::high, "M800", "mp3", "size_320mp3", 320000},
                Format{Quality::standard, "M500", "mp3", "size_128mp3", 128000}};
            Json names = Json::array(), mids = Json::array(), types = Json::array();
            std::vector<const Format*> choices;
            for (const auto& f : formats) {
                if (f.quality > requirement.requested || (f.quality != requirement.requested && requirement.policy == domain::QualityPolicy::strict)) continue;
                if (!file.contains(f.size) || number(file, f.size) == 0) continue;
                choices.push_back(&f); names.push_back(std::string{f.prefix} + media + "." + f.extension);
                mids.push_back(ref.id.value()); types.push_back(0);
            }
            if (choices.empty()) throw Error{ErrorCode::quality_unavailable, "QQ has no declared file for the requested quality", false, {}};
            auto uin = c.secret ? cookie_value(c.secret->reveal(), "uin") : "0";
            if (!decimal(uin)) uin = "0";
            std::random_device random;
            const auto guid = std::to_string(1000000000ULL + static_cast<std::uint64_t>(random()) % 9000000000ULL);
            auto data = client_.rpc("resolve_audio", "vkey.GetVkeyServer", "CgiGetVkey",
                {{"guid", guid}, {"songmid", mids}, {"songtype", types}, {"filename", names},
                 {"uin", uin}, {"loginflag", c.secret ? 1 : 0}, {"platform", "20"}}, c);
            if (number(data, "retcode") != 0) throw Error{ErrorCode::upstream_failed, "QQ audio resolution failed", false, {}};
            const auto& urls = data.at("midurlinfo");
            if (!urls.is_array() || urls.size() != choices.size()) throw std::invalid_argument{"audio result count"};
            std::set<std::string> seen;
            for (const auto& v : urls) {
                auto name = text(v, "filename", true);
                if (!seen.insert(name).second || std::find(names.begin(), names.end(), name) == names.end() || text(v, "songmid", true) != ref.id.value())
                    throw std::invalid_argument{"audio resource identity"};
            }
            bool denied = false;
            for (std::size_t i = 0; i < choices.size(); ++i) {
                for (const auto& v : urls) {
                    if (v.at("filename") != names[i]) continue;
                    auto status = number(v, "result");
                    if (status == 104003) denied = true;
                    else if (status != 0) throw Error{ErrorCode::upstream_failed, "QQ rejected an audio candidate", false, {{"qq_code", std::to_string(status)}}};
                    auto path = text(v, "purl");
                    if (status != 0 || path.empty()) continue;
                    auto expected = names[i].get<std::string>();
                    if (!(path == expected || path.starts_with(expected + "?")) || path.find('#') != path.npos || !text(v, "ekey").empty())
                        throw std::invalid_argument{"audio URL or encrypted resource"};
                    auto url = take(application::transport::Url::parse("https://ws.stream.qqmusic.qq.com/" + path));
                    if (url.host() != "ws.stream.qqmusic.qq.com" || !url.is_https()) throw std::invalid_argument{"audio origin"};
                    const auto& f = *choices[i];
                    ports::ResolvedAudio out{ref, f.quality, domain::Completeness::full, std::string{f.extension},
                        f.bitrate ? std::optional<std::int64_t>{f.bitrate} : std::nullopt, url.str(),
                        {{"User-Agent", std::string{user_agent}}}, {}, track.duration_ms, ports::ProcessingPlan{}};
                    if (data.contains("expiration")) {
                        const auto ttl = number(data, "expiration");
                        if (ttl == 0 || ttl > 7 * 86400) throw std::invalid_argument{"audio expiry"};
                        out.expires_at = std::chrono::system_clock::now() + std::chrono::seconds{ttl};
                    }
                    // URL 包含临时 vkey，仅供后端传输，不向客户端提供受控直链。
                    out.shareable = false;
                    return out;
                }
            }
            if (denied) throw Error{ErrorCode::credentials_required, "QQ account cannot access this audio quality", false, {{"qq_code", "104003"}}};
            throw Error{ErrorCode::not_found, "QQ did not provide a playable audio file", false, {}};
        });
    }
private: Client client_;
};
class Links final : public ports::ILinkParser {
public:
    domain::Result<ports::ParsedLink> parse_link(std::string_view link) override {
        auto url = application::transport::Url::parse(link);
        if (!url || url->host() != "y.qq.com" || url->port() != (url->is_https() ? 443 : 80) || link.find('#') != link.npos)
            return Error::validation("link", "not a supported QQ link");
        struct Route { std::string_view prefix; std::string_view kind; bool numeric; bool html; };
        constexpr std::array routes{Route{"/n/ryqq/songDetail/", "track", false, false},
            Route{"/n/ryqq/albumDetail/", "album", false, false}, Route{"/n/ryqq/playlist/", "playlist", true, false},
            Route{"/n/ryqq/singer/", "artist", false, false}, Route{"/n/yqq/song/", "track", false, true},
            Route{"/n/yqq/album/", "album", false, true}};
        for (const auto& route : routes) if (url->path().starts_with(route.prefix)) {
            auto id = url->path().substr(route.prefix.size());
            if (route.html) { if (!id.ends_with(".html")) break; id.remove_suffix(5); }
            if (!(route.numeric ? decimal(id) : mid(id))) break;
            // 页面定位依赖路径，身份查询参数会造成歧义，不能悄悄忽略。
            auto query = std::string_view{url->target()}.substr(url->path().size());
            if (query.find("songmid=") != query.npos || query.find("songid=") != query.npos ||
                query.find("disstid=") != query.npos || query.find("albummid=") != query.npos)
                return Error::validation("link", "ambiguous QQ entity link");
            return ports::ParsedLink{std::string{route.kind}, parsing::ref(std::string{id}, route.numeric)};
        }
        return Error::validation("link", "unsupported QQ entity link");
    }
};
class Verifier final : public ports::ICredentialVerifier {
public:
    explicit Verifier(const ports::ProviderSetup& s) : client_(s) {}
    domain::Result<ports::CredentialVerification> verify(const ports::CredentialSnapshot& snapshot, const ports::OperationContext& operation) override {
        return checked([&] {
            ports::CredentialVerification result;
            auto cookie = snapshot.secret.reveal();
            if (!decimal(cookie_value(cookie, "uin")) || (cookie_value(cookie, "qqmusic_key").empty() && cookie_value(cookie, "qm_keyst").empty())) {
                result.status = ports::VerificationStatus::invalid; return result;
            }
            auto secret = std::make_shared<ports::SecretBytes>(std::string{cookie});
            ports::ProviderContext c{operation, snapshot.identity, {}, std::move(secret)};
            Json data;
            try { data = client_.rpc("verify_credentials", "music.UserInfo.userInfoServer", "GetLoginUserInfo", Json::object(), c); }
            catch (const Error& e) {
                if (e.code != ErrorCode::credentials_required) throw;
                result.status = ports::VerificationStatus::invalid; return result;
            }
            auto info = data.at("info");
            result.status = number(info, "retCode") == 0 && !text(info, "nick").empty() ? ports::VerificationStatus::valid : ports::VerificationStatus::invalid;
            // 不把账号昵称或 UID 暴露为展示字段；成功登录也不推断会员和歌曲购买权。
            return result;
        });
    }
private: Client client_;
};
template<class Port, class Impl> void bind(ports::ProviderBundle& bundle) {
    bundle.bindings.push_back(ports::CapabilityBinding::bind<Port>([](const ports::ProviderSetup& s) -> domain::Result<std::shared_ptr<Port>> {
        if (!s.upstream) return Error{ErrorCode::internal, "QQ requires the upstream client", false, {}};
        return std::shared_ptr<Port>{std::make_shared<Impl>(s)};
    }));
}
}
void add_audio(ports::ProviderBundle& b) {
    bind<ports::ITrackDetail, Audio>(b); bind<ports::ILyrics, Audio>(b); bind<ports::IAssetResolver, Audio>(b);
    b.bindings.push_back(ports::CapabilityBinding::bind<ports::ILinkParser>([](const ports::ProviderSetup&) -> domain::Result<std::shared_ptr<ports::ILinkParser>> {
        return std::shared_ptr<ports::ILinkParser>{std::make_shared<Links>()};
    }));
    b.auth_profiles = {ports::AuthProfileSpec{.auth_profile = std::string{cookie_profile}, .kind = ports::CredentialKind::cookie,
        .version = 1, .input_fields = {{.name = "cookie", .required = true, .secret = true}},
        .verifier = [](const ports::ProviderSetup& s) -> domain::Result<std::shared_ptr<ports::ICredentialVerifier>> {
            if (!s.upstream) return Error{ErrorCode::internal, "QQ requires the upstream client", false, {}};
            return std::shared_ptr<ports::ICredentialVerifier>{std::make_shared<Verifier>(s)};
        }}};
}
}
