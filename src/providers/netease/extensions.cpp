#include "providers/netease/capabilities.hpp"
#include "providers/netease/client.hpp"
#include "providers/netease/parsing/parsers.hpp"

#include <algorithm>
#include <charconv>

namespace music_service::providers::netease {
namespace {
std::string_view cookie(const ports::ProviderContext& c) { return c.secret ? c.secret->reveal() : std::string_view{}; }
domain::Result<void> entity(const domain::PlatformRef& r) {
    if (r.source.value() != protocol::source || !parsing::is_valid_song_id(r.id.value()))
        return Error::validation("id", "is not a netease entity id");
    return domain::ok();
}
domain::Result<std::uint32_t> offset(std::string_view s) {
    std::uint32_t n{};
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
    if (s.empty() || ec != std::errc{} || end != s.data() + s.size() || n > 10000)
        return Error{ErrorCode::cursor_mismatch, "invalid catalog offset", false, {}};
    return n;
}

class Personal final : public ports::IUserPlaylists {
public:
    explicit Personal(const ports::ProviderSetup& s) : client_(s) {}
    domain::Result<ports::CatalogPage<domain::Playlist>> user_playlists(
        const ports::ProviderContext& c, const ports::ListQuery& q) override {
        const auto secret = cookie(c);
        if (!protocol::music_u_of(secret)) return Error{ErrorCode::credentials_required, "personal playlists need an account", false, {}};
        if (q.limit < 1 || q.limit > 50) return Error::validation("limit", "must be between 1 and 50");
        auto at = offset(q.cursor.value_or("0"));
        if (!at) return at.error();
        auto request = protocol::account(call_for(secret));
        request.operation = "user_playlists";
        auto body = client_.body(std::move(request), c, secret);
        if (!body) return body.error();
        auto account = parsing::parse_account(*body);
        if (!account) return account.error();
        if (!account->signed_in || !account->user_id)
            return Error{ErrorCode::credentials_required, "netease session expired", false, {}};
        body = client_.body(protocol::user_playlists(*account->user_id, *at, q.limit, call_for(secret)), c, secret);
        if (!body) return body.error();
        auto found = parsing::parse_user_playlists(*body, *account->user_id);
        if (!found) return found.error();
        const auto count = static_cast<std::uint32_t>(found->items.size());
        if (count > q.limit) return Error{ErrorCode::upstream_failed, "personal page exceeds limit", false, {}};
        if (found->more && *at + count >= 10000)
            return Error{ErrorCode::upstream_failed, "personal catalog exceeds the 10000 item enumeration bound", false, {}};
        ports::CatalogPage<domain::Playlist> page;
        page.page.items = std::move(found->items);
        const bool more = found->more && *at + count < 10000;
        page.page.page = {q.limit, count, std::nullopt, more};
        if (more) page.next_cursor = std::to_string(*at + count);
        return page;
    }
private:
    Client client_;
};

class HotSongs final : public ports::ArtistTracks {
public:
    explicit HotSongs(const ports::ProviderSetup& s) : client_(s) {}
    domain::Result<ports::CatalogPage<domain::Track>> list(const ports::ProviderContext& c,
        const domain::PlatformRef& r, const ports::ListQuery& q) override {
        if (auto v = entity(r); !v) return v.error();
        if (q.limit < 1 || q.limit > 50) return Error::validation("limit", "hot songs limit must be between 1 and 50");
        auto body = client_.body(protocol::artist_tracks(r.id.value(), call_for()), c);
        if (!body) return body.error();
        auto found = parsing::parse_artist_tracks(*body, r.id.value());
        if (!found) return found.error();
        std::uint32_t at{};
        if (q.cursor) {
            const auto split = q.cursor->find(':');
            if (split == std::string::npos) return Error{ErrorCode::cursor_mismatch, "invalid artist cursor", false, {}};
            auto n = offset(std::string_view{*q.cursor}.substr(0, split));
            if (!n) return n.error();
            if (q.cursor->substr(split + 1) != found->revision)
                return Error{ErrorCode::cursor_expired, "hot songs changed; restart pagination", false, {}};
            at = *n;
        }
        if (at > found->tracks.size()) return Error{ErrorCode::cursor_mismatch, "artist offset out of bounds", false, {}};
        const auto end = std::min<std::size_t>(at + q.limit, found->tracks.size());
        ports::CatalogPage<domain::Track> page;
        page.page.items.assign(found->tracks.begin() + at, found->tracks.begin() + static_cast<std::ptrdiff_t>(end));
        page.page.page = {q.limit, static_cast<std::uint32_t>(end - at), std::nullopt,
            end < found->tracks.size(), found->tracks.size(), 0}; // hot 50, never all works
        if (end < found->tracks.size()) page.next_cursor = std::to_string(end) + ":" + found->revision;
        return page;
    }
private:
    Client client_;
};

class Artists final : public ports::ArtistSearch, public ports::ArtistAlbums {
public:
    explicit Artists(const ports::ProviderSetup& s) : client_(s) {}
    domain::Result<ports::CatalogPage<domain::Artist>> search(const ports::ProviderContext& c,
        const ports::SearchQuery& q) override {
        if (q.text.empty() || q.text.size() > 200) return Error::validation("q", "must be 1 to 200 bytes");
        if (q.limit < 1 || q.limit > 50) return Error::validation("limit", "must be between 1 and 50");
        auto at = offset(q.cursor.value_or("0"));
        if (!at) return at.error();
        auto body = client_.body(protocol::search_artists(q.text, *at, q.limit, call_for()), c);
        if (!body) return body.error();
        auto found = parsing::parse_artist_search(*body);
        if (!found) return found.error();
        const auto count = static_cast<std::uint32_t>(found->items.size());
        if (count > q.limit) return Error{ErrorCode::upstream_failed, "artist search exceeds limit", false, {}};
        const bool more = count > 0 && *at + count < found->total;
        if (more && *at + count >= 10000)
            return Error{ErrorCode::upstream_failed, "artist search exceeds enumeration bound", false, {}};
        ports::CatalogPage<domain::Artist> page;
        page.page.items = std::move(found->items);
        page.page.page = {q.limit, count, found->total, more};
        if (more) page.next_cursor = std::to_string(*at + count);
        return page;
    }
    domain::Result<ports::CatalogPage<domain::Album>> list(const ports::ProviderContext& c,
        const domain::PlatformRef& r, const ports::ListQuery& q) override {
        if (auto v = entity(r); !v) return v.error();
        if (q.limit < 1 || q.limit > 50) return Error::validation("limit", "must be between 1 and 50");
        auto at = offset(q.cursor.value_or("0"));
        if (!at) return at.error();
        auto body = client_.body(protocol::artist_albums(r.id.value(), *at, q.limit, call_for()), c);
        if (!body) return body.error();
        auto found = parsing::parse_artist_albums(*body, r.id.value());
        if (!found) return found.error();
        const auto count = static_cast<std::uint32_t>(found->items.size());
        if (count > q.limit) return Error{ErrorCode::upstream_failed, "artist albums exceeds limit", false, {}};
        if (found->more && *at + count >= 10000)
            return Error{ErrorCode::upstream_failed, "artist albums exceeds enumeration bound", false, {}};
        ports::CatalogPage<domain::Album> page;
        page.page.items = std::move(found->items);
        page.page.page = {q.limit, count, std::nullopt, found->more};
        if (found->more) page.next_cursor = std::to_string(*at + count);
        return page;
    }
private:
    Client client_;
};

class Podcasts final : public ports::PodcastDetail, public ports::PodcastEpisodes {
public:
    explicit Podcasts(const ports::ProviderSetup& s) : client_(s) {}
    domain::Result<domain::Podcast> detail(const ports::ProviderContext& c, const domain::PlatformRef& r) override {
        if (auto v = entity(r); !v) return v.error();
        auto body = client_.body(protocol::podcast(r.id.value(), call_for(cookie(c))), c, cookie(c));
        if (!body) return body.error();
        return parsing::parse_podcast(*body, r.id.value());
    }
    domain::Result<ports::CatalogPage<domain::Episode>> list(const ports::ProviderContext& c,
        const domain::PlatformRef& r, const ports::ListQuery& q) override {
        if (auto v = entity(r); !v) return v.error();
        if (q.limit < 1 || q.limit > 100) return Error::validation("limit", "must be between 1 and 100");
        auto at = offset(q.cursor.value_or("0"));
        if (!at) return at.error();
        auto body = client_.body(protocol::episodes(r.id.value(), *at, q.limit, call_for(cookie(c))), c, cookie(c));
        if (!body) return body.error();
        auto found = parsing::parse_episodes(*body, r.id.value());
        if (!found) return found.error();
        const auto count = static_cast<std::uint32_t>(found->items.size());
        if (count > q.limit) return Error{ErrorCode::upstream_failed, "episode page exceeds limit", false, {}};
        if (found->more && *at + count >= 10000)
            return Error{ErrorCode::upstream_failed, "podcast exceeds the 10000 episode enumeration bound", false, {}};
        ports::CatalogPage<domain::Episode> page;
        page.page.items = std::move(found->items);
        const bool more = found->more && *at + count < 10000;
        page.page.page = {q.limit, count, found->total, more};
        if (more) page.next_cursor = std::to_string(*at + count);
        return page;
    }
private:
    Client client_;
};
class Episodes final : public ports::EpisodeDetail {
public:
    explicit Episodes(const ports::ProviderSetup& s) : client_(s) {}
    domain::Result<domain::Episode> detail(const ports::ProviderContext& c, const domain::PlatformRef& r) override {
        if (auto v = entity(r); !v) return v.error();
        auto body = client_.body(protocol::episode(r.id.value(), call_for(cookie(c))), c, cookie(c));
        if (!body) return body.error();
        return parsing::parse_episode(*body, r.id.value());
    }
private:
    Client client_;
};
template<class Port, class Impl> domain::Result<std::shared_ptr<Port>> make(const ports::ProviderSetup& s) {
    if (!s.upstream) return Error{ErrorCode::internal, "netease needs the upstream client", false, {}};
    return std::shared_ptr<Port>{std::make_shared<Impl>(s)};
}
} // namespace
domain::Result<std::shared_ptr<ports::IUserPlaylists>> make_user_playlists(const ports::ProviderSetup& s) {
    return make<ports::IUserPlaylists, Personal>(s);
}
domain::Result<std::shared_ptr<ports::ArtistTracks>> make_artist_tracks(const ports::ProviderSetup& s) {
    return make<ports::ArtistTracks, HotSongs>(s);
}
domain::Result<std::shared_ptr<ports::ArtistSearch>> make_artist_search(const ports::ProviderSetup& s) {
    return make<ports::ArtistSearch, Artists>(s);
}
domain::Result<std::shared_ptr<ports::ArtistAlbums>> make_artist_albums(const ports::ProviderSetup& s) {
    return make<ports::ArtistAlbums, Artists>(s);
}
domain::Result<std::shared_ptr<ports::PodcastDetail>> make_podcast_detail(const ports::ProviderSetup& s) {
    return make<ports::PodcastDetail, Podcasts>(s);
}
domain::Result<std::shared_ptr<ports::PodcastEpisodes>> make_podcast_episodes(const ports::ProviderSetup& s) {
    return make<ports::PodcastEpisodes, Podcasts>(s);
}
domain::Result<std::shared_ptr<ports::EpisodeDetail>> make_episode_detail(const ports::ProviderSetup& s) {
    return make<ports::EpisodeDetail, Episodes>(s);
}
} // namespace music_service::providers::netease
