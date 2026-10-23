#include "providers/kuwo/parsing/artists.hpp"
#include "providers/kuwo/parsing/parsers.hpp"
#include "application/transport/url.hpp"

#include <nlohmann/json.hpp>
#include <charconv>
#include <limits>
#include <set>
#include <stdexcept>

namespace music_service::providers::kuwo::parsing {
namespace {
using Json = nlohmann::json;
using domain::Error;
using domain::ErrorCode;
namespace ports = application::ports;

Error invalid() { return {ErrorCode::upstream_failed, "kuwo returned invalid catalog data", false, {}}; }
template<class F> auto checked(F&& f) -> domain::Result<decltype(f())> {
    try { return f(); }
    catch (const Error& error) { return error; }
    catch (const std::exception&) { return invalid(); }
}
std::string text(const Json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) return {};
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_unsigned()) return std::to_string(it->get<std::uint64_t>());
    if (it->is_number_integer()) return std::to_string(it->get<std::int64_t>());
    throw invalid();
}
std::optional<std::uint64_t> count(const Json& j, const char* key) {
    const auto value = text(j, key);
    if (value.empty()) return {};
    std::uint64_t out{};
    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), out);
    if (ec != std::errc{} || end != value.data() + value.size()) throw invalid();
    return out;
}
std::optional<std::string> clean(const Json& j, const char* key, std::size_t max = 4096) {
    auto value = decode_text(text(j, key));
    if (value.size() > max || !domain::is_valid_utf8(value)) throw invalid();
    if (value.empty()) return {};
    return value;
}
std::string required_name(const Json& j, const char* key) {
    auto value = clean(j, key);
    if (!value) throw invalid();
    return *value;
}
domain::PlatformRef ref_of(std::string_view id) {
    if (!is_valid_rid(id)) throw invalid();
    return {domain::SourceId::parse("kuwo").value(), domain::PlatformId::parse(id).value()};
}
Json data(std::string_view body, bool detail = false) {
    auto doc = Json::parse(body);
    if (!doc.is_object()) throw invalid();
    const auto code = text(doc, "code");
    if (detail && code == "-1" && (!doc.contains("data") || doc["data"].is_null()))
        throw Error{ErrorCode::not_found, "kuwo entity was not found", false, {}};
    if (code != "200") {
        if (code == "429") throw Error{ErrorCode::upstream_rate_limited, "kuwo is limiting requests", true, {}};
        throw invalid();
    }
    if (!doc.contains("data")) throw invalid();
    if (detail && doc["data"].is_null()) throw Error{ErrorCode::not_found, "kuwo entity was not found", false, {}};
    if (!doc["data"].is_object()) throw invalid();
    return std::move(doc["data"]);
}

// 只接受官网公开图片目录；专辑封面不能用歌手头像或任意 URL 替代。
domain::Artwork picture(const Json& j, const char* key, std::string_view directory) {
    auto value = text(j, key);
    if (value.empty()) return {};
    auto url = application::transport::Url::parse(value);
    if (!url || !(url->host() == "kuwo.cn" || url->host().ends_with(".kuwo.cn")) ||
        !url->path().starts_with(directory) || (url->is_https() ? url->port() != 443 : url->port() != 80)) return {};
    if (!url->is_https()) value = "https://" + url->host() + url->target();
    return value;
}
domain::Artist artist(const Json& j) {
    if (!j.is_object()) throw invalid();
    domain::Artist out{ref_of(text(j, "id")), required_name(j, "name")};
    out.artwork = picture(j, "pic300", "/star/starheads/");
    if (!out.artwork) out.artwork = picture(j, "pic", "/star/starheads/");
    out.description = clean(j, "info", 128 * 1024);
    out.alternate_name = clean(j, "aartist");
    out.country = clean(j, "country");
    out.birth_date = clean(j, "birthday");
    out.birthplace = clean(j, "birthplace");
    out.gender = clean(j, "gener");
    out.language = clean(j, "language");
    out.height = clean(j, "tall");
    out.weight = clean(j, "weight");
    out.constellation = clean(j, "constellation");
    out.track_count = count(j, "musicNum");
    out.album_count = count(j, "albumNum");
    out.mv_count = count(j, "mvNum");
    return out;
}
domain::Track track(const Json& j) {
    if (!j.is_object()) throw invalid();
    domain::Track out{ref_of(text(j, "rid")), required_name(j, "name"),
        parse_artists(text(j, "artist"), text(j, "artistid")), {}, {}, {}};
    const auto album_id = text(j, "albumid");
    if (auto name = clean(j, "album")) {
        std::optional<domain::CollectionRef> album_ref;
        if (is_valid_rid(album_id)) album_ref = ref_of(album_id);
        out.album = domain::AlbumSummary{*name, album_ref};
        if (album_ref) out.artwork = picture(j, "albumpic", "/star/albumcover/");
    }
    if (const auto seconds = count(j, "duration"); seconds && *seconds > 0) {
        if (*seconds > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / 1000)) throw invalid();
        out.duration_ms = static_cast<std::int64_t>(*seconds) * 1000;
    }
    if (const auto number = count(j, "track"); number && *number > 0 && *number <= UINT32_MAX)
        out.track_number = static_cast<std::uint32_t>(*number);
    return out;
}
domain::Album album(const Json& j) {
    if (!j.is_object()) throw invalid();
    domain::Album out{ref_of(text(j, "albumid"))};
    out.title = required_name(j, "album");
    out.artists = parse_artists(text(j, "artist"), text(j, "artistid"));
    out.artwork = picture(j, "pic", "/star/albumcover/");
    out.release_date = clean(j, "releaseDate");
    if (out.release_date == "0000-00-00") out.release_date.reset();
    return out;
}
template<class Item, class Parse> ports::CatalogPage<Item> page(const Json& doc, const char* key,
    std::uint32_t page_index, std::uint32_t limit, Parse parse) {
    if (!limit || page_index > 10000) throw invalid();
    const auto total = count(doc, "total");
    const auto& list = doc.at(key);
    if (!total || !list.is_array() || list.size() > limit) throw invalid();
    const auto start = static_cast<std::uint64_t>(page_index) * limit;
    if ((list.empty() && start < *total) || (!list.empty() && start + list.size() > *total)) throw invalid();
    ports::CatalogPage<Item> out;
    std::set<std::string> ids;
    for (const auto& j : list) {
        auto value = parse(j);
        if (!ids.insert(value.ref.id.value()).second) throw invalid();
        out.page.items.push_back(std::move(value));
    }
    // 按上游页窗口推进，短页不能按可见数量推进或伪装成最后一页。
    const bool more = start + limit < *total;
    if (more && page_index == 10000) throw invalid();
    out.page.page = {limit, static_cast<std::uint32_t>(out.page.items.size()), total, more};
    if (more) out.next_cursor = std::to_string(page_index + 1);
    return out;
}
} // namespace

domain::Result<ports::CatalogPage<domain::Artist>> parse_artist_search(
    std::string_view body, std::uint32_t index, std::uint32_t limit) {
    return checked([&] {
        auto doc = data(body);
        // 请求 pn 从 1 开始，搜索响应的 pn 从 0 开始。
        if (count(doc, "pn") != index || count(doc, "rn") != limit) throw invalid();
        return page<domain::Artist>(doc, "artistList", index, limit, artist);
    });
}
domain::Result<domain::Artist> parse_artist_detail(std::string_view body, std::string_view id) {
    return checked([&] {
        auto out = artist(data(body, true));
        if (out.ref.id.value() != id) throw invalid();
        return out;
    });
}
domain::Result<ports::CatalogPage<domain::Track>> parse_artist_tracks(
    std::string_view body, std::uint32_t index, std::uint32_t limit) {
    return checked([&] { return page<domain::Track>(data(body), "list", index, limit, track); });
}
domain::Result<ports::CatalogPage<domain::Album>> parse_artist_albums(
    std::string_view body, std::uint32_t index, std::uint32_t limit) {
    return checked([&] { return page<domain::Album>(data(body), "albumList", index, limit, album); });
}
domain::Result<domain::Track> parse_track_info(std::string_view body, std::string_view id) {
    return checked([&] {
        auto out = track(data(body, true));
        if (out.ref.id.value() != id) throw invalid();
        return out;
    });
}
} // namespace music_service::providers::kuwo::parsing
