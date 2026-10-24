#include "providers/qianqian/parsing.hpp"
#include "application/transport/url.hpp"
#include <charconv>
#include <set>

namespace music_service::providers::qianqian::parsing {
std::string text(const Json& json, const char* key, bool required) {
    const auto it = json.find(key);
    if (it == json.end() || it->is_null()) {
        if (required) throw std::invalid_argument{"missing text"};
        return {};
    }
    auto value = it->is_number_unsigned() ? std::to_string(it->get<std::uint64_t>()) :
        it->is_number_integer() && it->get<std::int64_t>() >= 0 ? std::to_string(it->get<std::int64_t>()) : it->get<std::string>();
    if (value.size() > 65536 || !domain::is_valid_utf8(value) || value.find('\0') != value.npos || (required && value.empty()))
        throw std::invalid_argument{"text limit or encoding"};
    return value;
}
std::uint64_t number(const Json& json, const char* key, std::uint64_t maximum) {
    const auto value = text(json, key, true);
    std::uint64_t out{};
    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), out);
    if (ec != std::errc{} || end != value.data() + value.size() || out > maximum) throw std::invalid_argument{"number"};
    return out;
}
bool more(const Json& json) { return number(json, "haveMore", 1) == 1; }
void array(const Json& json, std::size_t maximum) {
    if (!json.is_array() || json.size() > maximum) throw std::invalid_argument{"array limit"};
}
domain::PlatformRef ref(std::string id, char prefix) {
    if (!identifier(id, prefix)) throw std::invalid_argument{"entity identity"};
    return {take(domain::SourceId::parse("qianqian")), take(domain::PlatformId::parse(id))};
}
std::string resource_url(std::string value, bool audio) {
    if (value.empty()) return {};
    auto parsed = application::transport::Url::parse(value);
    if (!parsed || !parsed->is_https() || parsed->port() != 443 || value.find('#') != value.npos)
        throw std::invalid_argument{"resource URL"};
    const auto& host = parsed->host();
    const bool approved = audio ? host.starts_with("audio") && host.ends_with(".dmhmusic.com") :
        host.ends_with(".dmhmusic.com") || host == "static-qianqian.taihe.com" || host == "static1-qianqian.91q.com";
    if (!approved) throw std::invalid_argument{"resource origin"};
    return parsed->str();
}
namespace {
std::vector<domain::ArtistSummary> artists(const Json& json) {
    // 公开歌单可缺少演唱者字段；保留曲目及位置，不凭空补写歌手。
    const auto it = json.find("artist");
    if (it == json.end() || it->is_null()) return {};
    const auto& list = *it;
    array(list, 100);
    std::vector<domain::ArtistSummary> result;
    std::set<std::string> seen;
    // artistType=38 是演唱者；作词、作曲及制作人员不冒充歌手。
    for (const auto& artist : list) {
        if (number(artist, "artistType", 1000) != 38) continue;
        auto name = text(artist, "name", true);
        if (!seen.insert(name).second) continue;
        domain::ArtistSummary out{std::move(name), {}};
        const auto id = text(artist, "artistCode");
        if (!id.empty()) out.ref = ref(id, 'A');
        result.push_back(std::move(out));
    }
    return result;
}
domain::Artwork artwork(const Json& json) {
    const auto value = text(json, "pic");
    if (value.empty()) return {};
    return resource_url(value);
}
}
domain::Track track(const Json& json) {
    auto id = text(json, "TSID");
    const auto asset = text(json, "assetId");
    if (id.empty()) id = asset;
    if (!asset.empty() && asset != id) throw std::invalid_argument{"conflicting song identity"};
    domain::Track out{ref(id, 'T'), text(json, "title", true), artists(json)};
    if (json.contains("duration")) {
        const auto seconds = number(json, "duration", 86400);
        if (seconds) out.duration_ms = static_cast<std::int64_t>(seconds * 1000);
    }
    auto album_title = text(json, "albumTitle");
    if (!album_title.empty()) {
        out.album = domain::AlbumSummary{std::move(album_title), {}};
        auto album_id = text(json, "albumAssetCode");
        if (!album_id.empty()) out.album->ref = ref(album_id, 'P');
    }
    out.artwork = artwork(json);
    if (json.contains("sort")) {
        auto sort = number(json, "sort", 10000);
        if (sort) out.track_number = static_cast<std::uint32_t>(sort);
    }
    if (json.contains("isVip") && !json.at("isVip").is_null()) {
        const auto vip = number(json, "isVip");
        if (vip <= 1) out.pay.pay_month = static_cast<std::uint32_t>(vip);
        if (vip == 1) out.pay.pay_play = 1;
    }
    if (json.contains("pay_model") && !json.at("pay_model").is_null()) {
        const auto model = number(json, "pay_model");
        if (model == 3) out.pay.pay_play = 1;
        else if (model == 2 && out.pay.pay_month == 0) out.pay.pay_play = 0;
    }
    // isPaid is an account purchase state; neither it nor isVip proves download rights.
    return out;
}
domain::Playlist playlist(const Json& json) {
    domain::Playlist out{ref(text(json, "id", true), '\0')};
    out.name = text(json, "title", true);
    out.artwork = artwork(json);
    if (!text(json, "desc").empty()) out.description = text(json, "desc");
    if (json.contains("trackCount")) out.track_count = number(json, "trackCount");
    return out;
}
domain::Album album(const Json& json) {
    domain::Album out{ref(text(json, "albumAssetCode", true), 'P')};
    out.title = text(json, "title", true);
    out.artists = artists(json);
    out.artwork = artwork(json);
    auto date = text(json, "releaseDate");
    if (date.size() >= 10) out.release_date = date.substr(0, 10);
    if (!text(json, "introduce").empty()) out.description = text(json, "introduce");
    if (json.contains("trackList")) { array(json.at("trackList"), 2000); out.track_count = json.at("trackList").size(); }
    return out;
}
Json song(const Client& client, const ports::ProviderContext& context, const domain::TrackRef& ref, std::string_view operation) {
    check_ref(ref, 'T');
    auto doc = client.get(operation, "song/info", {{"TSID", ref.id.value()}}, context);
    array(doc, 1);
    if (doc.empty()) throw Error{ErrorCode::not_found, "Qianqian track was not found", false, {}};
    auto info = doc.front();
    if (track(info).ref != ref) throw std::invalid_argument{"song response identity"};
    return info;
}
}
