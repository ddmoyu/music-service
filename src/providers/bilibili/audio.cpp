#include "providers/bilibili/internal.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>

namespace music_service::providers::bilibili {
namespace {
application::transport::Url audio_url(const std::string& value) {
    auto url = application::transport::Url::parse(value);
    // CDN 地址是受控平台响应；仍拒绝外站、IP、用户信息和明文降级。
    if (!url || !url->is_https() || url->port() != 443 ||
        (!url->host().ends_with(".bilivideo.com") && !url->host().ends_with(".bilivideo.cn") &&
         !url->host().ends_with(".bilivideo.net")))
        throw std::invalid_argument{"audio origin"};
    return *url;
}
std::optional<std::chrono::system_clock::time_point>
expiry(const application::transport::Url& url) {
    auto query = std::string_view{url.target()}.substr(url.path().size());
    if (!query.empty())
        query.remove_prefix(1);
    std::optional<std::chrono::system_clock::time_point> result;
    while (!query.empty()) {
        const auto at = query.find('&');
        const auto part = query.substr(0, at);
        query = at == query.npos ? std::string_view{} : query.substr(at + 1);
        if (!part.starts_with("deadline="))
            continue;
        if (result)
            throw std::invalid_argument{"duplicate deadline"};
        const auto seconds = decimal(part.substr(9), 4102444800ULL);
        result = std::chrono::system_clock::time_point{std::chrono::seconds{seconds}};
        if (*result <= std::chrono::system_clock::now() + std::chrono::seconds{30})
            throw Error{ErrorCode::resource_expired,
                        "Bilibili returned an expired audio address",
                        true,
                        {}};
    }
    return result;
}
bool flagged(const Json& data, const char* key) {
    if (!data.contains(key))
        return false;
    if (data.at(key).is_boolean())
        return data.at(key).get<bool>();
    return integer(data.at(key), 0, 1) == 1;
}

class Audio final : public ports::IAssetResolver {
  public:
    explicit Audio(const ports::ProviderSetup& setup) : client_(setup) {}
    domain::Result<ports::ResolvedAudio>
    resolve_audio(const ports::ProviderContext& context, const domain::TrackRef& ref,
                  const domain::ResourceRequirement& requirement) override {
        return checked([&] {
            (void)parse_id(ref);
            if (requirement.requested != domain::Quality::standard &&
                requirement.policy == domain::QualityPolicy::strict)
                throw Error{ErrorCode::quality_unavailable,
                            "Bilibili currently supports standard AAC audio",
                            false,
                            {}};
            const auto part = view(client_, context, ref, "resolve_audio");
            const auto doc = client_.get("resolve_audio",
                                         "/x/player/playurl?bvid=" + part.bvid + "&cid=" +
                                             std::to_string(part.cid) + "&fnval=16&fnver=0&fourk=0",
                                         context);
            const auto& data = doc.at("data");
            // 某些响应会回显身份；若有就必须与本次分 P 完全相同。
            if ((data.contains("bvid") && text(data, "bvid") != part.bvid) ||
                (data.contains("cid") &&
                 static_cast<std::uint64_t>(integer(data.at("cid"), 1, INT64_MAX)) != part.cid))
                throw std::invalid_argument{"audio identity"};
            if (!data.contains("dash") || !data.at("dash").is_object())
                throw Error{ErrorCode::capability_unsupported,
                            "Bilibili did not provide an independent DASH audio track",
                            false,
                            {}};
            const auto& dash = data.at("dash");
            if (!dash.contains("audio") || dash.at("audio").is_null() || dash.at("audio").empty())
                throw Error{
                    ErrorCode::not_found, "Bilibili has no independent audio track", false, {}};
            const auto& rows = dash.at("audio");
            if (!rows.is_array() || rows.size() > 32)
                throw std::invalid_argument{"audio count"};
            const Json* selected = nullptr;
            std::int64_t bandwidth = 0;
            for (const auto& row : rows) {
                const auto mime = text(row, row.contains("mimeType") ? "mimeType" : "mime_type");
                const auto codec = text(row, "codecs");
                const auto id = integer(row.at("id"), 1, INT32_MAX);
                if (mime != "audio/mp4" ||
                    (codec != "mp4a.40.2" && codec != "mp4a.40.5" && codec != "mp4a.40.29") ||
                    (id != 30216 && id != 30232 && id != 30280))
                    continue;
                const auto rate = integer(row.at("bandwidth"), 1, 2000000);
                if (!selected || rate > bandwidth) {
                    selected = &row;
                    bandwidth = rate;
                }
            }
            if (!selected)
                throw Error{ErrorCode::quality_unavailable,
                            "Bilibili has no supported AAC audio track",
                            false,
                            {}};
            const auto value =
                text(*selected, selected->contains("baseUrl") ? "baseUrl" : "base_url");
            const auto url = audio_url(value);
            const auto expires = expiry(url);
            const auto duration = integer(data.at("timelength"), 1, 7LL * 86400 * 1000);
            auto completeness = domain::Completeness::unknown;
            const bool preview =
                part.paid_preview || flagged(data, "is_preview") || flagged(data, "isPreview");
            if (preview)
                completeness = domain::Completeness::preview;
            else if (part.track.duration_ms) {
                const auto expected = *part.track.duration_ms;
                const auto tolerance = std::max<std::int64_t>(2000, expected / 100);
                if (duration + tolerance < expected)
                    completeness = domain::Completeness::preview;
                else if (std::abs(duration - expected) > tolerance)
                    throw std::invalid_argument{"part duration"};
                else
                    completeness = domain::Completeness::full;
            }
            if (completeness == domain::Completeness::preview && !requirement.allow_preview)
                throw Error{ErrorCode::preview_rejected,
                            "Bilibili returned a preview or paid video",
                            false,
                            {}};
            // 使用完整单 URL 的音频文件，不跟随 durl 视频或拼接分片。
            return ports::ResolvedAudio{
                part.track.ref,
                domain::Quality::standard,
                completeness,
                "m4a",
                bandwidth,
                url.str(),
                {{"User-Agent", std::string{user_agent}},
                 {"Referer", "https://www.bilibili.com/video/" + part.bvid + "/"}},
                expires,
                duration,
                {},
                false};
        });
    }

  private:
    Client client_;
};

class Verifier final : public ports::ICredentialVerifier {
  public:
    explicit Verifier(const ports::ProviderSetup& setup) : client_(setup) {}
    domain::Result<ports::CredentialVerification>
    verify(const ports::CredentialSnapshot& snapshot,
           const ports::OperationContext& operation) override {
        return checked([&] {
            ports::CredentialVerification result;
            if (cookie_value(snapshot.secret.reveal(), "SESSDATA").empty()) {
                result.status = ports::VerificationStatus::invalid;
                return result;
            }
            ports::ProviderContext context{
                operation,
                snapshot.identity,
                {},
                std::make_shared<ports::SecretBytes>(std::string{snapshot.secret.reveal()})};
            const auto doc =
                client_.get("verify_credentials", "/x/web-interface/nav", context, true);
            if (!doc.at("data").at("isLogin").is_boolean())
                throw std::invalid_argument{"login status"};
            const bool valid = doc.at("code") == 0 && doc.at("data").at("isLogin").get<bool>();
            result.status =
                valid ? ports::VerificationStatus::valid : ports::VerificationStatus::invalid;
            // 不暴露昵称／UID，不由登录状态推断大会员或具体视频权限。
            return result;
        });
    }

  private:
    Client client_;
};
} // namespace

void add_audio(ports::ProviderBundle& bundle) {
    bundle.bindings.push_back(ports::CapabilityBinding::bind<ports::IAssetResolver>(
        [](const ports::ProviderSetup& setup)
            -> domain::Result<std::shared_ptr<ports::IAssetResolver>> {
            if (!setup.upstream)
                return Error{
                    ErrorCode::internal, "Bilibili requires the upstream client", false, {}};
            return std::shared_ptr<ports::IAssetResolver>{std::make_shared<Audio>(setup)};
        }));
    bundle.auth_profiles = {ports::AuthProfileSpec{
        .auth_profile = std::string{cookie_profile},
        .kind = ports::CredentialKind::cookie,
        .version = 1,
        .input_fields = {{.name = "cookie", .required = true, .secret = true}},
        .verifier = [](const ports::ProviderSetup& setup)
            -> domain::Result<std::shared_ptr<ports::ICredentialVerifier>> {
            if (!setup.upstream)
                return Error{
                    ErrorCode::internal, "Bilibili requires the upstream client", false, {}};
            return std::shared_ptr<ports::ICredentialVerifier>{std::make_shared<Verifier>(setup)};
        }}};
}
} // namespace music_service::providers::bilibili
