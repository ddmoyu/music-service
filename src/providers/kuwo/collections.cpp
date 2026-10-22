#include "providers/kuwo/collections.hpp"

#include "providers/kuwo/client.hpp"
#include "providers/kuwo/parsing/collections.hpp"
#include "providers/kuwo/parsing/parsers.hpp"
#include "providers/kuwo/protocol/requests.hpp"

#include <type_traits>
#include <utility>
#include <vector>

namespace music_service::providers::kuwo {

namespace {

using domain::Error;
using domain::ErrorCode;
namespace tag = ports::catalog_tag;

constexpr std::size_t max_query_bytes = 200;
constexpr std::uint32_t max_page_size = 100;

// One page of a numbered listing: more pages exist while the claimed total is beyond it.
template <class Item>
ports::CatalogPage<Item> page_of(std::vector<Item> items, std::uint32_t limit, std::uint32_t page,
                                 std::uint64_t total) {
    ports::CatalogPage<Item> result;
    const auto returned = static_cast<std::uint32_t>(items.size());
    const bool more = returned > 0 && static_cast<std::uint64_t>(page + 1) * limit < total;
    result.page.items = std::move(items);
    result.page.page = {limit, returned, total, more};
    if (more) {
        result.next_cursor = std::to_string(page + 1);
    }
    return result;
}

domain::Result<void> check_limit(std::uint32_t limit) {
    if (limit < 1 || limit > max_page_size) {
        return Error::validation("limit", "must be between 1 and 100");
    }
    return domain::ok();
}

domain::Result<std::string> collection_id(const domain::PlatformRef& entity, const char* what) {
    if (entity.source.value() != protocol::source) {
        return Error::validation("id", std::string{"is not a kuwo "} + what);
    }
    if (!parsing::is_valid_collection_id(entity.id.value())) {
        return Error::validation("id", std::string{"is not a kuwo "} + what + " id");
    }
    return entity.id.value();
}

// Playlist (ft=playlist) and album (ft=album) search on the legacy route.
template <class Tag> class Search final : public ports::ICatalogSearch<Tag> {
public:
    explicit Search(const ports::ProviderSetup& setup) : client_(setup) {}

    domain::Result<ports::CatalogPage<typename Tag::Item>> search(const ports::ProviderContext& context,
                                                                  const ports::SearchQuery& query) override {
        const auto text = parsing::decode_text(query.text);
        if (text.empty() || query.text.size() > max_query_bytes) {
            return Error::validation("q", "must be 1 to 200 bytes");
        }
        if (query.limit < 1 || query.limit > protocol::max_search_page_size) {
            return Error::validation("limit", "must be between 1 and 50");
        }
        auto page = page_cursor(query.cursor);
        if (!page) {
            return std::move(page).error();
        }
        constexpr bool playlists = std::is_same_v<typename Tag::Item, domain::Playlist>;
        auto body = client_.body(protocol::search_collections(playlists ? "playlist" : "album", query.text, *page,
                                                              query.limit, Tag::key),
                                 context);
        if (!body) {
            return std::move(body).error();
        }
        auto found = [&] {
            if constexpr (playlists) {
                return parsing::parse_playlist_search(*body, *page);
            } else {
                return parsing::parse_album_search(*body, *page);
            }
        }();
        if (!found) {
            return std::move(found).error();
        }
        return page_of(std::move(found->items), query.limit, *page, found->total);
    }

private:
    Client client_;
};

// Playlist, album and chart pages: the detail is the first page's header, the track
// list one page of the same call.
struct PlaylistSource {
    using Page = parsing::PlaylistPage;
    static constexpr const char* what = "playlist";
    static ports::HttpRequest request(std::string_view id, std::uint32_t page, std::uint32_t size, std::string_view op) {
        return protocol::playlist_page(id, page, size, op);
    }
    static auto parse(std::string_view body, std::string_view id) { return parsing::parse_playlist_page(body, id); }
    static auto& header(parsing::PlaylistPage& page) { return page.playlist; }
};
struct AlbumSource {
    using Page = parsing::AlbumPage;
    static constexpr const char* what = "album";
    static ports::HttpRequest request(std::string_view id, std::uint32_t page, std::uint32_t size, std::string_view op) {
        return protocol::album_page(id, page, size, op);
    }
    static auto parse(std::string_view body, std::string_view id) { return parsing::parse_album_page(body, id); }
    static auto& header(parsing::AlbumPage& page) { return page.album; }
};
struct ChartSource {
    using Page = parsing::ChartPage;
    static constexpr const char* what = "chart";
    static ports::HttpRequest request(std::string_view id, std::uint32_t page, std::uint32_t size, std::string_view op) {
        return protocol::chart_page(id, page, size, op);
    }
    static auto parse(std::string_view body, std::string_view id) { return parsing::parse_chart_page(body, id); }
    static auto& header(parsing::ChartPage& page) { return page.chart; }
};

template <class Source>
domain::Result<typename Source::Page> fetch_page(const Client& client, const ports::ProviderContext& context,
                                                 const domain::PlatformRef& entity, std::uint32_t page,
                                                 std::uint32_t size, std::string_view operation) {
    auto id = collection_id(entity, Source::what);
    if (!id) {
        return std::move(id).error();
    }
    auto body = client.body(Source::request(*id, page, size, operation), context);
    if (!body) {
        return std::move(body).error();
    }
    return Source::parse(*body, *id);
}

template <class Tag, class Source> class Detail final : public ports::IEntityDetail<Tag> {
public:
    explicit Detail(const ports::ProviderSetup& setup) : client_(setup) {}

    domain::Result<typename Tag::Item> detail(const ports::ProviderContext& context,
                                              const domain::PlatformRef& entity) override {
        auto page = fetch_page<Source>(client_, context, entity, 0, 1, Tag::key);
        if (!page) {
            return std::move(page).error();
        }
        return std::move(Source::header(*page));
    }

private:
    Client client_;
};

template <class Tag, class Source> class Tracks final : public ports::IEntityList<Tag> {
public:
    explicit Tracks(const ports::ProviderSetup& setup) : client_(setup) {}

    domain::Result<ports::CatalogPage<domain::Track>> list(const ports::ProviderContext& context,
                                                           const domain::PlatformRef& entity,
                                                           const ports::ListQuery& query) override {
        if (auto valid = check_limit(query.limit); !valid) {
            return valid.error();
        }
        auto page = page_cursor(query.cursor);
        if (!page) {
            return std::move(page).error();
        }
        auto found = fetch_page<Source>(client_, context, entity, *page, query.limit, Tag::key);
        if (!found) {
            return std::move(found).error();
        }
        const auto total = Source::header(*found).track_count.value_or(0);
        return page_of(std::move(found->tracks), query.limit, *page, total);
    }

private:
    Client client_;
};

// wapi pages are 1-based; the cursor stays 0-based like every other Kuwo listing.
domain::Result<ports::CatalogPage<domain::Playlist>> playlist_list(const Client& client,
                                                                   const ports::ProviderContext& context,
                                                                   const ports::ListQuery& query,
                                                                   const auto& request) {
    if (auto valid = check_limit(query.limit); !valid) {
        return valid.error();
    }
    auto page = page_cursor(query.cursor);
    if (!page) {
        return std::move(page).error();
    }
    auto body = client.body(request(*page + 1, query.limit), context);
    if (!body) {
        return std::move(body).error();
    }
    auto found = parsing::parse_playlist_list(*body);
    if (!found) {
        return std::move(found).error();
    }
    return page_of(std::move(found->items), query.limit, *page, found->total);
}

class CategoryPlaylists final : public ports::CategoryPlaylists {
public:
    explicit CategoryPlaylists(const ports::ProviderSetup& setup) : client_(setup) {}

    domain::Result<ports::CatalogPage<domain::Playlist>> list(const ports::ProviderContext& context,
                                                              const domain::PlatformRef& entity,
                                                              const ports::ListQuery& query) override {
        auto id = collection_id(entity, "category");
        if (!id) {
            return std::move(id).error();
        }
        // An unknown category answers an empty page, like an empty one.
        return playlist_list(client_, context, query, [&](std::uint32_t page, std::uint32_t size) {
            return protocol::tag_playlists(*id, page, size);
        });
    }

private:
    Client client_;
};

class Directory final : public ports::ICategories, public ports::ICharts, public ports::IRecommendedPlaylists {
public:
    explicit Directory(const ports::ProviderSetup& setup) : client_(setup) {}

    domain::Result<std::vector<domain::Category>> categories(const ports::ProviderContext& context) override {
        auto body = client_.body(protocol::playlist_tags(), context);
        if (!body) {
            return std::move(body).error();
        }
        return parsing::parse_categories(*body);
    }

    domain::Result<std::vector<domain::Chart>> charts(const ports::ProviderContext& context) override {
        auto body = client_.body(protocol::chart_list(), context);
        if (!body) {
            return std::move(body).error();
        }
        return parsing::parse_charts(*body);
    }

    domain::Result<ports::CatalogPage<domain::Playlist>> recommend_playlists(const ports::ProviderContext& context,
                                                                            const ports::ListQuery& query) override {
        return playlist_list(client_, context, query,
                             [](std::uint32_t page, std::uint32_t size) { return protocol::recommended_playlists(page, size); });
    }

private:
    Client client_;
};

template <class Port, class Impl> ports::CapabilityBinding bind() {
    return ports::CapabilityBinding::bind<Port>(
        [](const ports::ProviderSetup& setup) -> domain::Result<std::shared_ptr<Port>> {
            if (auto ready = require_upstream(setup); !ready) {
                return ready.error();
            }
            return std::shared_ptr<Port>{std::make_shared<Impl>(setup)};
        });
}

} // namespace

std::vector<ports::CapabilitySpec> collection_capabilities() {
    const auto page = ports::Pagination::cursor;
    return {
        {.key = "search_playlists", .pagination = page, .catalog_kind = ports::CatalogKind::live},
        {.key = "search_albums", .pagination = page, .catalog_kind = ports::CatalogKind::live},
        {.key = "playlist_detail"},
        {.key = "playlist_tracks", .pagination = page},
        {.key = "album_detail"},
        {.key = "album_tracks", .pagination = page},
        {.key = "chart_detail"},
        {.key = "chart_tracks", .pagination = page},
        {.key = "categories", .catalog_kind = ports::CatalogKind::live},
        {.key = "category_playlists", .pagination = page},
        {.key = "charts", .catalog_kind = ports::CatalogKind::live},
        {.key = "recommend_playlists", .pagination = page},
    };
}

std::vector<ports::CapabilityBinding> collection_bindings() {
    return {
        bind<ports::PlaylistSearch, Search<tag::SearchPlaylists>>(),
        bind<ports::AlbumSearch, Search<tag::SearchAlbums>>(),
        bind<ports::PlaylistDetail, Detail<tag::PlaylistDetail, PlaylistSource>>(),
        bind<ports::PlaylistTracks, Tracks<tag::PlaylistTracks, PlaylistSource>>(),
        bind<ports::AlbumDetail, Detail<tag::AlbumDetail, AlbumSource>>(),
        bind<ports::AlbumTracks, Tracks<tag::AlbumTracks, AlbumSource>>(),
        bind<ports::ChartDetail, Detail<tag::ChartDetail, ChartSource>>(),
        bind<ports::ChartTracks, Tracks<tag::ChartTracks, ChartSource>>(),
        bind<ports::ICategories, Directory>(),
        bind<ports::CategoryPlaylists, CategoryPlaylists>(),
        bind<ports::ICharts, Directory>(),
        bind<ports::IRecommendedPlaylists, Directory>(),
    };
}

} // namespace music_service::providers::kuwo
