#include "providers/kugou/artists.hpp"
#include "application/transport/url.hpp"
#include "providers/kugou/paging.hpp"
#include "providers/kugou/parsing.hpp"
#include <array>
#include <chrono>
#include <map>
#include <set>
#include <type_traits>

namespace music_service::providers::kugou {
std::string artist_search_url(std::string_view query, std::uint32_t page_number,
                              std::uint32_t limit, std::uint64_t timestamp_ms) {
    const auto now = std::to_string(timestamp_ms);
    std::map<std::string, std::string> params{{"appid", "1014"},
                                              {"srcappid", "2919"},
                                              {"clientver", "20000"},
                                              {"clienttime", now},
                                              {"mid", now},
                                              {"uuid", now},
                                              {"dfid", "-"},
                                              {"keyword", std::string{query}},
                                              {"page", std::to_string(page_number)},
                                              {"pagesize", std::to_string(limit)}};
    return signed_web_url("https://complexsearch.kugou.com/v1/search/author", std::move(params));
}
namespace {
using parsing::number;
using parsing::text;
Json artist_info(const Client& client, const ports::ProviderContext& context,
                 const domain::PlatformRef& ref, std::string_view operation) {
    check_ref(ref);
    auto doc = client.get(
        operation, std::string{mobile_api} + "singer/info?singerid=" + ref.id.value(), context);
    // 合法但不存在的 singerid 返回 status=0 / errcode=0，且没有 data。
    if (number(doc, "status") == 0 && number(doc, "errcode") == 0 &&
        (!doc.contains("data") || doc.at("data").is_null() || doc.at("data").empty()))
        throw Error{ErrorCode::not_found, "Kugou artist was not found", false, {}};
    api_success(doc);
    auto data = doc.at("data");
    if (text(data, "singerid", true) != ref.id.value())
        throw std::invalid_argument{"artist identity"};
    return data;
}
class Search final : public ports::ArtistSearch {
  public:
    explicit Search(const ports::ProviderSetup& setup) : client_(setup) {}
    domain::Result<ports::CatalogPage<domain::Artist>>
    search(const ports::ProviderContext& context, const ports::SearchQuery& query) override {
        return checked([&] {
            const auto start = offset(query.cursor, query.limit, false);
            if (query.text.empty() || query.text.size() > 200 || !domain::is_valid_utf8(query.text))
                throw Error::validation("q", "must contain 1 to 200 UTF-8 bytes");
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
            auto doc = client_.get("search_artists",
                                   artist_search_url(query.text, start / query.limit + 1,
                                                     query.limit, static_cast<std::uint64_t>(now)),
                                   context);
            api_success(doc);
            const auto& data = doc.at("data");
            if (number(data, "page") != start / query.limit + 1 ||
                number(data, "pagesize") != query.limit || number(data, "from") != start)
                throw std::invalid_argument{"artist search page identity"};
            const auto& list = data.at("lists");
            parsing::array(list, query.limit);
            std::vector<domain::Artist> values;
            std::set<std::string> ids;
            for (const auto& entry : list) {
                auto artist = parsing::artist(entry, true);
                if (!ids.insert(artist.ref.id.value()).second)
                    throw std::invalid_argument{"duplicate artist identity"};
                values.push_back(std::move(artist));
            }
            return page(std::move(values), start, query.limit, number(data, "total"));
        });
    }

  private:
    Client client_;
};
class Detail final : public ports::ArtistDetail {
  public:
    explicit Detail(const ports::ProviderSetup& setup) : client_(setup) {}
    domain::Result<domain::Artist> detail(const ports::ProviderContext& context,
                                          const domain::PlatformRef& ref) override {
        return checked(
            [&] { return parsing::artist(artist_info(client_, context, ref, "artist_detail")); });
    }

  private:
    Client client_;
};
template <class Tag> class List final : public ports::IEntityList<Tag> {
  public:
    explicit List(const ports::ProviderSetup& setup) : client_(setup) {}
    domain::Result<ports::CatalogPage<typename Tag::Item>>
    list(const ports::ProviderContext& context, const domain::PlatformRef& ref,
         const ports::ListQuery& query) override {
        return checked([&] {
            constexpr bool tracks = std::is_same_v<Tag, ports::catalog_tag::ArtistTracks>;
            const auto start = offset(query.cursor, query.limit, true);
            auto artist = parsing::artist(artist_info(client_, context, ref, Tag::key));
            auto data = client_.api(Tag::key,
                                    std::string{"singer/"} + (tracks ? "song" : "album") +
                                        "?singerid=" + ref.id.value() + "&sorttype=2" +
                                        window(start, query.limit),
                                    context);
            const auto total = number(data, "total");
            // timestamp 是接口当前时间，不能作为目录修订号；以身份、类型、排序和总数绑定游标。
            const auto revision =
                digest(Json{std::string{Tag::key}, ref.id.value(), total, 2}.dump());
            same_revision(query.cursor, revision);
            const auto& entries = data.at("info");
            parsing::array(entries, query.limit);
            std::vector<typename Tag::Item> values;
            std::uint64_t unavailable = 0;
            for (const auto& entry : entries) {
                if constexpr (tracks) {
                    const auto id = parsing::first(entry, {"FileHash", "hash"});
                    if (id.empty() || id == "00000000000000000000000000000000") {
                        ++unavailable;
                        continue;
                    }
                    auto song = parsing::track(entry);
                    // 仅精确同名的单一歌手绑定父目录身份，不改写合作演唱者。
                    if (song.artists.size() == 1 && song.artists.front().name == artist.name)
                        song.artists.front().ref = artist.ref;
                    values.push_back(std::move(song));
                } else {
                    auto album = parsing::album(entry);
                    if (album.artists.size() == 1 && album.artists.front().name == artist.name)
                        album.artists.front().ref = artist.ref;
                    values.push_back(std::move(album));
                }
            }
            auto result = page(std::move(values), start, query.limit, total, revision);
            if (entries.size() == std::min<std::uint64_t>(query.limit, total - start))
                result.page.page.unavailable_count = unavailable;
            return result;
        });
    }

  private:
    Client client_;
};
} // namespace
void add_artists(ports::ProviderBundle& bundle) {
    bind<ports::ArtistSearch, Search>(bundle);
    bind<ports::ArtistDetail, Detail>(bundle);
    bind<ports::ArtistTracks, List<ports::catalog_tag::ArtistTracks>>(bundle);
    bind<ports::ArtistAlbums, List<ports::catalog_tag::ArtistAlbums>>(bundle);
}
} // namespace music_service::providers::kugou
