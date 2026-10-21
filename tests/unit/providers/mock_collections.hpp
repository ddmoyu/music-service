#pragma once

// A simulated catalog source with every T21 capability: playlist, album and artist
// search, collection details, paged entity lists, categories, charts, recommendations
// and lyrics, but no audio. Answers from memory; a shared script counts calls, delays,
// fails and decides how many pages each list has.

#include "mock_providers.hpp"

#include "application/ports/providers/provider.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <type_traits>

namespace music_service::test {

struct CollectionScript {
    std::atomic<int> calls{0};
    std::atomic<int> delay_ms{0};
    std::atomic<bool> fail{false};
    int pages{3};                                    // every search and list has this many pages
    std::set<std::string> missing{"gone"};           // ids answered with NOT_FOUND
    std::set<std::string> without_lyrics{"silent"};  // tracks without lyrics
};

namespace collections_detail {

inline domain::PlatformRef ref(const domain::SourceId& source, const std::string& id) {
    return {source, unwrap(domain::PlatformId::parse(id))};
}

// One item of the requested type; name says where it came from.
template <class Item> Item item(const domain::SourceId& source, const std::string& id, const std::string& name) {
    if constexpr (std::is_same_v<Item, domain::Track>) {
        return domain::Track{ref(source, id), name, {{"singer", std::nullopt}}, std::nullopt, 180000, {}};
    } else if constexpr (std::is_same_v<Item, domain::Playlist>) {
        domain::Playlist p{ref(source, id), name};
        p.creator = "curator";
        p.track_count = 120;
        return p;
    } else if constexpr (std::is_same_v<Item, domain::Album>) {
        domain::Album a{ref(source, id), name};
        a.artists = {{"singer", ref(source, "ar1")}};
        a.release_date = "2003-07";
        a.track_count = 11;
        return a;
    } else if constexpr (std::is_same_v<Item, domain::Artist>) {
        return domain::Artist{ref(source, id), name, std::nullopt};
    } else {
        static_assert(std::is_same_v<Item, domain::Chart>);
        domain::Chart c{ref(source, id), name};
        c.update_hint = "daily";
        return c;
    }
}

// Waits for the script's delay (stopping early when asked), then fails if told to.
inline std::optional<domain::Error> behave(CollectionScript& script, const ports::ProviderContext& context) {
    ++script.calls;
    if (const auto delay = script.delay_ms.load(); delay > 0) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds{delay};
        // As a real transport does: the call ends at its deadline or when stopped.
        while (std::chrono::steady_clock::now() < until && !context.operation.stop.stop_requested() &&
               !context.operation.expired(std::chrono::steady_clock::now())) {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
    }
    if (script.fail) {
        return domain::Error{domain::ErrorCode::upstream_failed, "broken", true, {}};
    }
    return std::nullopt;
}

// Page n of a list of `pages` pages, min(limit, 2) items each, named "<label>#<n>".
template <class Item>
ports::CatalogPage<Item> page_of(const CollectionScript& script, const domain::SourceId& source,
                                 const std::string& label, std::uint32_t limit, const std::optional<std::string>& cursor) {
    const int n = cursor ? std::stoi(*cursor) : 0;
    ports::CatalogPage<Item> page;
    const auto count = std::min<std::uint32_t>(limit, 2);
    for (std::uint32_t i = 0; i < count; ++i) {
        page.page.items.push_back(
            item<Item>(source, "i" + std::to_string(n) + "-" + std::to_string(i), label + "#" + std::to_string(n)));
    }
    const bool more = n + 1 < script.pages;
    page.page.page = {limit, count, static_cast<std::uint64_t>(script.pages) * count, more};
    if (more) {
        page.next_cursor = std::to_string(n + 1);
    }
    return page;
}

template <class Tag> class Search final : public ports::ICatalogSearch<Tag> {
public:
    Search(domain::SourceId source, std::shared_ptr<CollectionScript> script)
        : source_(std::move(source)), script_(std::move(script)) {}
    domain::Result<ports::CatalogPage<typename Tag::Item>> search(const ports::ProviderContext& context,
                                                                  const ports::SearchQuery& query) override {
        if (auto failed = behave(*script_, context)) {
            return *failed;
        }
        return page_of<typename Tag::Item>(*script_, source_, query.text, query.limit, query.cursor);
    }

private:
    domain::SourceId source_;
    std::shared_ptr<CollectionScript> script_;
};

template <class Tag> class Detail final : public ports::IEntityDetail<Tag> {
public:
    explicit Detail(std::shared_ptr<CollectionScript> script) : script_(std::move(script)) {}
    domain::Result<typename Tag::Item> detail(const ports::ProviderContext& context,
                                              const domain::PlatformRef& entity) override {
        if (auto failed = behave(*script_, context)) {
            return *failed;
        }
        if (script_->missing.contains(entity.id.value())) {
            return domain::Error{domain::ErrorCode::not_found, "no such entity", false, {}};
        }
        return item<typename Tag::Item>(entity.source, entity.id.value(), "detail:" + entity.id.value());
    }

private:
    std::shared_ptr<CollectionScript> script_;
};

template <class Tag> class List final : public ports::IEntityList<Tag> {
public:
    explicit List(std::shared_ptr<CollectionScript> script) : script_(std::move(script)) {}
    domain::Result<ports::CatalogPage<typename Tag::Item>> list(const ports::ProviderContext& context,
                                                                const domain::PlatformRef& entity,
                                                                const ports::ListQuery& query) override {
        if (auto failed = behave(*script_, context)) {
            return *failed;
        }
        if (script_->missing.contains(entity.id.value())) {
            return domain::Error{domain::ErrorCode::not_found, "no such entity", false, {}};
        }
        return page_of<typename Tag::Item>(*script_, entity.source, entity.id.value(), query.limit, query.cursor);
    }

private:
    std::shared_ptr<CollectionScript> script_;
};

class Directory final : public ports::ICategories, public ports::ICharts, public ports::IRecommendedPlaylists,
                        public ports::ILyrics {
public:
    Directory(domain::SourceId source, std::shared_ptr<CollectionScript> script)
        : source_(std::move(source)), script_(std::move(script)) {}
    domain::Result<std::vector<domain::Category>> categories(const ports::ProviderContext& context) override {
        if (auto failed = behave(*script_, context)) {
            return *failed;
        }
        return std::vector<domain::Category>{{"pop", "Pop", "style", domain::ListedKind::playlist},
                                             {"night", "Night", std::nullopt, domain::ListedKind::playlist}};
    }
    domain::Result<std::vector<domain::Chart>> charts(const ports::ProviderContext& context) override {
        if (auto failed = behave(*script_, context)) {
            return *failed;
        }
        return std::vector<domain::Chart>{item<domain::Chart>(source_, "top", "Top 100")};
    }
    domain::Result<ports::CatalogPage<domain::Playlist>> recommend_playlists(const ports::ProviderContext& context,
                                                                            const ports::ListQuery& query) override {
        if (auto failed = behave(*script_, context)) {
            return *failed;
        }
        return page_of<domain::Playlist>(*script_, source_, "recommended", query.limit, query.cursor);
    }
    domain::Result<domain::TrackLyrics> lyrics(const ports::ProviderContext& context,
                                               const domain::TrackRef& track) override {
        if (auto failed = behave(*script_, context)) {
            return *failed;
        }
        domain::TrackLyrics lyrics;
        if (script_->without_lyrics.contains(track.id.value())) {
            lyrics.unavailable = true;
            return lyrics;
        }
        lyrics.lrc = "[00:01.00]first line\n[00:05.50]second line";
        lyrics.karaoke = {{1000, 4500, {{1000, 2000, "first"}, {3000, 2500, "line"}}}};
        return lyrics;
    }

private:
    domain::SourceId source_;
    std::shared_ptr<CollectionScript> script_;
};

template <class Port, class Make> ports::CapabilityBinding bind_port(Make make) {
    return ports::CapabilityBinding::bind<Port>(
        [make](const ports::ProviderSetup& setup) -> domain::Result<std::shared_ptr<Port>> {
            return std::shared_ptr<Port>{make(setup)};
        });
}

} // namespace collections_detail

// Every T21 capability, anonymous; categories are built in, charts live.
inline ports::ProviderBundle mock_collections(std::string_view name, std::shared_ptr<CollectionScript> script) {
    namespace d = collections_detail;
    namespace tag = ports::catalog_tag;
    auto bundle = base_bundle(name);
    const auto cursor = ports::Pagination::cursor;
    bundle.capabilities = {
        {.key = "search_playlists", .pagination = cursor, .catalog_kind = ports::CatalogKind::live},
        {.key = "search_albums", .pagination = cursor},
        {.key = "search_artists", .pagination = cursor},
        {.key = "playlist_detail"},
        {.key = "album_detail"},
        {.key = "chart_detail"},
        {.key = "playlist_tracks", .pagination = cursor},
        {.key = "album_tracks", .pagination = cursor},
        {.key = "artist_tracks", .pagination = cursor},
        {.key = "chart_tracks", .pagination = cursor},
        {.key = "artist_albums", .pagination = cursor},
        {.key = "category_playlists", .pagination = cursor},
        {.key = "categories", .catalog_kind = ports::CatalogKind::built_in},
        {.key = "charts", .catalog_kind = ports::CatalogKind::live},
        {.key = "recommend_playlists", .pagination = cursor},
        {.key = "lyrics"},
    };
    auto directory = std::make_shared<std::shared_ptr<d::Directory>>();
    const auto shared_directory = [directory, script](const ports::ProviderSetup& setup) {
        if (!*directory) {
            *directory = std::make_shared<d::Directory>(setup.source, script);
        }
        return *directory;
    };
    bundle.bindings = {
        d::bind_port<ports::PlaylistSearch>([script](const ports::ProviderSetup& s) {
            return std::make_shared<d::Search<tag::SearchPlaylists>>(s.source, script);
        }),
        d::bind_port<ports::AlbumSearch>([script](const ports::ProviderSetup& s) {
            return std::make_shared<d::Search<tag::SearchAlbums>>(s.source, script);
        }),
        d::bind_port<ports::ArtistSearch>([script](const ports::ProviderSetup& s) {
            return std::make_shared<d::Search<tag::SearchArtists>>(s.source, script);
        }),
        d::bind_port<ports::PlaylistDetail>(
            [script](const ports::ProviderSetup&) { return std::make_shared<d::Detail<tag::PlaylistDetail>>(script); }),
        d::bind_port<ports::AlbumDetail>(
            [script](const ports::ProviderSetup&) { return std::make_shared<d::Detail<tag::AlbumDetail>>(script); }),
        d::bind_port<ports::ChartDetail>(
            [script](const ports::ProviderSetup&) { return std::make_shared<d::Detail<tag::ChartDetail>>(script); }),
        d::bind_port<ports::PlaylistTracks>(
            [script](const ports::ProviderSetup&) { return std::make_shared<d::List<tag::PlaylistTracks>>(script); }),
        d::bind_port<ports::AlbumTracks>(
            [script](const ports::ProviderSetup&) { return std::make_shared<d::List<tag::AlbumTracks>>(script); }),
        d::bind_port<ports::ArtistTracks>(
            [script](const ports::ProviderSetup&) { return std::make_shared<d::List<tag::ArtistTracks>>(script); }),
        d::bind_port<ports::ChartTracks>(
            [script](const ports::ProviderSetup&) { return std::make_shared<d::List<tag::ChartTracks>>(script); }),
        d::bind_port<ports::ArtistAlbums>(
            [script](const ports::ProviderSetup&) { return std::make_shared<d::List<tag::ArtistAlbums>>(script); }),
        d::bind_port<ports::CategoryPlaylists>([script](const ports::ProviderSetup&) {
            return std::make_shared<d::List<tag::CategoryPlaylists>>(script);
        }),
        d::bind_port<ports::ICategories>(shared_directory),
        d::bind_port<ports::ICharts>(shared_directory),
        d::bind_port<ports::IRecommendedPlaylists>(shared_directory),
        d::bind_port<ports::ILyrics>(shared_directory),
    };
    return bundle;
}

} // namespace music_service::test
