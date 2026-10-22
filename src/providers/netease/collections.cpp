#include "providers/netease/capabilities.hpp"
#include "providers/netease/client.hpp"
#include "providers/netease/parsing/parsers.hpp"

#include <algorithm>
#include <charconv>
#include <map>
#include <type_traits>

namespace music_service::providers::netease {
namespace {
namespace tag = ports::catalog_tag;

domain::Result<std::uint32_t> offset_of(std::string_view text) {
    std::uint32_t n{};
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), n);
    if (text.empty() || ec != std::errc{} || end != text.data() + text.size() || n > 10000)
        return Error{ErrorCode::cursor_mismatch, "invalid netease offset", false, {}};
    return n;
}

domain::Result<std::uint32_t> checked_cursor(const ports::ListQuery& query, const std::string& revision) {
    if (query.limit < 1 || query.limit > 100) return Error::validation("limit", "must be between 1 and 100");
    if (!query.cursor) return 0U;
    const auto colon = query.cursor->find(':');
    if (colon == std::string::npos) return Error{ErrorCode::cursor_mismatch, "invalid collection cursor", false, {}};
    auto offset = offset_of(std::string_view{*query.cursor}.substr(0, colon));
    if (!offset) return offset.error();
    if (query.cursor->substr(colon + 1) != revision)
        return Error{ErrorCode::cursor_expired, "collection changed; restart pagination", false, {}};
    return offset;
}

domain::Result<void> check_entity(const domain::PlatformRef& ref) {
    if (ref.source.value() != protocol::source || !parsing::is_valid_song_id(ref.id.value()))
        return Error::validation("id", "is not a netease collection id");
    return domain::ok();
}

template<class Tag> class Search final : public ports::ICatalogSearch<Tag> {
public:
    explicit Search(const ports::ProviderSetup& s) : client_(s) {}
    domain::Result<ports::CatalogPage<typename Tag::Item>> search(const ports::ProviderContext& context,
                                                                 const ports::SearchQuery& q) override {
        if (q.text.empty() || q.text.size() > 200) return Error::validation("q", "must be 1 to 200 bytes");
        if (q.limit < 1 || q.limit > 50) return Error::validation("limit", "must be between 1 and 50");
        auto offset = offset_of(q.cursor.value_or("0"));
        if (!offset) return offset.error();
        constexpr bool playlist = std::is_same_v<typename Tag::Item, domain::Playlist>;
        auto body = client_.body(protocol::search_collections(playlist, q.text, *offset, q.limit, call_for()), context);
        if (!body) return body.error();
        auto found = [&] {
            if constexpr (playlist) return parsing::parse_playlist_search(*body);
            else return parsing::parse_album_search(*body);
        }();
        if (!found) return found.error();
        if (found->items.size() > q.limit) return Error{ErrorCode::upstream_failed, "search exceeded requested page", false, {}};
        ports::CatalogPage<typename Tag::Item> page;
        const auto count = static_cast<std::uint32_t>(found->items.size());
        const auto next = *offset + count;
        const bool more = count > 0 && next < found->total && next <= 10000;
        page.page.items = std::move(found->items);
        page.page.page = {q.limit, count, found->total, more};
        if (more) page.next_cursor = std::to_string(next);
        return page;
    }
private:
    Client client_;
};

template<bool Playlist> struct Collection {
    static auto request(std::string_view id) {
        if constexpr (Playlist) return protocol::playlist(id, call_for());
        else return protocol::album(id, call_for());
    }
    static auto parse(std::string_view body, std::string_view id) {
        if constexpr (Playlist) return parsing::parse_playlist(body, id);
        else return parsing::parse_album(body, id);
    }
};

template<class Tag, bool Playlist> class Detail final : public ports::IEntityDetail<Tag> {
public:
    explicit Detail(const ports::ProviderSetup& s) : client_(s) {}
    domain::Result<typename Tag::Item> detail(const ports::ProviderContext& c, const domain::PlatformRef& ref) override {
        if (auto valid = check_entity(ref); !valid) return valid.error();
        auto body = client_.body(Collection<Playlist>::request(ref.id.value()), c);
        if (!body) return body.error();
        auto data = Collection<Playlist>::parse(*body, ref.id.value());
        if (!data) return data.error();
        return std::move(data->detail);
    }
private:
    Client client_;
};

template<class Tag, bool Playlist> class Tracks final : public ports::IEntityList<Tag> {
public:
    explicit Tracks(const ports::ProviderSetup& s) : client_(s) {}
    domain::Result<ports::CatalogPage<domain::Track>> list(const ports::ProviderContext& c,
        const domain::PlatformRef& ref, const ports::ListQuery& q) override {
        if (auto valid = check_entity(ref); !valid) return valid.error();
        if (q.limit < 1 || q.limit > 100) return Error::validation("limit", "must be between 1 and 100");
        if constexpr (std::is_same_v<Tag, tag::ChartTracks>) {
            auto directory_request = protocol::charts(call_for());
            directory_request.operation = std::string{Tag::key};
            auto directory = client_.body(std::move(directory_request), c);
            if (!directory) return directory.error();
            auto charts = parsing::parse_charts(*directory);
            if (!charts) return charts.error();
            if (std::none_of(charts->begin(), charts->end(), [&](const auto& v) { return v.ref == ref; }))
                return Error{ErrorCode::not_found, "netease chart not found", false, {}};
        }
        auto request = Collection<Playlist>::request(ref.id.value());
        request.operation = std::string{Tag::key};
        auto body = client_.body(std::move(request), c);
        if (!body) return body.error();
        auto data = Collection<Playlist>::parse(*body, ref.id.value());
        if (!data) return data.error();
        auto offset = checked_cursor(q, data->revision);
        if (!offset) return offset.error();
        const auto size = [&] {
            if constexpr (Playlist) return data->ids.size();
            else return data->tracks.size();
        }();
        if (*offset > size) return Error{ErrorCode::cursor_mismatch, "offset exceeds collection", false, {}};
        const auto end = std::min<std::size_t>(*offset + q.limit, size);
        ports::CatalogPage<domain::Track> page;
        if constexpr (Playlist) {
            // 最多当前页 100 个位置；每次补详情至多 50 个 ID。缺失项不导致下一页偏移回退。
            for (std::size_t at = *offset; at < end; at += 50) {
                const auto until = std::min(at + 50, end);
                const std::vector<std::string> ids(data->ids.begin() + static_cast<std::ptrdiff_t>(at),
                                                  data->ids.begin() + static_cast<std::ptrdiff_t>(until));
                auto details_request = protocol::song_details(ids, call_for());
                details_request.operation = std::string{Tag::key};
                auto response = client_.body(std::move(details_request), c);
                if (!response) return response.error();
                auto tracks = parsing::parse_tracks(*response);
                if (!tracks) return tracks.error();
                std::map<std::string, domain::Track> by_id;
                for (auto& track : *tracks) {
                    const auto id = track.ref.id.value();
                    if (std::find(ids.begin(), ids.end(), id) == ids.end())
                        return Error{ErrorCode::upstream_failed, "song batch returned an unrequested id", false, {}};
                    by_id.emplace(id, std::move(track));
                }
                for (const auto& id : ids) if (auto it = by_id.find(id); it != by_id.end()) page.page.items.push_back(it->second);
            }
        } else {
            page.page.items.assign(data->tracks.begin() + *offset, data->tracks.begin() + static_cast<std::ptrdiff_t>(end));
        }
        const auto returned = static_cast<std::uint32_t>(page.page.items.size());
        page.page.page = {q.limit, returned, data->detail.track_count, end < size, size, end - *offset - returned};
        if (end < size) page.next_cursor = std::to_string(end) + ":" + data->revision;
        return page;
    }
private:
    Client client_;
};

template<class Port, class Impl>
domain::Result<std::shared_ptr<Port>> make(const ports::ProviderSetup& s) {
    if (!s.upstream) return Error{ErrorCode::internal, "netease needs the upstream client", false, {}};
    return std::shared_ptr<Port>{std::make_shared<Impl>(s)};
}
} // namespace

domain::Result<std::shared_ptr<ports::PlaylistSearch>> make_playlist_search(const ports::ProviderSetup& s) {
    return make<ports::PlaylistSearch, Search<tag::SearchPlaylists>>(s);
}
domain::Result<std::shared_ptr<ports::AlbumSearch>> make_album_search(const ports::ProviderSetup& s) {
    return make<ports::AlbumSearch, Search<tag::SearchAlbums>>(s);
}
domain::Result<std::shared_ptr<ports::PlaylistDetail>> make_playlist_detail(const ports::ProviderSetup& s) {
    return make<ports::PlaylistDetail, Detail<tag::PlaylistDetail, true>>(s);
}
domain::Result<std::shared_ptr<ports::AlbumDetail>> make_album_detail(const ports::ProviderSetup& s) {
    return make<ports::AlbumDetail, Detail<tag::AlbumDetail, false>>(s);
}
domain::Result<std::shared_ptr<ports::PlaylistTracks>> make_playlist_tracks(const ports::ProviderSetup& s) {
    return make<ports::PlaylistTracks, Tracks<tag::PlaylistTracks, true>>(s);
}
domain::Result<std::shared_ptr<ports::AlbumTracks>> make_album_tracks(const ports::ProviderSetup& s) {
    return make<ports::AlbumTracks, Tracks<tag::AlbumTracks, false>>(s);
}
domain::Result<std::shared_ptr<ports::ChartTracks>> make_chart_tracks(const ports::ProviderSetup& s) {
    return make<ports::ChartTracks, Tracks<tag::ChartTracks, true>>(s);
}
} // namespace music_service::providers::netease
