#include "providers/qq/parsing.hpp"
#include "application/transport/url.hpp"
#include <limits>

namespace music_service::providers::qq::parsing {
std::string text(const Json& j, const char* key, bool required) {
    auto i = j.find(key);
    if (i == j.end() || i->is_null()) { if (required) throw std::invalid_argument{"missing text"}; return {}; }
    auto value = i->is_number_unsigned() ? std::to_string(i->get<std::uint64_t>()) :
                 i->is_number_integer() && i->get<std::int64_t>() >= 0 ? std::to_string(i->get<std::int64_t>()) : i->get<std::string>();
    if (value.size() > 65536 || (required && value.empty())) throw std::invalid_argument{"text limit"};
    return value;
}
std::uint64_t number(const Json& j, const char* key) {
    const auto& value = j.at(key);
    if (value.is_number_unsigned()) return value.get<std::uint64_t>();
    if (value.is_number_integer() && value.get<std::int64_t>() >= 0) return static_cast<std::uint64_t>(value.get<std::int64_t>());
    if (value.is_string()) {
        auto s = value.get<std::string>(); std::uint64_t n{};
        auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
        if (!s.empty() && ec == std::errc{} && end == s.data() + s.size()) return n;
    }
    throw std::invalid_argument{"number"};
}
domain::PlatformRef ref(std::string id, bool numeric) {
    if (!(numeric ? decimal(id) : mid(id))) throw std::invalid_argument{"identifier"};
    return {take(domain::SourceId::parse("qq")), take(domain::PlatformId::parse(id))};
}
domain::Artwork artwork(std::string value) {
    if (value.empty()) return {};
    if (value.starts_with("http://")) value.replace(0, 7, "https://");
    auto url = application::transport::Url::parse(value);
    if (!url || !url->is_https() || url->port() != 443) return {};
    const auto& h = url->host();
    if (h != "y.gtimg.cn" && h != "qpic.y.qq.com" && h != "y.qq.com") return {};
    return url->str();
}
namespace {
std::vector<domain::ArtistSummary> singers(const Json& list) {
    if (!list.is_array() || list.size() > 100) throw std::invalid_argument{"singers"};
    std::vector<domain::ArtistSummary> out;
    for (const auto& v : list) {
        domain::ArtistSummary artist{text(v, "name", true), {}};
        const auto id = text(v, "mid"); if (mid(id)) artist.ref = ref(id);
        out.push_back(std::move(artist));
    }
    return out;
}
}
domain::Track track(const Json& j) {
    domain::Track t{ref(text(j, "mid", true)), text(j, "title").empty() ? text(j, "name", true) : text(j, "title"), singers(j.at("singer"))};
    const auto seconds = number(j, "interval");
    if (seconds > 86400) throw std::invalid_argument{"duration"};
    if (seconds) t.duration_ms = static_cast<std::int64_t>(seconds) * 1000;
    const auto& a = j.at("album");
    if (!text(a, "name").empty()) t.album = domain::AlbumSummary{text(a, "name"), {}};
    const auto id = text(a, "mid");
    if (mid(id)) {
        if (t.album) t.album->ref = ref(id);
        t.artwork = "https://y.gtimg.cn/music/photo_new/T002R500x500M000" + id + ".jpg";
    }
    if (j.contains("index_album")) { const auto n = number(j, "index_album"); if (n > 0 && n <= 10000) t.track_number = static_cast<std::uint32_t>(n); }
    const auto subtitle = text(j, "subtitle"); if (!subtitle.empty()) t.version.label = subtitle;
    return t;
}
domain::Playlist playlist(const Json& j) {
    const auto id = j.contains("dissid") ? text(j, "dissid", true) :
                    j.contains("content_id") ? text(j, "content_id", true) : text(j, "id", true);
    domain::Playlist p{ref(id, true)};
    p.name = j.contains("dissname") ? text(j, "dissname", true) : text(j, "title", true);
    if (j.contains("creator") && j["creator"].is_object()) p.creator = text(j["creator"], "name");
    else if (!text(j, "username").empty()) p.creator = text(j, "username");
    else if (!text(j, "host_nick").empty()) p.creator = text(j, "host_nick");
    auto description = text(j, "introduction"); if (description.empty()) description = text(j, "desc");
    if (!description.empty()) p.description = std::move(description);
    auto cover = text(j, "imgurl"); if (cover.empty()) cover = text(j, "cover"); if (cover.empty()) cover = text(j, "picurl");
    p.artwork = artwork(cover);
    if (j.contains("song_count")) p.track_count = number(j, "song_count");
    else if (j.contains("songnum")) p.track_count = number(j, "songnum");
    if (j.contains("listennum")) p.play_count = number(j, "listennum");
    else if (j.contains("listen_num")) p.play_count = number(j, "listen_num");
    return p;
}
domain::Album album(const Json& j) {
    const auto id = j.contains("albumMID") ? text(j, "albumMID", true) : text(j, "albumMid", true);
    domain::Album a{ref(id)};
    a.title = text(j, "albumName", true);
    if (j.contains("singer_list")) a.artists = singers(j["singer_list"]);
    else if (!text(j, "singerName").empty()) a.artists.push_back({text(j, "singerName"), {}});
    auto date = text(j, "publicTime"); if (date.empty()) date = text(j, "publishDate");
    if (!date.empty()) a.release_date = date;
    if (j.contains("song_count")) a.track_count = number(j, "song_count");
    // artist_albums 的 totalNum=0 不是可靠的专辑曲目总数，不使用它。
    a.artwork = "https://y.gtimg.cn/music/photo_new/T002R500x500M000" + id + ".jpg";
    if (!text(j, "desc").empty()) a.description = text(j, "desc");
    return a;
}
domain::Artist artist(const Json& j) {
    return {ref(text(j, "singerMID", true)), text(j, "singerName", true), artwork(text(j, "singerPic"))};
}
domain::Chart chart(const Json& j) {
    domain::Chart c{ref(text(j, "topId", true), true)};
    c.name = text(j, "title", true);
    if (!text(j, "intro").empty()) c.description = text(j, "intro");
    if (!text(j, "updateTime").empty()) c.updated_at = text(j, "updateTime");
    c.track_count = number(j, "totalNum");
    return c;
}
}
