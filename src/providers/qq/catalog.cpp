#include "providers/qq/bundle.hpp"
#include "providers/qq/parsing.hpp"
#include "application/transport/url.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <type_traits>

namespace music_service::providers::qq {
namespace {
using parsing::text;
using parsing::number;
using application::transport::encode_query_component;
template<class Item> Item item(const Json& j) {
    if constexpr (std::is_same_v<Item, domain::Track>) return parsing::track(j);
    else if constexpr (std::is_same_v<Item, domain::Playlist>) return parsing::playlist(j);
    else if constexpr (std::is_same_v<Item, domain::Album>) return parsing::album(j);
    else return parsing::artist(j);
}
void list_shape(const Json& list, std::size_t max) {
    if (!list.is_array() || list.size() > max) throw std::invalid_argument{"list limit"};
}
template<class Item> std::vector<Item> items(const Json& list, std::size_t max = 50) {
    list_shape(list, max);
    std::vector<Item> out;
    for (const auto& j : list) out.push_back(item<Item>(j));
    return out;
}
template<class Item> ports::CatalogPage<Item> page(std::vector<Item> values, std::uint32_t start,
    std::uint32_t limit, std::uint64_t total, std::uint32_t consumed, std::string rev = {}) {
    if (consumed > limit || values.size() > consumed || (consumed == 0 && total > start))
        throw std::invalid_argument{"inconsistent page"};
    const bool more = start + consumed < total && start + consumed <= 10000;
    ports::CatalogPage<Item> result;
    result.page.items = std::move(values);
    const auto returned = static_cast<std::uint32_t>(result.page.items.size());
    result.page.page = {limit, returned, total, more, {}, consumed - returned};
    if (more) result.next_cursor = std::to_string(start + consumed) + (rev.empty() ? "" : ":" + rev);
    return result;
}
template<class Item> ports::CatalogPage<Item> window_page(std::vector<Item> values, std::uint32_t start,
    std::uint32_t limit, std::uint64_t total, std::string rev = {}) {
    if (start > total) throw std::invalid_argument{"page offset exceeds total"};
    const auto consumed = static_cast<std::uint32_t>(std::min<std::uint64_t>(limit, total - start));
    const bool filtered = values.size() < consumed;
    // begin/page_num 是上游窗口位置。返回条目可能被过滤，不能按可见条目数推进。
    auto result = page(std::move(values), start, limit, total, consumed, std::move(rev));
    // 没有原始 ID 清单时，不能把短页推断为已知数量的不可用歌曲。
    if (filtered) result.page.page.unavailable_count.reset();
    return result;
}
void same_revision(const std::optional<std::string>& cursor, const std::string& current) {
    if (cursor && revision(cursor) != current)
        throw Error{ErrorCode::cursor_expired, "QQ collection changed; restart pagination", false, {}};
}
template<class Item> ports::CatalogPage<Item> search_page(const Client& client,
    const ports::ProviderContext& c, const ports::SearchQuery& q) {
    const auto start = offset(q.cursor, q.limit);
    if (start % q.limit != 0 || (q.cursor && q.cursor->find(':') != q.cursor->npos))
        throw Error{ErrorCode::cursor_mismatch, "invalid QQ search page", false, {}};
    if (q.text.empty() || q.text.size() > 200 || !domain::is_valid_utf8(q.text))
        throw Error::validation("query", "must contain 1 to 200 UTF-8 bytes");
    constexpr int type = std::is_same_v<Item, domain::Track> ? 0 : std::is_same_v<Item, domain::Artist> ? 1 : std::is_same_v<Item, domain::Album> ? 2 : 3;
    constexpr auto field = type == 0 ? "song" : type == 1 ? "singer" : type == 2 ? "album" : "songlist";
    constexpr auto op = type == 0 ? "search_tracks" : type == 1 ? "search_artists" : type == 2 ? "search_albums" : "search_playlists";
    if (!c.secret) throw Error{ErrorCode::credentials_required, "QQ search requires a signed-in cookie", false, {}};
    // 此搜索接口未登录时也会 code=0 且空列表；先验证登录，避免伪报“没有结果”。
    auto account = client.rpc(op, "music.UserInfo.userInfoServer", "GetLoginUserInfo", Json::object(), c);
    if (number(account.at("info"), "retCode") != 0 || text(account.at("info"), "nick").empty())
        throw Error{ErrorCode::credentials_required, "QQ login is expired", false, {}};
    auto data = client.rpc(op, "music.search.SearchCgiService", "DoSearchForQQMusicDesktop",
        {{"query", q.text}, {"search_type", type}, {"page_num", start / q.limit + 1}, {"num_per_page", q.limit}}, c);
    const auto& meta = data.at("meta");
    if (number(meta, "curpage") != start / q.limit + 1 || number(meta, "perpage") != q.limit)
        throw std::invalid_argument{"search page identity"};
    const auto& list = data.at("body").at(field).at("list");
    auto values = items<Item>(list, q.limit);
    return window_page(std::move(values), start, q.limit, number(meta, "sum"));
}
class TrackSearch final : public ports::ITrackSearch {
public:
    explicit TrackSearch(const ports::ProviderSetup& s) : client_(s) {}
    domain::Result<ports::TrackSearchPage> search_tracks(const ports::ProviderContext& c, const ports::TrackSearchQuery& q) override {
        return checked([&] { return search_page<domain::Track>(client_, c, q); });
    }
private: Client client_;
};
template<class Tag> class Search final : public ports::ICatalogSearch<Tag> {
public:
    explicit Search(const ports::ProviderSetup& s) : client_(s) {}
    domain::Result<ports::CatalogPage<typename Tag::Item>> search(const ports::ProviderContext& c, const ports::SearchQuery& q) override {
        return checked([&] { return search_page<typename Tag::Item>(client_, c, q); });
    }
private: Client client_;
};
Json playlist_data(const Client& client, const ports::ProviderContext& c, const domain::PlatformRef& ref,
                   std::string_view op, std::uint32_t start, std::uint32_t limit) {
    check_ref(ref, true);
    return client.rpc(op, "music.srfDissInfo.aiDissInfo", "uniform_get_Dissinfo",
        {{"disstid", std::stoull(ref.id.value())}, {"userinfo", 1}, {"tag", 1}, {"orderlist", 1}, {"song_begin", start}, {"song_num", limit}}, c);
}
Json chart_data(const Client& client, const ports::ProviderContext& c, const domain::PlatformRef& ref,
                std::string_view op, std::uint32_t start, std::uint32_t limit) {
    check_ref(ref, true);
    return client.rpc(op, "musicToplist.ToplistInfoServer", "GetDetail",
        {{"topid", std::stoull(ref.id.value())}, {"offset", start}, {"num", limit}}, c, false);
}
template<class Tag> class Detail final : public ports::IEntityDetail<Tag> {
public:
    explicit Detail(const ports::ProviderSetup& s) : client_(s) {}
    domain::Result<typename Tag::Item> detail(const ports::ProviderContext& c, const domain::PlatformRef& ref) override {
        return checked([&]() -> typename Tag::Item {
            typename Tag::Item result = [&]() -> typename Tag::Item {
                if constexpr (std::is_same_v<typename Tag::Item, domain::Playlist>)
                    return parsing::playlist(playlist_data(client_, c, ref, Tag::key, 0, 1).at("dirinfo"));
                else if constexpr (std::is_same_v<typename Tag::Item, domain::Chart>)
                    return parsing::chart(chart_data(client_, c, ref, Tag::key, 0, 1).at("data"));
                else {
                    check_ref(ref);
                    auto doc = client_.rpc(Tag::key, "music.musichallAlbum.AlbumInfoServer", "GetAlbumDetail", {{"albumMid", ref.id.value()}}, c);
                    auto basic = doc.at("basicInfo"); basic["singer_list"] = doc.at("singer").at("singerList");
                    return parsing::album(basic);
                }
            }();
            if (result.ref != ref) throw Error{ErrorCode::not_found, "QQ entity was not found", false, {}};
            return result;
        });
    }
private: Client client_;
};
template<class Tag> class List final : public ports::IEntityList<Tag> {
public:
    explicit List(const ports::ProviderSetup& s) : client_(s) {}
    domain::Result<ports::CatalogPage<typename Tag::Item>> list(const ports::ProviderContext& c,
        const domain::PlatformRef& ref, const ports::ListQuery& q) override {
        return checked([&] {
            const auto start = offset(q.cursor, q.limit);
            Json list; std::uint64_t total{}; std::string rev;
            if constexpr (std::is_same_v<Tag, ports::catalog_tag::PlaylistTracks>) {
                auto doc = playlist_data(client_, c, ref, Tag::key, start, q.limit);
                const auto& info = doc.at("dirinfo");
                if (text(info, "id", true) != ref.id.value()) throw Error{ErrorCode::not_found, "QQ playlist not found", false, {}};
                total = number(info, "songnum");
                rev = digest(Json{ref.id.value(), total, info.value("mtime", Json{}), info.value("song_update_time", Json{})}.dump());
                list = doc.at("songlist");
            } else if constexpr (std::is_same_v<Tag, ports::catalog_tag::ChartTracks>) {
                auto doc = chart_data(client_, c, ref, Tag::key, start, q.limit);
                const auto& info = doc.at("data");
                if (text(info, "topId", true) != ref.id.value()) throw Error{ErrorCode::not_found, "QQ chart not found", false, {}};
                total = number(info, "totalNum");
                rev = digest(Json{ref.id.value(), total, text(info, "period", true), text(info, "updateTime", true)}.dump());
                same_revision(q.cursor, rev);
                const auto& ordered = info.at("song"); list_shape(ordered, q.limit);
                const auto& details = doc.at("songInfoList"); list_shape(details, q.limit);
                std::map<std::uint64_t, Json> by_id;
                for (const auto& v : details) if (!by_id.emplace(number(v, "id"), v).second) throw std::invalid_argument{"duplicate detail"};
                std::vector<domain::Track> tracks;
                for (const auto& v : ordered) {
                    if (auto it = by_id.find(number(v, "songId")); it != by_id.end()) tracks.push_back(parsing::track(it->second));
                }
                return page(std::move(tracks), start, q.limit, total, static_cast<std::uint32_t>(ordered.size()), rev);
            } else if constexpr (std::is_same_v<Tag, ports::catalog_tag::CategoryPlaylists>) {
                check_ref(ref, true);
                auto doc = client_.get(Tag::key, "/splcloud/fcgi-bin/fcg_get_diss_by_tag.fcg?format=json&outCharset=utf-8&picmid=1&g_tk=5381&sortId=5&categoryId=" +
                    ref.id.value() + "&sin=" + std::to_string(start) + "&ein=" + std::to_string(start + q.limit - 1), c).at("data");
                if (text(doc, "categoryId", true) != ref.id.value() || number(doc, "sin") != start) throw std::invalid_argument{"category identity"};
                total = number(doc, "sum"); list = doc.at("list");
                rev = digest(Json{ref.id.value(), total}.dump());
            } else {
                check_ref(ref);
                constexpr bool albums = std::is_same_v<Tag, ports::catalog_tag::ArtistAlbums>;
                constexpr bool artist = albums || std::is_same_v<Tag, ports::catalog_tag::ArtistTracks>;
                constexpr auto module = albums ? "music.musichallAlbum.AlbumListServer" : artist ? "musichall.song_list_server" : "music.musichallAlbum.AlbumSongList";
                constexpr auto method = albums ? "GetAlbumList" : artist ? "GetSingerSongList" : "GetAlbumSongList";
                constexpr auto idkey = artist ? "singerMid" : "albumMid";
                auto doc = client_.rpc(Tag::key, module, method, {{idkey, ref.id.value()}, {"begin", start}, {"num", q.limit}, {"order", artist ? 1 : 2}}, c);
                if (text(doc, idkey, true) != ref.id.value()) throw Error{ErrorCode::not_found, "QQ entity not found", false, {}};
                if (doc.contains("curBegin") && number(doc, "curBegin") != start) throw std::invalid_argument{"collection offset"};
                total = number(doc, albums ? "total" : "totalNum");
                list = Json::array(); const auto& source = doc.at(albums ? "albumList" : "songList"); list_shape(source, q.limit);
                for (const auto& entry : source) { if constexpr (albums) list.push_back(entry); else list.push_back(entry.at("songInfo")); }
                rev = digest(Json{ref.id.value(), total}.dump());
            }
            if constexpr (!std::is_same_v<Tag, ports::catalog_tag::ChartTracks>) {
                same_revision(q.cursor, rev);
                return window_page(items<typename Tag::Item>(list, q.limit), start, q.limit, total, rev);
            }
        });
    }
private: Client client_;
};
class Discovery final : public ports::ICategories, public ports::ICharts, public ports::IRecommendedPlaylists {
public:
    explicit Discovery(const ports::ProviderSetup& s) : client_(s) {}
    domain::Result<std::vector<domain::Category>> categories(const ports::ProviderContext& c) override {
        return checked([&] {
            auto doc = client_.get("categories", "/splcloud/fcgi-bin/fcg_get_diss_tag_conf.fcg?format=json&g_tk=5381&outCharset=utf-8", c);
            const auto& groups = doc.at("data").at("categories"); list_shape(groups, 100);
            std::vector<domain::Category> result; std::set<std::string> ids;
            for (const auto& group : groups) {
                const auto& entries = group.at("items"); list_shape(entries, 500);
                for (const auto& v : entries) {
                    if (v.contains("usable") && number(v, "usable") == 0) continue;
                    auto id = text(v, "categoryId", true);
                    if (!decimal(id) || !ids.insert(id).second) throw std::invalid_argument{"category id"};
                    result.push_back({id, text(v, "categoryName", true), text(group, "categoryGroupName"), domain::ListedKind::playlist});
                    if (result.size() > 1000) throw std::invalid_argument{"category limit"};
                }
            }
            return result;
        });
    }
    domain::Result<std::vector<domain::Chart>> charts(const ports::ProviderContext& c) override {
        return checked([&] {
            auto doc = client_.rpc("charts", "musicToplist.ToplistInfoServer", "GetAll", Json::object(), c, false);
            const auto& groups = doc.at("group"); list_shape(groups, 100);
            std::vector<domain::Chart> result; std::set<std::string> ids;
            for (const auto& group : groups) {
                const auto& entries = group.at("toplist"); list_shape(entries, 200);
                for (const auto& v : entries) {
                    auto entry = parsing::chart(v);
                    if (!ids.insert(entry.ref.id.value()).second) throw std::invalid_argument{"duplicate chart"};
                    result.push_back(std::move(entry));
                    if (result.size() > 1000) throw std::invalid_argument{"chart limit"};
                }
            }
            return result;
        });
    }
    domain::Result<ports::CatalogPage<domain::Playlist>> recommend_playlists(const ports::ProviderContext& c, const ports::ListQuery& q) override {
        return checked([&] {
            const auto start = offset(q.cursor, q.limit);
            auto doc = client_.rpc("recommend_playlists", "playlist.HotRecommendServer", "get_hot_recommend", {{"async", 1}, {"cmd", 2}}, c, false);
            const auto& list = doc.at("v_hot");
            auto all = items<domain::Playlist>(list, 500);
            // 播放量等展示字段持续变化，不应使同一顺序的推荐集合无法翻页。
            Json ordered_ids = Json::array();
            for (const auto& entry : all) ordered_ids.push_back(entry.ref.id.value());
            const auto rev = digest(ordered_ids.dump()); same_revision(q.cursor, rev);
            if (start > all.size()) throw Error{ErrorCode::cursor_mismatch, "recommendation offset exceeds set", false, {}};
            const auto end = std::min<std::size_t>(start + q.limit, all.size());
            std::vector<domain::Playlist> sliced(all.begin() + start, all.begin() + static_cast<std::ptrdiff_t>(end));
            auto result = page(std::move(sliced), start, q.limit, all.size(), static_cast<std::uint32_t>(end - start), rev);
            result.page.page.listed_total = all.size();
            return result;
        });
    }
private: Client client_;
};
template<class Port, class Impl> void bind(ports::ProviderBundle& bundle) {
    bundle.bindings.push_back(ports::CapabilityBinding::bind<Port>([](const ports::ProviderSetup& setup) -> domain::Result<std::shared_ptr<Port>> {
        if (!setup.upstream) return Error{ErrorCode::internal, "QQ requires the upstream client", false, {}};
        return std::shared_ptr<Port>{std::make_shared<Impl>(setup)};
    }));
}
}
void add_catalog(ports::ProviderBundle& b) {
    bind<ports::ITrackSearch, TrackSearch>(b);
    bind<ports::PlaylistSearch, Search<ports::catalog_tag::SearchPlaylists>>(b);
    bind<ports::AlbumSearch, Search<ports::catalog_tag::SearchAlbums>>(b);
    bind<ports::ArtistSearch, Search<ports::catalog_tag::SearchArtists>>(b);
    bind<ports::PlaylistDetail, Detail<ports::catalog_tag::PlaylistDetail>>(b);
    bind<ports::AlbumDetail, Detail<ports::catalog_tag::AlbumDetail>>(b);
    bind<ports::ChartDetail, Detail<ports::catalog_tag::ChartDetail>>(b);
    bind<ports::PlaylistTracks, List<ports::catalog_tag::PlaylistTracks>>(b);
    bind<ports::AlbumTracks, List<ports::catalog_tag::AlbumTracks>>(b);
    bind<ports::ArtistTracks, List<ports::catalog_tag::ArtistTracks>>(b);
    bind<ports::ArtistAlbums, List<ports::catalog_tag::ArtistAlbums>>(b);
    bind<ports::ChartTracks, List<ports::catalog_tag::ChartTracks>>(b);
    bind<ports::CategoryPlaylists, List<ports::catalog_tag::CategoryPlaylists>>(b);
    bind<ports::ICategories, Discovery>(b);
    bind<ports::ICharts, Discovery>(b);
    bind<ports::IRecommendedPlaylists, Discovery>(b);
}
}
