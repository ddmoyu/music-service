#include "providers/kuwo/artists.hpp"
#include "providers/kuwo/client.hpp"
#include "providers/kuwo/parsing/artists.hpp"
#include "providers/kuwo/parsing/parsers.hpp"
#include "providers/kuwo/protocol/requests.hpp"
#include <type_traits>

namespace music_service::providers::kuwo {
namespace {
using domain::Error;
namespace tag = ports::catalog_tag;
domain::Result<void> valid_ref(const domain::PlatformRef& ref) {
    if (ref.source.value() != "kuwo" || !parsing::is_valid_rid(ref.id.value()))
        return Error::validation("id", "must be a kuwo artist id");
    return domain::ok();
}
class Search final : public ports::ArtistSearch {
public:
    explicit Search(const ports::ProviderSetup& setup) : client_(setup) {}
    domain::Result<ports::CatalogPage<domain::Artist>> search(
        const ports::ProviderContext& context, const ports::SearchQuery& query) override {
        if (parsing::decode_text(query.text).empty() || query.text.size() > 200 || !domain::is_valid_utf8(query.text))
            return Error::validation("q", "must contain 1 to 200 UTF-8 bytes");
        if (!query.limit || query.limit > 50) return Error::validation("limit", "must be between 1 and 50");
        auto page = page_cursor(query.cursor);
        if (!page) return page.error();
        auto body = client_.body(protocol::artist_search(query.text, *page, query.limit), context);
        if (!body) return body.error();
        return parsing::parse_artist_search(*body, *page, query.limit);
    }
private: Client client_;
};
class Detail final : public ports::ArtistDetail {
public:
    explicit Detail(const ports::ProviderSetup& setup) : client_(setup) {}
    domain::Result<domain::Artist> detail(const ports::ProviderContext& context, const domain::PlatformRef& ref) override {
        if (auto valid = valid_ref(ref); !valid) return valid.error();
        auto body = client_.body(protocol::artist_detail(ref.id.value()), context);
        if (!body) return body.error();
        return parsing::parse_artist_detail(*body, ref.id.value());
    }
private: Client client_;
};
template<class Tag> class List final : public ports::IEntityList<Tag> {
public:
    explicit List(const ports::ProviderSetup& setup) : client_(setup) {}
    domain::Result<ports::CatalogPage<typename Tag::Item>> list(const ports::ProviderContext& context,
        const domain::PlatformRef& ref, const ports::ListQuery& query) override {
        if (auto valid = valid_ref(ref); !valid) return valid.error();
        constexpr bool tracks = std::is_same_v<Tag, tag::ArtistTracks>;
        if (!query.limit || query.limit > (tracks ? 100U : 50U))
            return Error::validation("limit", tracks ? "must be between 1 and 100" : "must be between 1 and 50");
        auto page = page_cursor(query.cursor);
        if (!page) return page.error();
        auto request = tracks ? protocol::artist_tracks(ref.id.value(), *page, query.limit) :
                                protocol::artist_albums(ref.id.value(), *page, query.limit);
        auto body = client_.body(std::move(request), context);
        if (!body) return body.error();
        if constexpr (tracks) return parsing::parse_artist_tracks(*body, *page, query.limit);
        else return parsing::parse_artist_albums(*body, *page, query.limit);
    }
private: Client client_;
};
template<class Interface, class Implementation> ports::CapabilityBinding bind() {
    return ports::CapabilityBinding::bind<Interface>([](const ports::ProviderSetup& setup)
        -> domain::Result<std::shared_ptr<Interface>> {
        if (auto ready = require_upstream(setup); !ready) return ready.error();
        return std::shared_ptr<Interface>{std::make_shared<Implementation>(setup)};
    });
}
}
std::vector<ports::CapabilitySpec> artist_capabilities() {
    return {{.key="search_artists", .pagination=ports::Pagination::cursor, .catalog_kind=ports::CatalogKind::live},
        {.key="artist_detail"}, {.key="artist_tracks", .pagination=ports::Pagination::cursor},
        {.key="artist_albums", .pagination=ports::Pagination::cursor}};
}
std::vector<ports::CapabilityBinding> artist_bindings() {
    return {bind<ports::ArtistSearch, Search>(), bind<ports::ArtistDetail, Detail>(),
        bind<ports::ArtistTracks, List<tag::ArtistTracks>>(), bind<ports::ArtistAlbums, List<tag::ArtistAlbums>>()};
}
} // namespace music_service::providers::kuwo
