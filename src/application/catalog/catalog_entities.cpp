// CatalogService: collection details, entity lists, category and chart catalogs,
// recommendations and lyrics (T21). Each call runs under the caller's account binding
// for its source and capability, is cached and merged per AuthIdentityKey and
// implementation, and never fetches more than the one page asked for.

#include "application/catalog/catalog_service.hpp"

#include "application/catalog/catalog_support.hpp"
#include "application/ports/providers/provider.hpp"

#include <any>
#include <utility>

namespace music_service::application::catalog {

namespace {

using domain::Error;
using domain::ErrorCode;
using SteadyClock = std::chrono::steady_clock;
using support::item_bytes;
using support::key_of;
using support::public_error;
using support::share_secret;

template <class Item> std::size_t page_bytes(const ports::CatalogPage<Item>& page) {
    std::size_t bytes = 128 + page.next_cursor.value_or("").size();
    for (const auto& item : page.page.items) {
        bytes += item_bytes(item);
    }
    return bytes;
}

template <class Item> std::size_t list_bytes(const std::vector<Item>& items) {
    std::size_t bytes = 128;
    for (const auto& item : items) {
        bytes += item_bytes(item);
    }
    return bytes;
}

template <class T> std::size_t value_bytes(const T& value) { return item_bytes(value); }

} // namespace

domain::Result<CatalogService::Bound> CatalogService::bind(const std::string& owner_id, const std::string& source,
                                                           std::string_view key) {
    if (auto parsed = domain::SourceId::parse(source); !parsed) {
        return std::move(parsed).error();
    }
    if (auto known = providers_->check_source(source); !known) {
        return known.error();
    }
    auto spec = providers_->describe_capability(source, key);
    if (!spec) {
        return std::move(spec).error(); // PROVIDER_DISABLED or CAPABILITY_UNSUPPORTED
    }
    auto binding = credentials_->resolve_binding(owner_id, source, key, std::nullopt);
    if (!binding) {
        return std::move(binding).error();
    }
    auto lease = credentials_->lease(binding->binding);
    if (!lease) {
        return std::move(lease).error();
    }
    auto view = providers_->describe(source);
    if (!view) {
        return std::move(view).error();
    }
    return Bound{source,
                 lease->identity,
                 binding->selection_version,
                 view->implementation_id + "@" + view->implementation_version,
                 share_secret(lease->secret),
                 std::move(spec).value()};
}

template <class T, class Call>
domain::Result<EntityResult<T>> CatalogService::cached(const Bound& bound, std::string_view key,
                                                       const std::string& parts, std::size_t (*bytes)(const T&),
                                                       const Call& call) {
    const auto cache_key = key_of({key, bound.identity.canonical(), bound.implementation, parts});
    const auto now = clock_->now();
    if (auto hit = entity_cache_.get(cache_key, now)) {
        return EntityResult<T>{std::any_cast<const T&>(*hit->value), CacheInfo{true, hit->created_at}};
    }
    const auto deadline = SteadyClock::now() + limits_.catalog.total_budget;
    auto fetched = entity_flights_.join(cache_key, deadline, [&]() -> SingleFlight<std::any>::Result {
        const ports::ProviderContext context{{deadline, {}}, bound.identity, std::nullopt, bound.secret};
        auto value = call(context);
        if (!value) {
            return std::move(value).error();
        }
        return std::make_shared<const std::any>(std::move(value).value());
    });
    if (!fetched) {
        return public_error(std::move(fetched).error()); // failures are never cached
    }
    const auto& value = std::any_cast<const T&>(*fetched.value());
    entity_cache_.put(cache_key, fetched.value(), bytes(value), now);
    return EntityResult<T>{value, CacheInfo{false, now}};
}

template <class Item, class Call>
domain::Result<EntityResult<Item>> CatalogService::detail_of(const std::string& owner_id, const std::string& source,
                                                             const std::string& id, std::string_view key,
                                                             const Call& call) {
    auto platform_id = domain::PlatformId::parse(id);
    if (!platform_id) {
        return std::move(platform_id).error();
    }
    auto bound = bind(owner_id, source, key);
    if (!bound) {
        return std::move(bound).error();
    }
    const domain::PlatformRef ref{domain::SourceId::parse(source).value(), std::move(platform_id).value()};
    return cached<Item>(*bound, key, id, &value_bytes<Item>,
                        [&](const ports::ProviderContext& context) { return call(context, ref); });
}

template <class Item, class Call>
domain::Result<ListResult<Item>> CatalogService::list_of(const ListRequest& request, std::string_view key,
                                                         std::uint32_t max_limit, std::uint32_t default_limit,
                                                         bool needs_id, const Call& call) {
    const auto limit = request.limit.value_or(default_limit);
    if (limit < 1 || limit > max_limit) {
        return Error::validation("limit", "must be between 1 and " + std::to_string(max_limit));
    }
    std::optional<domain::PlatformId> platform_id;
    if (needs_id) {
        auto parsed = domain::PlatformId::parse(request.id);
        if (!parsed) {
            return std::move(parsed).error();
        }
        platform_id = std::move(parsed).value();
    }
    const auto now = clock_->now();
    std::optional<ListCursor> cursor;
    if (request.cursor) {
        auto found = list_cursors_->get(request.owner_id, *request.cursor, now, ErrorCode::cursor_expired);
        if (!found) {
            return Error{ErrorCode::cursor_expired, "the cursor has expired", false, {}};
        }
        cursor = std::move(found->first);
        if (cursor->kind != key || cursor->source != request.source || cursor->entity != request.id ||
            cursor->limit != limit) {
            return Error{ErrorCode::cursor_mismatch, "the cursor belongs to another list", false, {}};
        }
    }
    auto bound = bind(request.owner_id, request.source, key);
    if (!bound) {
        return std::move(bound).error();
    }
    const auto identity = bound->identity.canonical();
    if (cursor && (cursor->identity != identity || cursor->selection_version != bound->selection_version ||
                   cursor->implementation != bound->implementation)) {
        return Error{ErrorCode::cursor_expired, "the account or implementation behind the cursor changed", false, {}};
    }
    const auto provider_cursor = cursor ? std::optional<std::string>{cursor->provider_cursor} : std::nullopt;
    const auto source_id = domain::SourceId::parse(request.source).value();
    // Lists without an entity (recommendations) use the source as their subject.
    const domain::PlatformRef ref{source_id, platform_id ? *platform_id : domain::PlatformId::parse(request.source).value()};
    const ports::ListQuery query{limit, provider_cursor};
    auto page = cached<ports::CatalogPage<Item>>(
        *bound, key, key_of({request.id, std::to_string(limit), provider_cursor.value_or("")}), &page_bytes<Item>,
        [&](const ports::ProviderContext& context) { return call(context, ref, query); });
    if (!page) {
        return std::move(page).error();
    }
    const auto& found = page->value;
    ListResult<Item> result;
    result.items = found.page.items;
    result.cache = page->cache;
    result.page = SourcePage{limit, found.page.page.returned, found.page.page.total, found.page.page.has_more,
                             std::nullopt};
    if (found.next_cursor) {
        auto handle = list_cursors_->put(request.owner_id,
                                         ListCursor{std::string{key}, request.source, request.id, limit, identity,
                                                    bound->selection_version, bound->implementation,
                                                    *found.next_cursor},
                                         now + limits_.lifetimes.cursor, now);
        if (!handle) {
            return std::move(handle).error(); // more exists but cannot be continued now
        }
        result.page.next_cursor = std::move(handle).value();
    }
    return result;
}

template <class Item, class Call>
domain::Result<DirectoryResult<Item>> CatalogService::directory_of(const std::string& owner_id,
                                                                   const std::string& source, std::string_view key,
                                                                   const Call& call) {
    auto bound = bind(owner_id, source, key);
    if (!bound) {
        return std::move(bound).error();
    }
    auto found = cached<std::vector<Item>>(*bound, key, {}, &list_bytes<Item>, call);
    if (!found) {
        return std::move(found).error();
    }
    return DirectoryResult<Item>{std::move(found->value), bound->spec.catalog_kind, found->cache};
}

namespace {

// The provider call of a capability port, as a function of its context and arguments.
template <class Port, class Method> auto through(const ports::ProviderDirectory& providers, std::string source,
                                                  Method method) {
    return [&providers, source = std::move(source), method](const ports::ProviderContext& context,
                                                             const auto&... args) {
        using Value = std::decay_t<decltype((std::declval<Port&>().*method)(context, args...).value())>;
        auto port = providers.capability<Port>(source);
        if (!port) {
            return domain::Result<Value>{std::move(port).error()};
        }
        return ((*port.value()).*method)(context, args...);
    };
}

} // namespace

domain::Result<EntityResult<domain::Playlist>>
CatalogService::playlist_detail(const std::string& owner_id, const std::string& source, const std::string& id) {
    return detail_of<domain::Playlist>(owner_id, source, id, ports::capability::playlist_detail,
                                       through<ports::PlaylistDetail>(*providers_, source, &ports::PlaylistDetail::detail));
}

domain::Result<EntityResult<domain::Album>>
CatalogService::album_detail(const std::string& owner_id, const std::string& source, const std::string& id) {
    return detail_of<domain::Album>(owner_id, source, id, ports::capability::album_detail,
                                    through<ports::AlbumDetail>(*providers_, source, &ports::AlbumDetail::detail));
}

domain::Result<EntityResult<domain::Chart>>
CatalogService::chart_detail(const std::string& owner_id, const std::string& source, const std::string& id) {
    return detail_of<domain::Chart>(owner_id, source, id, ports::capability::chart_detail,
                                    through<ports::ChartDetail>(*providers_, source, &ports::ChartDetail::detail));
}

domain::Result<ListResult<domain::Track>> CatalogService::playlist_tracks(const ListRequest& request) {
    return list_of<domain::Track>(request, ports::capability::playlist_tracks, 100, 50, true,
                                  through<ports::PlaylistTracks>(*providers_, request.source, &ports::PlaylistTracks::list));
}

domain::Result<ListResult<domain::Track>> CatalogService::album_tracks(const ListRequest& request) {
    return list_of<domain::Track>(request, ports::capability::album_tracks, 100, 50, true,
                                  through<ports::AlbumTracks>(*providers_, request.source, &ports::AlbumTracks::list));
}

domain::Result<ListResult<domain::Track>> CatalogService::artist_tracks(const ListRequest& request) {
    return list_of<domain::Track>(request, ports::capability::artist_tracks, 100, 50, true,
                                  through<ports::ArtistTracks>(*providers_, request.source, &ports::ArtistTracks::list));
}

domain::Result<ListResult<domain::Track>> CatalogService::chart_tracks(const ListRequest& request) {
    return list_of<domain::Track>(request, ports::capability::chart_tracks, 100, 50, true,
                                  through<ports::ChartTracks>(*providers_, request.source, &ports::ChartTracks::list));
}

domain::Result<ListResult<domain::Album>> CatalogService::artist_albums(const ListRequest& request) {
    return list_of<domain::Album>(request, ports::capability::artist_albums, 50, 20, true,
                                  through<ports::ArtistAlbums>(*providers_, request.source, &ports::ArtistAlbums::list));
}

domain::Result<ListResult<domain::Playlist>> CatalogService::category_playlists(const ListRequest& request) {
    return list_of<domain::Playlist>(
        request, ports::capability::category_playlists, 50, 20, true,
        through<ports::CategoryPlaylists>(*providers_, request.source, &ports::CategoryPlaylists::list));
}

domain::Result<ListResult<domain::Playlist>> CatalogService::recommend_playlists(const ListRequest& request) {
    auto unscoped = request;
    unscoped.id.clear(); // recommendations belong to the source, not to an entity
    return list_of<domain::Playlist>(
        unscoped, ports::capability::recommend_playlists, 50, 20, false,
        [call = through<ports::IRecommendedPlaylists>(*providers_, request.source,
                                                       &ports::IRecommendedPlaylists::recommend_playlists)](
            const ports::ProviderContext& context, const domain::PlatformRef&, const ports::ListQuery& query) {
            return call(context, query);
        });
}

domain::Result<DirectoryResult<domain::Category>> CatalogService::categories(const std::string& owner_id,
                                                                             const std::string& source) {
    return directory_of<domain::Category>(owner_id, source, ports::capability::categories,
                                          through<ports::ICategories>(*providers_, source, &ports::ICategories::categories));
}

domain::Result<DirectoryResult<domain::Chart>> CatalogService::charts(const std::string& owner_id,
                                                                      const std::string& source) {
    return directory_of<domain::Chart>(owner_id, source, ports::capability::charts,
                                       through<ports::ICharts>(*providers_, source, &ports::ICharts::charts));
}

domain::Result<EntityResult<domain::TrackLyrics>>
CatalogService::lyrics(const std::string& owner_id, const std::string& source, const std::string& id) {
    return detail_of<domain::TrackLyrics>(owner_id, source, id, ports::capability::lyrics,
                                          through<ports::ILyrics>(*providers_, source, &ports::ILyrics::lyrics));
}

} // namespace music_service::application::catalog
