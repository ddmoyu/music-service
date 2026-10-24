#include "providers/kugou/web_audio.hpp"
#include "providers/kugou/parsing.hpp"
#include "application/transport/url.hpp"
#include <algorithm>
#include <chrono>

namespace music_service::providers::kugou {
namespace {
using parsing::number;
using parsing::text;

std::string_view trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    return value;
}
std::string value_of(std::string_view values, std::string_view key, char separator) {
    std::string result;
    while (!values.empty()) {
        const auto end = values.find(separator);
        const auto pair = trim(values.substr(0, end));
        const auto eq = pair.find('=');
        if (eq != pair.npos && trim(pair.substr(0, eq)) == key) {
            const auto value = trim(pair.substr(eq + 1));
            if (!result.empty() && result != value)
                throw Error{ErrorCode::credentials_required, "Kugou Cookie contains conflicting login fields", false, {}};
            result = value;
        }
        values = end == values.npos ? std::string_view{} : values.substr(end + 1);
    }
    return result;
}
bool identifier(std::string_view value, std::size_t max) {
    return !value.empty() && value.size() <= max && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}
std::map<std::string, std::string> login_params(const ports::ProviderContext& c) {
    if (!c.secret || c.secret->reveal().empty())
        throw Error{ErrorCode::credentials_required, "Kugou audio requires the selected Cookie", false, {}};
    const auto cookie = c.secret->reveal();
    if (cookie.size() > 64 * 1024 || cookie.find_first_of("\r\n") != cookie.npos)
        throw Error{ErrorCode::credentials_required, "invalid Kugou Cookie", false, {}};
    const auto nested = value_of(cookie, "KuGoo", ';');
    const auto account_field = [&](std::string_view name) {
        auto value = value_of(nested, name, '&');
        const auto direct = value_of(cookie, name, ';');
        if (!value.empty() && !direct.empty() && value != direct)
            throw Error{ErrorCode::credentials_required, "Kugou Cookie contains conflicting login fields", false, {}};
        return value.empty() ? direct : value;
    };
    auto userid = account_field("KugooID"), token = account_field("t");
    auto mid = value_of(cookie, "kg_mid", ';'), dfid = value_of(cookie, "kg_dfid", ';');
    if (mid.empty()) mid = value_of(cookie, "mid", ';');
    if (dfid.empty()) dfid = value_of(cookie, "dfid", ';');
    if (!decimal(userid) || !identifier(token, 1024) || !identifier(mid, 128) || !identifier(dfid, 128))
        throw Error{ErrorCode::credentials_required,
                    "Kugou Cookie needs KugooID, t, kg_mid and kg_dfid login fields", false, {}};
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return {{"appid", "1014"}, {"srcappid", "2919"}, {"clientver", "20000"},
            {"clienttime", std::to_string(now)}, {"platid", "4"}, {"mid", mid}, {"uuid", mid},
            {"dfid", dfid}, {"userid", std::move(userid)}, {"token", std::move(token)}};
}
Json songinfo(const Client& client, const ports::ProviderContext& c,
              std::map<std::string, std::string> params, const std::string& hash) {
    auto doc = client.get("resolve_audio",
        signed_web_url("https://wwwapi.kugou.com/play/songinfo", std::move(params)), c, true);
    const auto code = number(doc, doc.contains("err_code") ? "err_code" : "error_code");
    if (number(doc, "status") != 1 || code != 0) {
        if (code == 1002)
            throw Error{ErrorCode::upstream_rate_limited, "Kugou request rate limited", true, {}};
        throw Error{ErrorCode::upstream_failed, "Kugou web audio request was rejected", false,
                    {{"kugou_code", std::to_string(code)}}};
    }
    auto data = doc.at("data");
    if (!data.is_object() || lower(text(data, "hash", true)) != hash)
        throw std::invalid_argument{"web audio identity"};
    return data;
}
}

ports::ResolvedAudio resolve_web_audio(const Client& client, const ports::ProviderContext& c,
                                       const domain::TrackRef& ref,
                                       const domain::ResourceRequirement& requirement) {
    auto params = login_params(c);
    // 官网网页音源只确认了 standard MP3；高档 strict 请求不能冒充支持或回退匿名。
    if (requirement.requested != domain::Quality::standard &&
        requirement.policy == domain::QualityPolicy::strict)
        throw Error{ErrorCode::quality_unavailable, "Kugou Cookie audio currently supports standard MP3", false, {}};
    params["hash"] = ref.id.value();
    auto data = songinfo(client, c, params, ref.id.value());
    // 官网先以 hash 查出专辑歌曲身份，再使用 encode_album_audio_id 取得账号授权音源。
    const auto encoded = text(data, "encode_album_audio_id", true);
    if (!identifier(encoded, 128)) throw std::invalid_argument{"web audio identifier"};
    params.erase("hash");
    params["encode_album_audio_id"] = encoded;
    data = songinfo(client, c, std::move(params), ref.id.value());
    if (text(data, "encode_album_audio_id", true) != encoded)
        throw std::invalid_argument{"web album audio identity"};
    const auto location = text(data, "play_url");
    if (location.empty())
        throw Error{ErrorCode::credentials_required, "Kugou account has no permission for this audio", false, {}};
    const auto free_part = number(data, "is_free_part");
    if (free_part > 1) throw std::invalid_argument{"web preview flag"};
    const bool preview = free_part == 1;
    if (preview && !requirement.allow_preview)
        throw Error{ErrorCode::preview_rejected, "Kugou account only returned a preview", false, {}};
    const auto duration = number(data, "timelength"), size = number(data, "filesize"), bitrate = number(data, "bitrate");
    if (!duration || duration > 86400000 || !size) throw std::invalid_argument{"web audio duration or size"};
    if (bitrate != 128 && bitrate != 128000)
        throw Error{ErrorCode::quality_unavailable, "Kugou returned a different audio quality", false, {}};
    const auto url = application::transport::Url::parse(location);
    if (!url || !url->is_https() || url->port() != 443 || !url->host().ends_with(".kugou.com") ||
        !url->path().ends_with(".mp3"))
        throw std::invalid_argument{"web audio origin or format"};
    ports::ResolvedAudio out{ref, domain::Quality::standard,
        preview ? domain::Completeness::preview : domain::Completeness::full, "mp3",
        preview ? std::nullopt : std::optional<std::int64_t>{128000}, url->str(),
        {{"User-Agent", std::string{user_agent}}}, {}, static_cast<std::int64_t>(duration), ports::ProcessingPlan{}};
    out.shareable = false;
    return out;
}
}
