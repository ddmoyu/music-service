#include "providers/migu/parsing.hpp"
#include "application/transport/url.hpp"
#include <charconv>

namespace music_service::providers::migu::parsing {
std::string text(const Json& j, const char* key, bool required) {
    if (!j.contains(key) || j.at(key).is_null()) {
        if (required)
            throw std::invalid_argument{"missing text"};
        return {};
    }
    auto out = j.at(key).get<std::string>();
    if (out.size() > 128 * 1024 || out.find('\0') != out.npos || (required && out.empty()))
        throw std::invalid_argument{"text limit"};
    return out;
}
std::string first(const Json& j, std::initializer_list<const char*> keys) {
    for (auto key : keys) {
        auto value = text(j, key);
        if (!value.empty())
            return value;
    }
    return {};
}
std::optional<std::uint64_t> number(const Json& j, const char* key) {
    if (!j.contains(key) || j.at(key).is_null() || j.at(key) == "")
        return {};
    const auto& v = j.at(key);
    std::uint64_t out{};
    if (v.is_number_unsigned())
        out = v.get<std::uint64_t>();
    else if (v.is_number_integer() && v.get<std::int64_t>() >= 0)
        out = static_cast<std::uint64_t>(v.get<std::int64_t>());
    else if (v.is_string()) {
        auto s = v.get<std::string>();
        auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
        if (s.empty() || ec != std::errc{} || end != s.data() + s.size())
            throw std::invalid_argument{"number"};
    } else
        throw std::invalid_argument{"number"};
    if (out > 1000000000000ULL)
        throw std::invalid_argument{"number limit"};
    return out;
}
std::uint64_t count(const Json& j, const char* key) {
    const auto value = number(j, key);
    if (!value)
        throw std::invalid_argument{"missing count"};
    return *value;
}
domain::PlatformRef ref(std::string id, bool album) {
    std::string_view raw = id;
    if (album && raw.starts_with("d:"))
        raw.remove_prefix(2);
    if (!decimal(raw))
        throw std::invalid_argument{"identifier"};
    return {take(domain::SourceId::parse("migu")), take(domain::PlatformId::parse(id))};
}
domain::Artwork artwork(std::string value) {
    if (value.empty())
        return {};
    if (value.starts_with("/data/"))
        value = "https://d.musicapp.migu.cn" + value;
    if (value.starts_with("http://"))
        value.replace(0, 7, "https://");
    auto u = application::transport::Url::parse(value);
    if (!u || !u->is_https() || u->port() != 443 || !u->host().ends_with(".migu.cn") ||
        value.find('#') != value.npos)
        return {};
    return u->str();
}
namespace {
domain::Artwork image(const Json& j) {
    auto direct = first(j, {"musicListPicUrl", "originalImgUrl", "imageUrl", "img", "img1"});
    if (!direct.empty())
        return artwork(direct);
    for (auto key : {"imgItems", "albumImgs", "imgItem"}) {
        if (!j.contains(key))
            continue;
        const auto& values = j.at(key);
        if (values.is_object())
            return artwork(text(values, "img"));
        array_shape(values, 20);
        for (const auto& v : values)
            if (auto url = artwork(text(v, "img")))
                return url;
    }
    return {};
}
std::vector<domain::ArtistSummary> artists(const Json& j) {
    std::vector<domain::ArtistSummary> out;
    for (auto key : {"singerList", "singers", "artists"}) {
        if (!j.contains(key))
            continue;
        const auto& values = j.at(key);
        array_shape(values, 100);
        for (const auto& v : values) {
            const auto id = text(v, "id");
            out.push_back(
                {text(v, "name", true), decimal(id) ? std::optional{ref(id)} : std::nullopt});
        }
        if (!out.empty())
            return out;
    }
    if (auto name = text(j, "singer"); !name.empty()) {
        auto id = text(j, "singerId");
        out.push_back({std::move(name), decimal(id) ? std::optional{ref(id)} : std::nullopt});
    }
    return out;
}
std::optional<std::string> optional(std::string value) {
    return value.empty() ? std::nullopt : std::optional{std::move(value)};
}
} // namespace
domain::Track track(const Json& j) {
    domain::Track out{.ref = ref(text(j, "contentId", true)),
                      .title = first(j, {"songName", "name"}),
                      .artists = artists(j)};
    if (out.title.empty())
        throw std::invalid_argument{"track title"};
    auto album_id = text(j, "albumId"), album_name = text(j, "album");
    if (j.contains("albums")) {
        const auto& values = j.at("albums");
        array_shape(values, 20);
        if (!values.empty()) {
            album_id = text(values.front(), "id");
            album_name = text(values.front(), "name");
        }
    }
    if (!album_name.empty())
        out.album = domain::AlbumSummary{
            album_name, decimal(album_id) ? std::optional{ref(album_id)} : std::nullopt};
    if (auto duration = number(j, "duration"); duration && *duration) {
        if (*duration > 86400)
            throw std::invalid_argument{"duration limit"};
        out.duration_ms = static_cast<std::int64_t>(*duration * 1000);
    } else if (const auto length = text(j, "length"); !length.empty()) {
        if (length.size() != 8 || length[2] != ':' || length[5] != ':')
            throw std::invalid_argument{"duration format"};
        const auto part = [&](std::size_t i) {
            if (length[i] < '0' || length[i] > '9' || length[i + 1] < '0' || length[i + 1] > '9')
                throw std::invalid_argument{"duration digits"};
            return (length[i] - '0') * 10 + length[i + 1] - '0';
        };
        auto h = part(0), m = part(3), s = part(6);
        if (h > 23 || m > 59 || s > 59)
            throw std::invalid_argument{"duration range"};
        if (h || m || s)
            out.duration_ms = (h * 3600LL + m * 60LL + s) * 1000;
    }
    out.artwork = image(j);
    // A positive paid-audition marker is evidence of a playback charge. Zero alone
    // does not prove a full free resource; vipType's empty/unknown enums stay unknown.
    if (number(j, "chargeAuditions") == 1) out.pay.pay_play = 1;
    if (auto n = number(j, "trackNumber"); n && *n) {
        if (*n > 10000)
            throw std::invalid_argument{"track number"};
        out.track_number = static_cast<std::uint32_t>(*n);
    }
    return out;
}
domain::Playlist playlist(const Json& j) {
    auto id = first(j, {"musicListId", "resId", "id"});
    if (id.empty() && j.contains("logEvent"))
        id = text(j.at("logEvent"), "contentId", true);
    domain::Playlist out{.ref = ref(id), .name = first(j, {"title", "name", "txt"})};
    if (out.name.empty())
        throw std::invalid_argument{"playlist title"};
    out.creator = optional(first(j, {"ownerName", "userName"}));
    out.description = optional(first(j, {"summary", "txt2"}));
    out.artwork = image(j);
    out.track_count = number(j, "musicNum");
    out.play_count = number(j, "playNum");
    out.updated_at = optional(text(j, "publishTime"));
    if (j.contains("opNumItem"))
        out.play_count = number(j.at("opNumItem"), "playNum");
    return out;
}
domain::Album album(const Json& j) {
    const auto type = text(j, "resourceType", true);
    if (type != "5" && type != "2003")
        throw std::invalid_argument{"album type"};
    auto id = type == "5" ? first(j, {"contentId", "id"}) : first(j, {"albumId", "id"});
    if (type == "5")
        id = "d:" + id;
    domain::Album out{
        .ref = ref(id, true), .title = first(j, {"title", "name"}), .artists = artists(j)};
    if (out.title.empty())
        throw std::invalid_argument{"album title"};
    out.artwork = image(j);
    out.track_count = number(j, "totalCount");
    out.release_date = optional(first(j, {"publishDate", "publishTime"}));
    out.description = optional(text(j, "summary"));
    return out;
}
domain::Artist artist(const Json& j) {
    return {.ref = ref(text(j, "id", true)), .name = text(j, "name", true), .artwork = image(j)};
}
domain::Chart chart(const Json& j) {
    domain::Chart out{.ref = ref(first(j, {"rankId", "columnId"})),
                      .name = first(j, {"rankName", "title"})};
    if (out.name.empty())
        throw std::invalid_argument{"chart title"};
    out.description = optional(text(j, "desc"));
    out.updated_at = optional(text(j, "updateTime"));
    out.track_count = number(j, "totalCount");
    return out;
}
} // namespace music_service::providers::migu::parsing
