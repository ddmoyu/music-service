#include "providers/kugou/parsing.hpp"
#include "application/transport/url.hpp"
#include <charconv>

namespace music_service::providers::kugou::parsing {
std::string text(const Json& j, const char* key, bool required) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) {
        if (required)
            throw std::invalid_argument{"missing text"};
        return {};
    }
    auto value = it->is_number_unsigned() ? std::to_string(it->get<std::uint64_t>())
                 : it->is_number_integer() && it->get<std::int64_t>() >= 0
                     ? std::to_string(it->get<std::int64_t>())
                     : it->get<std::string>();
    if (value.size() > 65536 || (required && value.empty()))
        throw std::invalid_argument{"text limit"};
    return value;
}
std::string first(const Json& j, std::initializer_list<const char*> keys) {
    for (auto key : keys) {
        auto value = text(j, key);
        if (!value.empty())
            return value;
    }
    return {};
}
std::uint64_t number(const Json& j, const char* key) {
    auto value = text(j, key, true);
    std::uint64_t n{};
    auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), n);
    if (ec != std::errc{} || end != value.data() + value.size())
        throw std::invalid_argument{"number"};
    return n;
}
void array(const Json& j, std::size_t max) {
    if (!j.is_array() || j.size() > max)
        throw std::invalid_argument{"array limit"};
}
domain::PlatformRef ref(std::string id, bool song) {
    if (!(song ? hash(id) : decimal(id)))
        throw std::invalid_argument{"identifier"};
    return {take(domain::SourceId::parse("kugou")),
            take(domain::PlatformId::parse(song ? lower(std::move(id)) : id))};
}
domain::Artwork artwork(std::string value, bool artist) {
    if (value.empty())
        return {};
    if (auto at = value.find("{size}"); at != value.npos)
        value.replace(at, 6, "400");
    if (value.starts_with("http://"))
        value.replace(0, 7, "https://");
    auto url = application::transport::Url::parse(value);
    if (!url || !url->is_https() || url->port() != 443 ||
        (url->host() != "imge.kugou.com" && url->host() != "imgessl.kugou.com" &&
         !(artist && url->host() == "singerimg.kugou.com")))
        return {};
    return url->str();
}
namespace {
std::string clean(std::string value) {
    // 搜索只请求 em 高亮；不执行或保留其标签。
    for (std::string_view marker : {"<em>", "</em>"})
        for (auto at = value.find(marker); at != value.npos; at = value.find(marker))
            value.erase(at, marker.size());
    return value;
}
} // namespace
domain::Track track(const Json& j, bool detail) {
    const auto id = first(j, {"FileHash", "hash"});
    auto title = clean(first(j, {"SongName", "songname", "songName"}));
    auto artist = clean(first(j, {"SingerName", "singername", "author_name"}));
    const auto filename = clean(first(j, {"FileName", "filename", "fileName"}));
    if (title.empty() || artist.empty()) {
        auto split = filename.find(" - ");
        if (title.empty())
            title = split == filename.npos ? filename : filename.substr(split + 3);
        if (artist.empty() && split != filename.npos)
            artist = filename.substr(0, split);
    }
    if (title.empty())
        throw std::invalid_argument{"track title"};
    domain::Track out{ref(id, true), title, {}};
    if (!artist.empty())
        out.artists.push_back({artist, {}});
    auto suffix = text(j, "Suffix");
    if (!suffix.empty() && !out.title.ends_with(suffix))
        out.title += " " + suffix;
    if (!suffix.empty())
        out.version.label = suffix;
    const auto duration_key = detail                   ? "timeLength"
                              : j.contains("Duration") ? "Duration"
                                                       : "duration";
    if (j.contains(duration_key)) {
        auto seconds = number(j, duration_key);
        if (seconds > 86400)
            throw std::invalid_argument{"duration"};
        if (seconds)
            out.duration_ms = static_cast<std::int64_t>(seconds) * 1000;
    }
    if (detail && j.contains("extra") && j.at("extra").contains("128timelength")) {
        const auto ms = number(j.at("extra"), "128timelength");
        if (ms > 86400000)
            throw std::invalid_argument{"duration"};
        if (ms)
            out.duration_ms = static_cast<std::int64_t>(ms);
    }
    // remark 是影视出处等歌曲备注，不能代替未知的专辑名。
    const auto album_name = clean(first(j, {"AlbumName", "album_name"}));
    const auto album_id = first(j, {"AlbumID", "album_id", "albumid"});
    if (!album_name.empty()) {
        out.album = domain::AlbumSummary{album_name, {}};
        if (decimal(album_id))
            out.album->ref = ref(album_id);
    }
    // imgUrl / singerHead 是歌手头像，绝不能作为专辑封面。
    out.artwork = artwork(first(j, {"Image", "album_sizable_cover", "album_img"}));
    if (!out.artwork && j.contains("trans_param"))
        out.artwork = artwork(text(j.at("trans_param"), "union_cover"));
    return out;
}
domain::Playlist playlist(const Json& j) {
    domain::Playlist out{ref(text(j, "specialid", true))};
    out.name = clean(text(j, "specialname", true));
    auto creator = first(j, {"nickname", "username"});
    if (!creator.empty())
        out.creator = creator;
    auto intro = text(j, "intro");
    if (!intro.empty())
        out.description = intro;
    auto date = text(j, "publishtime");
    if (!date.empty())
        out.updated_at = date;
    if (j.contains("songcount"))
        out.track_count = number(j, "songcount");
    if (j.contains("playcount"))
        out.play_count = number(j, "playcount");
    out.artwork = artwork(text(j, "imgurl"));
    return out;
}
domain::Album album(const Json& j) {
    domain::Album out{ref(text(j, "albumid", true))};
    out.title = clean(text(j, "albumname", true));
    auto singer = text(j, "singername");
    if (!singer.empty())
        out.artists.push_back({clean(singer), {}});
    auto date = text(j, "publishtime");
    if (!date.empty())
        out.release_date = date.substr(0, 10);
    auto intro = text(j, "intro");
    if (!intro.empty())
        out.description = intro;
    if (j.contains("songcount"))
        out.track_count = number(j, "songcount");
    out.artwork = artwork(text(j, "imgurl"));
    return out;
}
domain::Artist artist(const Json& j, bool search) {
    domain::Artist out{ref(text(j, search ? "AuthorId" : "singerid", true))};
    out.name = clean(text(j, search ? "AuthorName" : "singername", true));
    if (out.name.empty())
        throw std::invalid_argument{"artist name"};
    out.artwork = artwork(text(j, search ? "Avatar" : "imgurl"), true);
    if (!search) {
        const auto profile = first(j, {"profile", "intro"});
        if (!profile.empty())
            out.description = profile;
        const auto alias = text(j, "alias");
        if (!alias.empty())
            out.alternate_name = alias;
    }
    auto count = [&](const char* key, std::optional<std::uint64_t>& target) {
        if (j.contains(key) && !j.at(key).is_null()) {
            const auto value = number(j, key);
            if (value > 10000000)
                throw std::invalid_argument{"artist count"};
            target = value;
        }
    };
    count(search ? "AudioCount" : "songcount", out.track_count);
    count(search ? "AlbumCount" : "albumcount", out.album_count);
    count(search ? "VideoCount" : "mvcount", out.mv_count);
    // 生平文本不拆成结构化出生日期、国籍等字段；没有独立证据的字段保持未知。
    return out;
}
domain::Chart chart(const Json& j) {
    domain::Chart out{ref(text(j, "rankid", true))};
    out.name = text(j, "rankname", true);
    auto intro = text(j, "intro");
    if (!intro.empty())
        out.description = intro;
    auto update = text(j, "update_frequency");
    if (!update.empty())
        out.update_hint = update;
    auto date = text(j, "rank_id_publish_date");
    if (!date.empty())
        out.updated_at = date;
    if (j.contains("extra") && j.at("extra").contains("resp") &&
        j.at("extra").at("resp").contains("all_total"))
        out.track_count = number(j.at("extra").at("resp"), "all_total");
    return out;
}
} // namespace music_service::providers::kugou::parsing
