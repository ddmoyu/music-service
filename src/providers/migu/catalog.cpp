#include "application/transport/url.hpp"
#include "providers/migu/bundle.hpp"
#include "providers/migu/parsing.hpp"
#include <algorithm>
#include <set>
#include <type_traits>

namespace music_service::providers::migu {
namespace {
using application::transport::encode_query_component;
using parsing::count;
using parsing::text;

template <class Item> Item item(const Json& value) {
    if constexpr (std::is_same_v<Item, domain::Track>)
        return parsing::track(value);
    else if constexpr (std::is_same_v<Item, domain::Playlist>)
        return parsing::playlist(value);
    else if constexpr (std::is_same_v<Item, domain::Album>)
        return parsing::album(value);
    else
        return parsing::artist(value);
}

// 上游固定窗口与公共 limit 独立；在窗口内续读，越过窗口时按窗口宽度推进。
template <class Item>
ports::CatalogPage<Item> slice(const Json& list, const Cursor& pos, std::uint32_t limit,
                               std::uint32_t base, std::uint32_t width,
                               std::optional<std::uint64_t> total, bool more,
                               std::string revision = {}) {
    array_shape(list, width);
    const auto local = pos.offset - base;
    if ((list.empty() && (pos.offset || more || (total && *total > base))) || local > list.size())
        throw std::invalid_argument{"inconsistent page"};
    if (total && (base > *total || list.size() > *total - base))
        throw std::invalid_argument{"page count"};
    // 已知总数时，窗口缺项不能直接跳到下一页或假称遍历结束；明确的空槽仍计入位置。
    if (total && list.size() != std::min<std::uint64_t>(width, *total - base))
        throw std::invalid_argument{"incomplete page window"};
    ports::CatalogPage<Item> result;
    const auto end = std::min(static_cast<std::size_t>(local) + limit, list.size());
    std::uint64_t unavailable = 0;
    for (std::size_t i = local; i < end; ++i) {
        if (list[i].is_null())
            ++unavailable;
        else
            result.page.items.push_back(item<Item>(list[i]));
    }
    const bool within = end < list.size();
    const auto next = within ? base + static_cast<std::uint32_t>(end) : base + width;
    if ((within || more) && next <= 10000)
        result.next_cursor = std::to_string(next) + (revision.empty() ? "" : ":" + revision);
    result.page.page = {limit, static_cast<std::uint32_t>(result.page.items.size()), total,
                        result.next_cursor.has_value()};
    if (unavailable)
        result.page.page.unavailable_count = unavailable;
    return result;
}

template <class Item>
ports::CatalogPage<Item> search(const Client& client, const ports::ProviderContext& c,
                                const ports::SearchQuery& query, std::string_view op) {
    auto pos = cursor(query.cursor, query.limit);
    if (!pos.revision.empty())
        throw Error{ErrorCode::cursor_mismatch, "invalid Migu search cursor", false, {}};
    if (query.text.empty() || query.text.size() > 1024 || !domain::is_valid_utf8(query.text) ||
        query.text.find('\0') != query.text.npos)
        throw Error::validation("q", "Migu search text must be valid UTF-8 and 1 to 1024 bytes");
    constexpr auto type = std::is_same_v<Item, domain::Track>      ? "song"
                          : std::is_same_v<Item, domain::Playlist> ? "songlist"
                          : std::is_same_v<Item, domain::Album>    ? "album"
                                                                   : "singer";
    constexpr auto field = std::is_same_v<Item, domain::Track>      ? "songResultData"
                           : std::is_same_v<Item, domain::Playlist> ? "songListResultData"
                           : std::is_same_v<Item, domain::Album>    ? "albumResultData"
                                                                    : "singerResultData";
    Json switches{{"song", 0},   {"album", 0},    {"singer", 0},  {"tagSong", 0},
                  {"mvSong", 0}, {"songlist", 0}, {"bestShow", 0}};
    switches[type] = 1;
    const auto base = pos.offset / 20 * 20;
    auto doc =
        client
            .get(op,
                 "https://pd.musicapp.migu.cn/MIGUM2.0/v1.0/content/"
                 "search_all.do?ua=Android_migu&version=5.0.1&pageSize=20&pageNo=" +
                     std::to_string(base / 20 + 1) + "&text=" + encode_query_component(query.text) +
                     "&searchSwitch=" + encode_query_component(switches.dump()),
                 c)
            .at(field);
    auto total = count(doc, "totalCount");
    return slice<Item>(doc.at("result"), pos, query.limit, base, 20, total, base + 20 < total);
}
class Tracks final : public ports::ITrackSearch {
  public:
    explicit Tracks(const ports::ProviderSetup& s) : client_(s) {}
    domain::Result<ports::TrackSearchPage>
    search_tracks(const ports::ProviderContext& c, const ports::TrackSearchQuery& q) override {
        return checked([&] { return search<domain::Track>(client_, c, q, "search_tracks"); });
    }

  private:
    Client client_;
};
template <class Tag> class Search final : public ports::ICatalogSearch<Tag> {
  public:
    explicit Search(const ports::ProviderSetup& s) : client_(s) {}
    domain::Result<ports::CatalogPage<typename Tag::Item>>
    search(const ports::ProviderContext& c, const ports::SearchQuery& q) override {
        return checked([&] { return migu::search<typename Tag::Item>(client_, c, q, Tag::key); });
    }

  private:
    Client client_;
};

Json chart_data(const Client& client, std::string_view op, const domain::PlatformRef& ref,
                std::uint32_t page, const ports::ProviderContext& c) {
    check_ref(ref);
    auto doc = client
                   .get(op,
                        std::string{api} + "/bmw/rank/rank-info/v1.0?rankId=" + ref.id.value() +
                            "&pageNo=" + std::to_string(page),
                        c)
                   .at("data");
    if (text(doc, "columnId", true) != ref.id.value())
        throw Error{ErrorCode::not_found, "Migu chart was not found", false, {}};
    return doc;
}
template <class Tag> class Detail final : public ports::IEntityDetail<Tag> {
  public:
    explicit Detail(const ports::ProviderSetup& s) : client_(s) {}
    domain::Result<typename Tag::Item> detail(const ports::ProviderContext& c,
                                              const domain::PlatformRef& ref) override {
        return checked([&]() -> typename Tag::Item {
            if constexpr (std::is_same_v<typename Tag::Item, domain::Chart>)
                return parsing::chart(chart_data(client_, Tag::key, ref, 1, c));
            else if constexpr (std::is_same_v<typename Tag::Item, domain::Album>)
                return parsing::album(client_.resource(Tag::key, ref, "2003", c));
            else
                return parsing::playlist(client_.resource(Tag::key, ref, "2021", c));
        });
    }

  private:
    Client client_;
};

template <class Tag> class List final : public ports::IEntityList<Tag> {
  public:
    explicit List(const ports::ProviderSetup& s) : client_(s) {}
    domain::Result<ports::CatalogPage<typename Tag::Item>>
    list(const ports::ProviderContext& c, const domain::PlatformRef& ref,
         const ports::ListQuery& q) override {
        return checked([&] {
            auto pos = cursor(q.cursor, q.limit);
            constexpr bool album = std::is_same_v<Tag, ports::catalog_tag::AlbumTracks>;
            constexpr bool chart = std::is_same_v<Tag, ports::catalog_tag::ChartTracks>;
            check_ref(ref, album);
            const auto base = chart ? 0U : pos.offset / 50 * 50;
            constexpr auto width = chart ? 1000U : 50U;
            Json doc, values;
            std::string updated;
            if constexpr (chart) {
                // rank-info 返回完整榜单快照（例如 100 首），忽略 pageNo 的续页含义。
                doc = chart_data(client_, Tag::key, ref, 1, c);
                const auto& rows = doc.at("contents");
                array_shape(rows, width);
                if (doc.at("hasNextPage").get<bool>() || count(doc, "totalCount") != rows.size())
                    throw std::invalid_argument{"incomplete chart snapshot"};
                values = Json::array();
                Json ids = Json::array();
                for (const auto& row : rows) {
                    ids.push_back(text(row, "resId"));
                    const auto data = text(row, "songData");
                    if (data.empty()) {
                        values.push_back(nullptr);
                        continue;
                    }
                    auto song = parse_json(data);
                    if (text(song, "contentId", true) != text(row, "resId", true))
                        throw std::invalid_argument{"chart song identity"};
                    values.push_back(std::move(song));
                }
                updated = text(doc, "updateTime") + ":" + digest(ids.dump());
            } else {
                auto id = ref.id.value();
                if constexpr (album) {
                    auto info = client_.resource(Tag::key, ref, "2003", c);
                    if (id.starts_with("d:"))
                        id = text(info, "materialId", true);
                    if (!decimal(id))
                        throw std::invalid_argument{"album material identity"};
                    updated = parsing::first(info, {"publishDate", "publishTime"});
                }
                auto path = album ? "/MIGUM2.0/v1.0/content/queryAlbumSong?albumId="
                                  : "/MIGUM3.0/resource/playlist/song/v2.0?playlistId=";
                doc = client_
                          .get(Tag::key,
                               std::string{api} + path + id +
                                   "&pageSize=50&pageNo=" + std::to_string(base / 50 + 1),
                               c)
                          .at("data");
                if (doc.empty())
                    throw Error{ErrorCode::not_found, "Migu collection was not found", false, {}};
                values = doc.at("songList");
                if constexpr (!album)
                    updated = text(doc, "publishTime");
            }
            const auto total = count(doc, "totalCount");
            auto rev = digest(Json{ref.id.value(), total, updated}.dump());
            same_revision(pos, rev);
            const bool more = !chart && base + width < total;
            return slice<domain::Track>(values, pos, q.limit, base, width, total, more, rev);
        });
    }

  private:
    Client client_;
};

void recommendations(const Json& node, Json& out, unsigned depth = 0) {
    if (depth > 12 || out.size() > 1000)
        throw std::invalid_argument{"recommendation tree limit"};
    if (node.is_array()) {
        array_shape(node, 1000);
        for (const auto& child : node)
            recommendations(child, out, depth + 1);
    } else if (node.is_object()) {
        // 场景模块每次请求随机换一组，不能和编辑／专题／上新目录共用顺序游标。
        if (text(node, "view") == "playlist_square_scene_zone")
            return;
        if (text(node, "view") == "ZJ-SongList-Item" && text(node, "resType") == "2021")
            out.push_back(node);
        else if (node.contains("contents"))
            recommendations(node.at("contents"), out, depth + 1);
    } else
        throw std::invalid_argument{"recommendation node"};
}
class Discovery final : public ports::ICategories,
                        public ports::ICharts,
                        public ports::IRecommendedPlaylists,
                        public ports::CategoryPlaylists {
  public:
    explicit Discovery(const ports::ProviderSetup& s) : client_(s) {}
    domain::Result<std::vector<domain::Category>>
    categories(const ports::ProviderContext& c) override {
        return checked([&] {
            auto data =
                client_
                    .get("categories",
                         std::string{api} + "/pc/v1.0/template/musiclistplaza-taglist/release", c)
                    .at("data");
            array_shape(data, 50);
            std::set<std::string> seen;
            std::vector<domain::Category> out;
            for (const auto& group : data) {
                auto name = text(group.at("header"), "title", true);
                const auto& values = group.at("content");
                array_shape(values, 200);
                for (const auto& entry : values) {
                    const auto& fields = entry.at("texts");
                    array_shape(fields, 3);
                    if (fields.size() < 2)
                        throw std::invalid_argument{"category fields"};
                    auto id = fields[1].get<std::string>(), title = fields[0].get<std::string>();
                    if (!decimal(id) || title.empty() || title.size() > 256)
                        throw std::invalid_argument{"category identity"};
                    if (seen.insert(id).second)
                        out.push_back({id, title, name});
                }
            }
            if (out.empty())
                throw std::invalid_argument{"empty category directory"};
            return out;
        });
    }
    domain::Result<std::vector<domain::Chart>> charts(const ports::ProviderContext& c) override {
        return checked([&] {
            auto data =
                client_.get("charts", "https://app.u.nf.migu.cn/bmw/rank/rank-index/v2.0", c)
                    .at("data")
                    .at("contents");
            array_shape(data, 50);
            std::set<std::string> seen;
            std::vector<domain::Chart> out;
            for (const auto& group : data) {
                const auto& values = group.at("contents");
                array_shape(values, 100);
                for (const auto& entry : values) {
                    if (text(entry, "rankId").empty())
                        continue;
                    auto value = parsing::chart(entry);
                    if (seen.insert(value.ref.id.value()).second)
                        out.push_back(std::move(value));
                }
            }
            if (out.empty())
                throw std::invalid_argument{"empty chart directory"};
            return out;
        });
    }
    domain::Result<ports::CatalogPage<domain::Playlist>>
    recommend_playlists(const ports::ProviderContext& c, const ports::ListQuery& q) override {
        return checked([&] {
            auto pos = cursor(q.cursor, q.limit);
            auto data =
                client_
                    .get("recommend_playlists",
                         std::string{api} +
                             "/pc/bmw/page-data/playlist-square-recommend/v1.0?templateVersion=2",
                         c)
                    .at("data");
            Json values = Json::array();
            recommendations(data.at("contents"), values);
            if (values.empty())
                throw std::invalid_argument{"empty recommendations"};
            Json ids = Json::array();
            for (const auto& v : values)
                ids.push_back(text(v, "resId", true));
            auto rev = digest(ids.dump());
            same_revision(pos, rev);
            return slice<domain::Playlist>(values, pos, q.limit, 0, 1000, values.size(), false,
                                           rev);
        });
    }
    domain::Result<ports::CatalogPage<domain::Playlist>> list(const ports::ProviderContext& c,
                                                              const domain::PlatformRef& ref,
                                                              const ports::ListQuery& q) override {
        return checked([&] {
            check_ref(ref);
            auto pos = cursor(q.cursor, q.limit);
            const auto base = pos.offset / 30 * 30;
            auto doc = client_
                           .get("category_playlists",
                                std::string{api} +
                                    "/pc/v1.0/template/musiclistplaza-listbytag/"
                                    "release?templateVersion=2&tagId=" +
                                    ref.id.value() + "&pageNumber=" + std::to_string(base / 30 + 1),
                                c)
                           .at("data");
            const auto& groups = doc.at("contentItemList");
            array_shape(groups, 20);
            Json values = Json::array();
            for (const auto& group : groups)
                if (group.contains("itemList")) {
                    const auto& rows = group.at("itemList");
                    array_shape(rows, 30);
                    for (const auto& row : rows)
                        if (row.contains("barList"))
                            values.push_back(row);
                }
            // 不跟随上游提供的明文 nextPageUrl，只验证其分页含义并重建 HTTPS 请求。
            const auto next = text(doc, "nextPageUrl");
            if (!next.empty()) {
                auto u = application::transport::Url::parse(next);
                const auto suffix = "?templateVersion=2&tagId=" + ref.id.value() +
                                    "&pageNumber=" + std::to_string(base / 30 + 2);
                if (!u || u->host() != "app.c.nf.migu.cn" ||
                    u->target() != "/MIGUM3.0/v1.0/template/musiclistplaza-listbytag" + suffix)
                    throw std::invalid_argument{"category continuation"};
            }
            Json ids = Json::array();
            for (const auto& v : values)
                ids.push_back(text(v.at("logEvent"), "contentId", true));
            const auto rev = digest(ids.dump());
            if (pos.offset % 30)
                same_revision(pos, rev);
            return slice<domain::Playlist>(values, pos, q.limit, base, 30, {}, !next.empty(), rev);
        });
    }

  private:
    Client client_;
};
} // namespace
void add_catalog(ports::ProviderBundle& b) {
    bind<ports::ITrackSearch, Tracks>(b);
    bind<ports::PlaylistSearch, Search<ports::catalog_tag::SearchPlaylists>>(b);
    bind<ports::AlbumSearch, Search<ports::catalog_tag::SearchAlbums>>(b);
    bind<ports::ArtistSearch, Search<ports::catalog_tag::SearchArtists>>(b);
    bind<ports::PlaylistDetail, Detail<ports::catalog_tag::PlaylistDetail>>(b);
    bind<ports::AlbumDetail, Detail<ports::catalog_tag::AlbumDetail>>(b);
    bind<ports::ChartDetail, Detail<ports::catalog_tag::ChartDetail>>(b);
    bind<ports::PlaylistTracks, List<ports::catalog_tag::PlaylistTracks>>(b);
    bind<ports::AlbumTracks, List<ports::catalog_tag::AlbumTracks>>(b);
    bind<ports::ChartTracks, List<ports::catalog_tag::ChartTracks>>(b);
    bind<ports::ICategories, Discovery>(b);
    bind<ports::ICharts, Discovery>(b);
    bind<ports::IRecommendedPlaylists, Discovery>(b);
    bind<ports::CategoryPlaylists, Discovery>(b);
}
} // namespace music_service::providers::migu
