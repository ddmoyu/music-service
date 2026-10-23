#include "providers/jamendo/parsing.hpp"
#include "application/transport/url.hpp"
#include <algorithm>
#include <type_traits>

namespace music_service::providers::jamendo {
namespace {
template<class Item> Item parse_item(const Json& value) {
    if constexpr (std::is_same_v<Item, domain::Track>) return parsing::track(value);
    else if constexpr (std::is_same_v<Item, domain::Album>) return parsing::album(value);
    else if constexpr (std::is_same_v<Item, domain::Artist>) return parsing::artist(value);
    else return parsing::playlist(value);
}
template<class Item> ports::CatalogPage<Item> open_page(const Json& data, std::uint32_t start,
    std::uint32_t limit, std::string_view expected_type) {
    if (data.size() > limit + 1) throw std::invalid_argument{"page limit"};
    ports::CatalogPage<Item> result;
    const auto consumed = std::min<std::size_t>(data.size(), limit);
    for (std::size_t i = 0; i < consumed; ++i) {
        const auto& row = data[i];
        if (parsing::text(row, "type", true) != expected_type) throw std::invalid_argument{"search type"};
        if (parsing::available(row)) result.page.items.push_back(parse_item<Item>(row));
    }
    const bool more = data.size() > limit;
    if (more && start + limit >= max_collection)
        throw Error{ErrorCode::upstream_failed, "Jamendo pagination exceeds the supported bound", false, {}};
    if (more) result.next_cursor = std::to_string(start + limit);
    result.page.page = {limit, static_cast<std::uint32_t>(result.page.items.size()), {}, more, {},
                        consumed - result.page.items.size()};
    return result;
}
template<class Item> ports::CatalogPage<Item> search_page(const Client& client, const ports::ProviderContext& context,
    const ports::SearchQuery& query) {
    const auto start = offset({query.limit, query.cursor});
    check_search(query.text);
    constexpr auto type = std::is_same_v<Item, domain::Track> ? "track" : std::is_same_v<Item, domain::Album> ? "album" : "artist";
    constexpr auto operation = std::is_same_v<Item, domain::Track> ? "search_tracks" : std::is_same_v<Item, domain::Album> ? "search_albums" : "search_artists";
    auto data = client.get(operation, "/api/search", "query=" + application::transport::encode_query_component(query.text) +
        "&type=" + type + "&identities=www&limit=" + std::to_string(query.limit + 1) + "&offset=" + std::to_string(start), context);
    // 多取一个观察项判断下一页；上游未声明总数时保持未知。
    return open_page<Item>(data, start, query.limit, type);
}
class TrackSearch final : public ports::ITrackSearch {
public:
    explicit TrackSearch(const ports::ProviderSetup& setup) : client_(setup) {}
    domain::Result<ports::TrackSearchPage> search_tracks(const ports::ProviderContext& c, const ports::TrackSearchQuery& q) override {
        return checked([&] { return search_page<domain::Track>(client_, c, q); });
    }
private: Client client_;
};
template<class Tag> class Search final : public ports::ICatalogSearch<Tag> {
public:
    explicit Search(const ports::ProviderSetup& setup) : client_(setup) {}
    domain::Result<ports::CatalogPage<typename Tag::Item>> search(const ports::ProviderContext& c, const ports::SearchQuery& q) override {
        return checked([&] { return search_page<typename Tag::Item>(client_, c, q); });
    }
private: Client client_;
};
template<class Tag> class Detail final : public ports::IEntityDetail<Tag> {
public:
    explicit Detail(const ports::ProviderSetup& setup) : client_(setup) {}
    domain::Result<typename Tag::Item> detail(const ports::ProviderContext& c, const domain::PlatformRef& ref) override {
        return checked([&] {
            constexpr bool album = std::is_same_v<typename Tag::Item, domain::Album>;
            constexpr bool artist = std::is_same_v<typename Tag::Item, domain::Artist>;
            auto value = client_.one(Tag::key, album ? "albums" : artist ? "artists" : "playlists", ref, c);
            if constexpr (album) return parsing::album_detail(client_, c, Tag::key, value);
            else return parse_item<typename Tag::Item>(value);
        });
    }
private: Client client_;
};
template<class Tag> class List final : public ports::IEntityList<Tag> {
public:
    explicit List(const ports::ProviderSetup& setup) : client_(setup) {}
    domain::Result<ports::CatalogPage<typename Tag::Item>> list(const ports::ProviderContext& c,
        const domain::PlatformRef& ref, const ports::ListQuery& q) override {
        return checked([&] {
            constexpr bool albums = std::is_same_v<Tag, ports::catalog_tag::ArtistAlbums>;
            constexpr bool album_tracks = std::is_same_v<Tag, ports::catalog_tag::AlbumTracks>;
            const auto start = offset(q, true);
            if constexpr (albums) if (q.limit > 50) throw Error::validation("limit", "must be between 1 and 50");
            auto parent = client_.one(Tag::key, albums ? "artists" : album_tracks ? "albums" : "playlists", ref, c);
            auto members = parsing::members(parent, albums ? "albums" : "tracks", !albums);
            const auto revision = digest(Json{Tag::key, ref.id.value(), members}.dump());
            same_revision(q, revision);
            if (start > members.size()) throw Error{ErrorCode::cursor_mismatch, "Jamendo offset exceeds collection", false, {}};
            const auto end = std::min<std::size_t>(start + q.limit, members.size());
            std::vector<std::string> ids;
            for (std::size_t i = start; i < end; ++i) ids.push_back(parsing::id(members[i]));
            auto records = client_.batch(Tag::key, albums ? "albums" : "tracks", ids, c);
            ports::CatalogPage<typename Tag::Item> result;
            std::vector<Json> rows;
            for (const auto& id : ids) {
                auto it = records.find(id);
                if (it == records.end() || !parsing::available(it->second)) continue;
                if constexpr (albums) {
                    if (parsing::id(it->second, "artistId") != ref.id.value()) throw std::invalid_argument{"album owner"};
                    auto item = parsing::album(it->second);
                    item.artists = {{parsing::artist(parent).name, ref}};
                    result.page.items.push_back(std::move(item));
                } else {
                    if constexpr (album_tracks)
                        if (parsing::id(it->second, "albumId") != ref.id.value()) throw std::invalid_argument{"track album"};
                    rows.push_back(it->second);
                }
            }
            if constexpr (!albums) result.page.items = parsing::tracks(client_, c, Tag::key, rows, album_tracks ? parent : Json());
            if (end < members.size()) result.next_cursor = std::to_string(end) + ":" + revision;
            const auto count = static_cast<std::uint32_t>(result.page.items.size());
            result.page.page = {q.limit, count, members.size(), end < members.size(), members.size(), end - start - count};
            return result;
        });
    }
private: Client client_;
};
class Recommendations final : public ports::IRecommendedPlaylists {
public:
    explicit Recommendations(const ports::ProviderSetup& setup) : client_(setup) {}
    domain::Result<ports::CatalogPage<domain::Playlist>> recommend_playlists(const ports::ProviderContext& c, const ports::ListQuery& q) override {
        return checked([&] {
            const auto start = offset(q);
            auto data = client_.get("recommend_playlists", "/api/playlists/featured",
                "limit=" + std::to_string(q.limit + 1) + "&offset=" + std::to_string(start), c);
            return open_page<domain::Playlist>(data, start, q.limit, "playlist");
        });
    }
private: Client client_;
};
}
void add_catalog(ports::ProviderBundle& bundle) {
    bind<ports::ITrackSearch, TrackSearch>(bundle);
    bind<ports::AlbumSearch, Search<ports::catalog_tag::SearchAlbums>>(bundle);
    bind<ports::ArtistSearch, Search<ports::catalog_tag::SearchArtists>>(bundle);
    bind<ports::AlbumDetail, Detail<ports::catalog_tag::AlbumDetail>>(bundle);
    bind<ports::ArtistDetail, Detail<ports::catalog_tag::ArtistDetail>>(bundle);
    bind<ports::PlaylistDetail, Detail<ports::catalog_tag::PlaylistDetail>>(bundle);
    bind<ports::AlbumTracks, List<ports::catalog_tag::AlbumTracks>>(bundle);
    bind<ports::PlaylistTracks, List<ports::catalog_tag::PlaylistTracks>>(bundle);
    bind<ports::ArtistAlbums, List<ports::catalog_tag::ArtistAlbums>>(bundle);
    bind<ports::IRecommendedPlaylists, Recommendations>(bundle);
}
}
