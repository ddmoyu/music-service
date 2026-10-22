#pragma once

#include "application/accounts/credential_service.hpp"
#include "application/catalog/handle_table.hpp"
#include "application/catalog/result_cache.hpp"
#include "application/config/service_config.hpp"
#include "application/ports/providers/directory.hpp"
#include "application/ports/runtime/clock.hpp"
#include "application/ports/runtime/executor.hpp"
#include "domain/catalog/collections.hpp"
#include "domain/catalog/track.hpp"
#include "domain/result.hpp"

#include <any>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace music_service::application::catalog {

struct CatalogLimits {
    config::Catalog catalog;
    config::Cache cache;
    config::Handles handles;
    config::Lifetimes lifetimes;
    std::size_t per_owner_handle_share{4}; // a principal may hold 1/share of each handle table
};

struct SearchRequest {
    std::string owner_id;
    std::string q;
    // Explicit sources, "all", or nothing for catalog.default_sources.
    std::vector<std::string> sources;
    bool all_sources{false};
    std::uint32_t limit{20};
    std::optional<std::string> cursor; // exactly one source
    std::optional<std::chrono::milliseconds> timeout;
};

enum class SourceStatus { ok, failed, unsupported, disabled };

struct SourcePage {
    std::uint32_t limit{0};
    std::uint32_t returned{0};
    std::optional<std::uint64_t> total;
    std::optional<bool> has_more;
    std::optional<std::string> next_cursor; // service cursor handle
    std::optional<std::uint64_t> listed_total{};
    std::optional<std::uint64_t> unavailable_count{};
};

struct CacheInfo {
    bool hit{false};
    std::chrono::system_clock::time_point created_at;
};

template <class Item> struct BasicSourceResult {
    std::string source;
    SourceStatus status{SourceStatus::failed};
    std::chrono::milliseconds elapsed{0};
    std::optional<SourcePage> page;
    std::optional<domain::Error> error;
    std::optional<CacheInfo> cache;
    std::vector<Item> items; // moved into BasicSearchResult::items
};
using SourceResult = BasicSourceResult<domain::Track>;

template <class Item> struct BasicSearchResult {
    std::vector<Item> items; // grouped by source in request order, upstream order within
    std::vector<BasicSourceResult<Item>> sources;
    bool partial{false};
    // Set when no source succeeded: every executed source timed out (UPSTREAM_TIMEOUT),
    // every one lacked local capacity such as cursor handles (SERVICE_BUSY), other
    // failures (UPSTREAM_FAILED), or nothing selected is usable
    // (CAPABILITY_UNSUPPORTED). The per-source results stay available.
    std::optional<domain::Error> failure;
};
using SearchResult = BasicSearchResult<domain::Track>;

// One entity, or one source's directory, from the platform or the cache.
template <class Item> struct EntityResult {
    Item value;
    CacheInfo cache;
};

template <class Item> struct DirectoryResult {
    std::vector<Item> items;
    std::optional<ports::CatalogKind> catalog_kind; // as the capability declares it
    CacheInfo cache;
};

// A page of what one entity holds. id is the playlist, album, artist, chart or category;
// recommendations take none. Without limit the route's default applies.
struct ListRequest {
    std::string owner_id;
    std::string source;
    std::string id;
    std::optional<std::uint32_t> limit;
    std::optional<std::string> cursor;
};

template <class Item> struct ListResult {
    std::vector<Item> items;
    SourcePage page; // next_cursor continues this list only
    CacheInfo cache;
};

struct LinkMatch {
    bool matched{false};
    std::string kind;
    std::optional<domain::PlatformRef> ref;
};

// Search, track details and link parsing (feature-modules.md, 搜索). Every platform
// call runs under the caller's account binding for that source: the cache, in-flight
// merging and cursors are keyed by the full AuthIdentityKey, and cursors made under
// the owner's default selection also carry its selection_version, so switching
// accounts (even A -> B -> A) expires them. Failures are never cached.
class CatalogService {
public:
    static constexpr std::size_t max_query_chars = 200;
    static constexpr std::size_t max_link_text_chars = 4096;

    CatalogService(CatalogLimits limits, std::shared_ptr<const ports::ProviderDirectory> providers,
                   std::shared_ptr<accounts::CredentialService> credentials,
                   std::shared_ptr<ports::WorkExecutor> executor, std::shared_ptr<const ports::Clock> clock,
                   std::function<std::string()> new_cursor_id);

    // Validation and cursor problems are errors; per-source outcomes are in the result.
    domain::Result<SearchResult> search_tracks(const SearchRequest& request);

    struct Detail {
        domain::Track track;
        CacheInfo cache;
    };
    domain::Result<Detail> track_detail(const std::string& owner_id, const std::string& source,
                                        const std::string& id);

    // One entity from share text, or matched=false. Several distinct entities are
    // VALIDATION_FAILED. Never touches the network.
    domain::Result<LinkMatch> parse_link(const std::string& owner_id, const std::string& text);

    // Collections, catalogs and lyrics (T21), routed by capability: a source without the
    // capability is CAPABILITY_UNSUPPORTED, a disabled one PROVIDER_DISABLED. Results are
    // cached and merged per AuthIdentityKey like search; list cursors are bound to the
    // owner, list, entity, limit, account and implementation.
    domain::Result<BasicSearchResult<domain::Playlist>> search_playlists(const SearchRequest& request);
    domain::Result<BasicSearchResult<domain::Album>> search_albums(const SearchRequest& request);
    domain::Result<BasicSearchResult<domain::Artist>> search_artists(const SearchRequest& request);

    domain::Result<EntityResult<domain::Playlist>> playlist_detail(const std::string& owner_id,
                                                                   const std::string& source, const std::string& id);
    domain::Result<EntityResult<domain::Album>> album_detail(const std::string& owner_id, const std::string& source,
                                                             const std::string& id);
    domain::Result<EntityResult<domain::Chart>> chart_detail(const std::string& owner_id, const std::string& source,
                                                             const std::string& id);

    // limit 1..100, default 50.
    domain::Result<ListResult<domain::Track>> playlist_tracks(const ListRequest& request);
    domain::Result<ListResult<domain::Track>> album_tracks(const ListRequest& request);
    domain::Result<ListResult<domain::Track>> artist_tracks(const ListRequest& request);
    domain::Result<ListResult<domain::Track>> chart_tracks(const ListRequest& request);
    // limit 1..50, default 20.
    domain::Result<ListResult<domain::Album>> artist_albums(const ListRequest& request);
    domain::Result<ListResult<domain::Playlist>> category_playlists(const ListRequest& request);
    domain::Result<ListResult<domain::Playlist>> recommend_playlists(const ListRequest& request);
    domain::Result<ListResult<domain::Playlist>> user_playlists(const ListRequest& request);
    domain::Result<EntityResult<domain::Podcast>> podcast_detail(const std::string& owner_id,
        const std::string& source, const std::string& id);
    domain::Result<EntityResult<domain::Episode>> episode_detail(const std::string& owner_id,
        const std::string& source, const std::string& id);
    domain::Result<ListResult<domain::Episode>> podcast_episodes(const ListRequest& request);

    domain::Result<DirectoryResult<domain::Category>> categories(const std::string& owner_id,
                                                                 const std::string& source);
    domain::Result<DirectoryResult<domain::Chart>> charts(const std::string& owner_id, const std::string& source);

    // A track without lyrics is unavailable=true, not an error.
    domain::Result<EntityResult<domain::TrackLyrics>> lyrics(const std::string& owner_id, const std::string& source,
                                                             const std::string& id);

private:
    struct CursorState;
    // A list page's continuation, bound to everything that produced it.
    struct ListCursor {
        std::string kind; // capability key
        std::string source;
        std::string entity;
        std::uint32_t limit{0};
        std::string identity;
        std::optional<std::uint64_t> selection_version;
        std::string implementation;
        std::string provider_cursor;
    };
    struct Bound {
        std::string source;
        domain::AuthIdentityKey identity;
        std::optional<std::uint64_t> selection_version;
        std::string implementation;
        std::shared_ptr<const ports::SecretBytes> secret;
        ports::CapabilitySpec spec;
    };

    template <class Item, class Call>
    domain::Result<BasicSearchResult<Item>> search_of(const SearchRequest& request, std::string_view key, Call call);
    template <class Item, class Call>
    BasicSourceResult<Item> run_search_of(const SearchRequest& request, const std::string& source,
                                          std::string_view key, const std::optional<CursorState>& cursor,
                                          std::chrono::steady_clock::time_point deadline, std::stop_token stop,
                                          const Call& call);
    domain::Result<std::vector<std::string>> select_sources(const SearchRequest& request, std::string_view key);

    // The caller's binding for one source and capability, leased.
    domain::Result<Bound> bind(const std::string& owner_id, const std::string& source, std::string_view key);
    template <class T, class Call>
    domain::Result<EntityResult<T>> cached(const Bound& bound, std::string_view key, const std::string& parts,
                                           std::size_t (*bytes)(const T&), const Call& call);
    template <class Item, class Call>
    domain::Result<ListResult<Item>> list_of(const ListRequest& request, std::string_view key, std::uint32_t max_limit,
                                             std::uint32_t default_limit, bool needs_id, const Call& call);
    template <class Item, class Call>
    domain::Result<EntityResult<Item>> detail_of(const std::string& owner_id, const std::string& source,
                                                 const std::string& id, std::string_view key, const Call& call);
    template <class Item, class Call>
    domain::Result<DirectoryResult<Item>> directory_of(const std::string& owner_id, const std::string& source,
                                                       std::string_view key, const Call& call);

    CatalogLimits limits_;
    std::shared_ptr<const ports::ProviderDirectory> providers_;
    std::shared_ptr<accounts::CredentialService> credentials_;
    std::shared_ptr<ports::WorkExecutor> executor_;
    std::shared_ptr<const ports::Clock> clock_;

    // Searches, collections, lists and lyrics share one cache; keys start with the
    // capability key, so a value is only ever read back as the type it was stored as.
    ResultCache<std::any> entity_cache_;
    ResultCache<domain::Track> detail_cache_;
    SingleFlight<std::any> entity_flights_;
    SingleFlight<domain::Track> detail_flights_;
    std::shared_ptr<HandleTable<CursorState>> cursors_;
    std::shared_ptr<HandleTable<ListCursor>> list_cursors_;
};

} // namespace music_service::application::catalog
