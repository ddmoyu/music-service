#include "providers/qianqian/parsing.hpp"
#include <algorithm>
#include <charconv>
#include <set>
#include <type_traits>

namespace music_service::providers::qianqian {
namespace {
using parsing::text;
using parsing::number;
std::uint32_t offset(const ports::ListQuery& query, bool revised) {
    if (query.limit == 0 || query.limit > 50) throw Error::validation("limit", "must be between 1 and 50");
    if (!query.cursor) return 0;
    const auto split = query.cursor->find(':');
    if (revised != (split != std::string::npos) || (revised && query.cursor->size() - split - 1 != 64))
        throw Error{ErrorCode::cursor_mismatch, "invalid Qianqian cursor", false, {}};
    auto value = std::string_view{*query.cursor}.substr(0, split);
    std::uint32_t result{};
    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (value.empty() || ec != std::errc{} || end != value.data() + value.size() || result > 10000 || result % query.limit != 0)
        throw Error{ErrorCode::cursor_mismatch, "invalid Qianqian page offset", false, {}};
    return result;
}
void revision(const ports::ListQuery& query, const std::string& current) {
    if (query.cursor && query.cursor->substr(query.cursor->find(':') + 1) != current)
        throw Error{ErrorCode::cursor_expired, "Qianqian collection changed; restart pagination", false, {}};
}
template<class Item> Item parse_item(const Json& json) {
    if constexpr (std::is_same_v<Item, domain::Track>) return parsing::track(json);
    else if constexpr (std::is_same_v<Item, domain::Album>) return parsing::album(json);
    else return parsing::playlist(json);
}
template<class Item> ports::CatalogPage<Item> page(const Json& list, const ports::ListQuery& query,
    std::uint32_t start, std::uint64_t total, bool more, std::string rev = {}) {
    parsing::array(list, query.limit);
    if (more && (list.empty() || start + query.limit > 10000))
        throw Error{ErrorCode::upstream_failed, "Qianqian page cannot make bounded progress", false, {}};
    ports::CatalogPage<Item> out;
    std::uint64_t unavailable = 0;
    for (const auto& value : list) {
        if (value.is_null()) { ++unavailable; continue; }
        out.page.items.push_back(parse_item<Item>(value));
    }
    out.page.page = {query.limit, static_cast<std::uint32_t>(out.page.items.size()), total, more, {}, {}};
    if (unavailable) out.page.page.unavailable_count = unavailable;
    // pageNo/pageSize 是请求窗口；短页也按窗口推进，避免重复或漏掉下一页。
    if (more) out.next_cursor = std::to_string(start + query.limit) + (rev.empty() ? "" : ":" + rev);
    return out;
}
template<class Item> ports::CatalogPage<Item> search(const Client& client, const ports::ProviderContext& context,
    const ports::SearchQuery& query) {
    ports::ListQuery paging{query.limit, query.cursor};
    const auto start = offset(paging, false);
    if (query.text.empty() || query.text.size() > 200 || !domain::is_valid_utf8(query.text) || query.text.find('\0') != query.text.npos)
        throw Error::validation("query", "must contain 1 to 200 UTF-8 bytes");
    constexpr bool tracks = std::is_same_v<Item, domain::Track>;
    auto data = client.get(tracks ? "search_tracks" : "search_albums", "search",
        {{"word", query.text}, {"type", tracks ? "1" : "3"}, {"pageNo", std::to_string(start / query.limit + 1)},
         {"pageSize", std::to_string(query.limit)}}, context);
    return page<Item>(data.at(tracks ? "typeTrack" : "typeAlbum"), paging, start, number(data, "total"), parsing::more(data));
}
class TrackSearch final : public ports::ITrackSearch {
public:
    explicit TrackSearch(const ports::ProviderSetup& setup) : client_(setup) {}
    domain::Result<ports::TrackSearchPage> search_tracks(const ports::ProviderContext& c, const ports::TrackSearchQuery& q) override {
        return checked([&] { return search<domain::Track>(client_, c, q); });
    }
private: Client client_;
};
class AlbumSearch final : public ports::AlbumSearch {
public:
    explicit AlbumSearch(const ports::ProviderSetup& setup) : client_(setup) {}
    domain::Result<ports::CatalogPage<domain::Album>> search(const ports::ProviderContext& c, const ports::SearchQuery& q) override {
        return checked([&] { return qianqian::search<domain::Album>(client_, c, q); });
    }
private: Client client_;
};
template<class Item> Json collection(const Client& client, const ports::ProviderContext& context,
    const domain::PlatformRef& ref, std::string_view operation, std::uint32_t start, std::uint32_t limit) {
    constexpr bool album = std::is_same_v<Item, domain::Album>;
    check_ref(ref, album ? 'P' : '\0');
    auto data = album ? client.get(operation, "album/info", {{"albumAssetCode", ref.id.value()}}, context) :
        client.get(operation, "tracklist/info", {{"id", ref.id.value()}, {"type", "0"},
            {"pageNo", std::to_string(start / limit + 1)}, {"pageSize", std::to_string(limit)}}, context);
    if (parse_item<Item>(data).ref != ref) throw std::invalid_argument{"collection response identity"};
    return data;
}
template<class Tag> class Detail final : public ports::IEntityDetail<Tag> {
public:
    explicit Detail(const ports::ProviderSetup& setup) : client_(setup) {}
    domain::Result<typename Tag::Item> detail(const ports::ProviderContext& c, const domain::PlatformRef& ref) override {
        return checked([&] { return parse_item<typename Tag::Item>(collection<typename Tag::Item>(client_, c, ref, Tag::key, 0, 1)); });
    }
private: Client client_;
};
template<class Tag> class Tracks final : public ports::IEntityList<Tag> {
public:
    explicit Tracks(const ports::ProviderSetup& setup) : client_(setup) {}
    domain::Result<ports::CatalogPage<domain::Track>> list(const ports::ProviderContext& c,
        const domain::PlatformRef& ref, const ports::ListQuery& q) override {
        return checked([&] {
            const auto start = offset(q, true);
            if constexpr (std::is_same_v<Tag, ports::catalog_tag::AlbumTracks>) {
                auto data = collection<domain::Album>(client_, c, ref, Tag::key, 0, q.limit);
                const auto album = parsing::album(data);
                const auto& all = data.at("trackList");
                parsing::array(all, 2000);
                const auto rev = digest(Json{ref.id.value(), all}.dump());
                revision(q, rev);
                Json window = Json::array();
                for (std::size_t i = start; i < std::min<std::size_t>(all.size(), start + q.limit); ++i) {
                    auto value = all[i];
                    if (!value.is_null()) {
                        value["albumTitle"] = album.title;
                        value["albumAssetCode"] = ref.id.value();
                        if (text(value, "pic").empty() && album.artwork) value["pic"] = *album.artwork;
                    }
                    window.push_back(std::move(value));
                }
                auto out = page<domain::Track>(window, q, start, all.size(), start + window.size() < all.size(), rev);
                out.page.page.listed_total = all.size();
                for (auto& track : out.page.items) track.track_total = static_cast<std::uint32_t>(all.size());
                return out;
            } else {
                auto data = collection<domain::Playlist>(client_, c, ref, Tag::key, start, q.limit);
                const auto total = number(data, "trackCount");
                const auto rev = digest(Json{ref.id.value(), total, text(data, "addDate"), text(data, "title")}.dump());
                revision(q, rev);
                auto& tracks = data.at("trackList");
                parsing::array(tracks, q.limit);
                // 此端点的 sort 是歌单位置，不是专辑曲号；顺序由原数组保留。
                for (auto& item : tracks) if (item.is_object()) item.erase("sort");
                return page<domain::Track>(tracks, q, start, total, parsing::more(data), rev);
            }
        });
    }
private: Client client_;
};
std::vector<domain::Category> categories(const Client& client, const ports::ProviderContext& context, std::string_view operation) {
    auto data = client.get(operation, "tracklist/category", {}, context);
    parsing::array(data, 100);
    std::vector<domain::Category> out;
    std::set<std::string> seen;
    for (const auto& group : data) {
        const auto& children = group.at("subCate");
        parsing::array(children, 100);
        for (const auto& child : children) {
            auto id = text(child, "id", true);
            if (!identifier(id) || !seen.insert(id).second || seen.size() > 500) throw std::invalid_argument{"category identity"};
            out.push_back({std::move(id), text(child, "categoryName", true), text(group, "categoryName", true), domain::ListedKind::playlist});
        }
    }
    return out;
}
class Discovery final : public ports::ICategories, public ports::IRecommendedPlaylists, public ports::CategoryPlaylists {
public:
    explicit Discovery(const ports::ProviderSetup& setup) : client_(setup) {}
    domain::Result<std::vector<domain::Category>> categories(const ports::ProviderContext& c) override {
        return checked([&] { return qianqian::categories(client_, c, "categories"); });
    }
    domain::Result<ports::CatalogPage<domain::Playlist>> recommend_playlists(const ports::ProviderContext& c, const ports::ListQuery& q) override {
        // 官方首页 hotSongList 使用同一 tracklist/list；这是公共热门歌单，不是个性化推荐。
        return checked([&] { return playlists(c, q, "recommend_playlists", {}); });
    }
    domain::Result<ports::CatalogPage<domain::Playlist>> list(const ports::ProviderContext& c,
        const domain::PlatformRef& ref, const ports::ListQuery& q) override {
        return checked([&] {
            check_ref(ref, '\0');
            (void)offset(q, true);
            const auto catalog = qianqian::categories(client_, c, "category_playlists");
            if (std::none_of(catalog.begin(), catalog.end(), [&](const auto& item) { return item.id == ref.id.value(); }))
                throw Error{ErrorCode::not_found, "Qianqian category was not found", false, {}};
            return playlists(c, q, "category_playlists", ref.id.value());
        });
    }
private:
    ports::CatalogPage<domain::Playlist> playlists(const ports::ProviderContext& c, const ports::ListQuery& q,
        std::string_view operation, const std::string& category) {
        const auto start = offset(q, true);
        Params params{{"pageNo", std::to_string(start / q.limit + 1)}, {"pageSize", std::to_string(q.limit)}};
        if (!category.empty()) params["subCateId"] = category;
        auto data = client_.get(operation, "tracklist/list", std::move(params), c);
        const auto total = number(data, "total");
        const auto rev = digest(Json{category, total}.dump());
        revision(q, rev);
        return page<domain::Playlist>(data.at("result"), q, start, total, parsing::more(data), rev);
    }
    Client client_;
};
}
void add_catalog(ports::ProviderBundle& bundle) {
    bind<ports::ITrackSearch, TrackSearch>(bundle);
    bind<ports::AlbumSearch, AlbumSearch>(bundle);
    bind<ports::PlaylistDetail, Detail<ports::catalog_tag::PlaylistDetail>>(bundle);
    bind<ports::AlbumDetail, Detail<ports::catalog_tag::AlbumDetail>>(bundle);
    bind<ports::PlaylistTracks, Tracks<ports::catalog_tag::PlaylistTracks>>(bundle);
    bind<ports::AlbumTracks, Tracks<ports::catalog_tag::AlbumTracks>>(bundle);
    bind<ports::ICategories, Discovery>(bundle);
    bind<ports::IRecommendedPlaylists, Discovery>(bundle);
    bind<ports::CategoryPlaylists, Discovery>(bundle);
}
}
