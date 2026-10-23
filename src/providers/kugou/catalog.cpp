#include "application/transport/url.hpp"
#include "providers/kugou/paging.hpp"
#include "providers/kugou/parsing.hpp"
#include <algorithm>
#include <set>
#include <type_traits>

namespace music_service::providers::kugou {
namespace {
using application::transport::encode_query_component;
using parsing::number;
using parsing::text;
template <class Item> Item item(const Json& j) {
    if constexpr (std::is_same_v<Item, domain::Track>)
        return parsing::track(j);
    else if constexpr (std::is_same_v<Item, domain::Playlist>)
        return parsing::playlist(j);
    else
        return parsing::album(j);
}
template <class Item> std::vector<Item> items(const Json& list, std::size_t max = 50) {
    parsing::array(list, max);
    std::vector<Item> out;
    for (const auto& j : list)
        out.push_back(item<Item>(j));
    return out;
}
template <class Item>
ports::CatalogPage<Item> search_page(const Client& client, const ports::ProviderContext& c,
                                     const ports::SearchQuery& q) {
    const auto start = offset(q.cursor, q.limit, false);
    if (q.text.empty() || q.text.size() > 200 || !domain::is_valid_utf8(q.text))
        throw Error::validation("query", "must contain 1 to 200 UTF-8 bytes");
    Json data;
    if constexpr (std::is_same_v<Item, domain::Track>) {
        auto doc = client.get("search_tracks",
                              "https://songsearch.kugou.com/song_search_v2?keyword=" +
                                  encode_query_component(q.text) +
                                  "&platform=WebFilter&format=json&userid=-1&clientver=&filter=2&"
                                  "iscorrection=1&privilege_filter=0&tag=em" +
                                  window(start, q.limit),
                              c);
        api_success(doc);
        data = doc.at("data");
        if (number(data, "page") != start / q.limit + 1 || number(data, "pagesize") != q.limit)
            throw std::invalid_argument{"search page identity"};
    } else {
        constexpr bool albums = std::is_same_v<Item, domain::Album>;
        data = client.api(albums ? "search_albums" : "search_playlists",
                          std::string{"search/"} + (albums ? "album" : "special") +
                              "?keyword=" + encode_query_component(q.text) + "&format=json" +
                              window(start, q.limit),
                          c);
    }
    return page(
        items<Item>(data.at(std::is_same_v<Item, domain::Track> ? "lists" : "info"), q.limit),
        start, q.limit, number(data, "total"));
}
class TrackSearch final : public ports::ITrackSearch {
  public:
    explicit TrackSearch(const ports::ProviderSetup& s) : client_(s) {}
    domain::Result<ports::TrackSearchPage>
    search_tracks(const ports::ProviderContext& c, const ports::TrackSearchQuery& q) override {
        return checked([&] { return search_page<domain::Track>(client_, c, q); });
    }

  private:
    Client client_;
};
template <class Tag> class Search final : public ports::ICatalogSearch<Tag> {
  public:
    explicit Search(const ports::ProviderSetup& s) : client_(s) {}
    domain::Result<ports::CatalogPage<typename Tag::Item>>
    search(const ports::ProviderContext& c, const ports::SearchQuery& q) override {
        return checked([&] { return search_page<typename Tag::Item>(client_, c, q); });
    }

  private:
    Client client_;
};
Json charts_data(const Client& client, const ports::ProviderContext& c, std::string_view op) {
    auto data = client.api(op,
                           "rank/"
                           "list?version=9108&plat=0&showtype=2&parentid=0&apiver=2&area_code=1&"
                           "withsong=1&with_res_tag=0&page=1&pagesize=100",
                           c);
    const auto& list = data.at("info");
    parsing::array(list, 100);
    if (number(data, "total") != list.size())
        throw std::invalid_argument{"incomplete chart directory"};
    return list;
}
Json entity_data(const Client& client, const ports::ProviderContext& c,
                 const domain::PlatformRef& ref, std::string_view kind, std::string_view op) {
    check_ref(ref);
    if (kind == "rank") {
        for (const auto& j : charts_data(client, c, op))
            if (text(j, "rankid") == ref.id.value())
                return j;
        throw Error{ErrorCode::not_found, "Kugou chart was not found", false, {}};
    }
    auto data = client.api(op,
                           std::string{kind} + "/info?" + std::string{kind} +
                               "id=" + ref.id.value() + "&version=9108&area_code=1",
                           c);
    if (text(data, (std::string{kind} + "id").c_str(), true) != ref.id.value())
        throw std::invalid_argument{"entity identity"};
    return data;
}
template <class Tag> class Detail final : public ports::IEntityDetail<Tag> {
  public:
    explicit Detail(const ports::ProviderSetup& s) : client_(s) {}
    domain::Result<typename Tag::Item> detail(const ports::ProviderContext& c,
                                              const domain::PlatformRef& ref) override {
        return checked([&]() -> typename Tag::Item {
            if constexpr (std::is_same_v<typename Tag::Item, domain::Playlist>)
                return parsing::playlist(entity_data(client_, c, ref, "special", Tag::key));
            else if constexpr (std::is_same_v<typename Tag::Item, domain::Album>)
                return parsing::album(entity_data(client_, c, ref, "album", Tag::key));
            else
                return parsing::chart(entity_data(client_, c, ref, "rank", Tag::key));
        });
    }

  private:
    Client client_;
};
template <class Tag> class Tracks final : public ports::IEntityList<Tag> {
  public:
    explicit Tracks(const ports::ProviderSetup& s) : client_(s) {}
    domain::Result<ports::CatalogPage<domain::Track>> list(const ports::ProviderContext& c,
                                                           const domain::PlatformRef& ref,
                                                           const ports::ListQuery& q) override {
        return checked([&] {
            const auto start = offset(q.cursor, q.limit, true);
            constexpr auto kind = std::is_same_v<Tag, ports::catalog_tag::PlaylistTracks>
                                      ? "special"
                                  : std::is_same_v<Tag, ports::catalog_tag::AlbumTracks> ? "album"
                                                                                         : "rank";
            auto meta = entity_data(client_, c, ref, kind, Tag::key);
            auto data =
                client_.api(Tag::key,
                            std::string{kind} + "/song?" + kind + "id=" + ref.id.value() +
                                "&version=9108&area_code=1&plat=0&ranktype=0&with_res_tag=0" +
                                window(start, q.limit),
                            c);
            const auto total = number(data, "total");
            const auto rev = digest(Json{ref.id.value(), total, text(meta, "publishtime"),
                                         text(meta, "rank_id_publish_date"), text(meta, "issue")}
                                        .dump());
            same_revision(q.cursor, rev);
            const auto& source = data.at("info");
            parsing::array(source, q.limit);
            std::vector<domain::Track> tracks;
            std::uint64_t unavailable = 0;
            for (const auto& j : source) {
                const auto id = parsing::first(j, {"FileHash", "hash"});
                // 已返回条目但明确没有可用 hash：保留原始位置并报告不可用数量。
                if (id.empty() || id == "00000000000000000000000000000000") {
                    ++unavailable;
                    continue;
                }
                auto song = parsing::track(j);
                if constexpr (std::is_same_v<Tag, ports::catalog_tag::AlbumTracks>) {
                    const auto album = parsing::album(meta);
                    song.album = domain::AlbumSummary{album.title, album.ref};
                    if (!song.artwork)
                        song.artwork = album.artwork;
                    if (j.contains("trans_param") && j.at("trans_param").contains("sort")) {
                        auto n = number(j.at("trans_param"), "sort");
                        if (n > 0 && n <= 10000)
                            song.track_number = static_cast<std::uint32_t>(n);
                    }
                    if (total <= 10000)
                        song.track_total = static_cast<std::uint32_t>(total);
                }
                tracks.push_back(std::move(song));
            }
            auto result = page(std::move(tracks), start, q.limit, total, rev);
            if (source.size() == std::min<std::uint64_t>(q.limit, total - start))
                result.page.page.unavailable_count = unavailable;
            return result;
        });
    }

  private:
    Client client_;
};
class Discovery final : public ports::ICategories,
                        public ports::ICharts,
                        public ports::IRecommendedPlaylists,
                        public ports::CategoryPlaylists {
  public:
    explicit Discovery(const ports::ProviderSetup& s) : client_(s) {}
    domain::Result<std::vector<domain::Category>>
    categories(const ports::ProviderContext& c) override {
        return checked([&] {
            auto doc = client_.get(
                "categories", "http://mobilecdnbj.kugou.com/api/v3/tag/list?pid=0&apiver=2&plat=0",
                c);
            api_success(doc);
            const auto& groups = doc.at("data").at("info");
            parsing::array(groups, 100);
            std::vector<domain::Category> out;
            std::set<std::string> seen;
            for (const auto& group : groups) {
                auto add = [&](const Json& j) {
                    auto id = text(j, "id", true), tag = text(j, "special_tag_id");
                    if (tag.empty())
                        tag = "0";
                    if (!decimal(id) || !decimal(tag, true))
                        throw std::invalid_argument{"category id"};
                    id += ":" + tag;
                    if (!seen.insert(id).second)
                        throw std::invalid_argument{"duplicate category"};
                    out.push_back({id, text(j, "name", true), text(group, "name", true),
                                   domain::ListedKind::playlist});
                    if (out.size() > 1000)
                        throw std::invalid_argument{"category limit"};
                };
                add(group);
                const auto& children = group.at("children");
                parsing::array(children, 200);
                for (const auto& child : children)
                    add(child);
            }
            return out;
        });
    }
    domain::Result<std::vector<domain::Chart>> charts(const ports::ProviderContext& c) override {
        return checked([&] {
            std::vector<domain::Chart> out;
            std::set<std::string> ids;
            for (const auto& j : charts_data(client_, c, "charts")) {
                auto chart = parsing::chart(j);
                if (!ids.insert(chart.ref.id.value()).second)
                    throw std::invalid_argument{"duplicate chart"};
                out.push_back(std::move(chart));
            }
            return out;
        });
    }
    domain::Result<ports::CatalogPage<domain::Playlist>> list(const ports::ProviderContext& c,
                                                              const domain::PlatformRef& ref,
                                                              const ports::ListQuery& q) override {
        return checked([&] {
            const auto start = offset(q.cursor, q.limit, true);
            const auto& id = ref.id.value();
            const auto sep = id.find(':');
            if (ref.source.value() != "kugou" || sep == id.npos || !decimal(id.substr(0, sep)) ||
                !decimal(id.substr(sep + 1), true))
                throw Error::validation("category_id",
                                        "must contain a Kugou category id and tag id");
            auto doc = client_.get(
                "category_playlists",
                "http://mobilecdnbj.kugou.com/api/v3/tag/specialList?plat=0&ugc=1&sort=2&id=" +
                    id.substr(0, sep) + "&tagid=" + id.substr(sep + 1) + window(start, q.limit),
                c);
            api_success(doc);
            const auto& data = doc.at("data");
            const auto total = number(data, "total"), rev_total = total;
            const auto rev = digest(Json{id, rev_total}.dump());
            same_revision(q.cursor, rev);
            return page(items<domain::Playlist>(data.at("info"), q.limit), start, q.limit, total,
                        rev);
        });
    }
    domain::Result<ports::CatalogPage<domain::Playlist>>
    recommend_playlists(const ports::ProviderContext& c, const ports::ListQuery& q) override {
        return checked([&] {
            const auto start = offset(q.cursor, q.limit, true);
            auto fetch = [&](std::uint32_t upstream_page) {
                auto doc = client_.get("recommend_playlists",
                                       "https://m.kugou.com/plist/index&json=true&page=" +
                                           std::to_string(upstream_page),
                                       c);
                const auto& data = doc.at("plist");
                if (number(data, "pagesize") != 30)
                    throw std::invalid_argument{"recommendation window"};
                parsing::array(data.at("list").at("info"), 30);
                return data.at("list");
            };
            const auto first = fetch(1);
            const auto total = number(first, "total");
            if (start > total)
                throw Error{
                    ErrorCode::cursor_mismatch, "recommendation offset exceeds set", false, {}};
            Json ids = Json::array();
            for (const auto& j : first.at("info"))
                ids.push_back(text(j, "specialid", true));
            const auto rev = digest(Json{total, ids}.dump());
            same_revision(q.cursor, rev);
            const auto end = std::min<std::uint64_t>(start + q.limit, total);
            std::vector<domain::Playlist> out;
            for (auto pos = start; pos < end;) {
                const auto upstream_page = pos / 30 + 1;
                auto data = upstream_page == 1 ? first : fetch(upstream_page);
                if (number(data, "total") != total)
                    throw Error{
                        ErrorCode::cursor_expired, "Kugou recommendations changed", false, {}};
                const auto& list = data.at("info");
                const auto count = std::min<std::uint64_t>(end - pos, 30 - pos % 30);
                if (pos % 30 + count > list.size())
                    throw std::invalid_argument{"short recommendation window"};
                for (std::uint64_t i = 0; i < count; ++i)
                    out.push_back(parsing::playlist(list.at(pos % 30 + i)));
                pos += static_cast<std::uint32_t>(count);
            }
            return page(std::move(out), start, q.limit, total, rev);
        });
    }

  private:
    Client client_;
};
} // namespace
void add_catalog(ports::ProviderBundle& b) {
    bind<ports::ITrackSearch, TrackSearch>(b);
    bind<ports::PlaylistSearch, Search<ports::catalog_tag::SearchPlaylists>>(b);
    bind<ports::AlbumSearch, Search<ports::catalog_tag::SearchAlbums>>(b);
    bind<ports::PlaylistDetail, Detail<ports::catalog_tag::PlaylistDetail>>(b);
    bind<ports::AlbumDetail, Detail<ports::catalog_tag::AlbumDetail>>(b);
    bind<ports::ChartDetail, Detail<ports::catalog_tag::ChartDetail>>(b);
    bind<ports::PlaylistTracks, Tracks<ports::catalog_tag::PlaylistTracks>>(b);
    bind<ports::AlbumTracks, Tracks<ports::catalog_tag::AlbumTracks>>(b);
    bind<ports::ChartTracks, Tracks<ports::catalog_tag::ChartTracks>>(b);
    bind<ports::ICategories, Discovery>(b);
    bind<ports::ICharts, Discovery>(b);
    bind<ports::IRecommendedPlaylists, Discovery>(b);
    bind<ports::CategoryPlaylists, Discovery>(b);
}
} // namespace music_service::providers::kugou
