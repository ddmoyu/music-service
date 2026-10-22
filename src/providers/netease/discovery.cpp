#include "providers/netease/capabilities.hpp"
#include "providers/netease/client.hpp"
#include "providers/netease/parsing/parsers.hpp"

#include <algorithm>
#include <charconv>
#include <deque>
#include <mutex>

namespace music_service::providers::netease {
namespace {
domain::Result<std::uint32_t> offset_of(std::string_view value) {
    std::uint32_t offset{};
    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), offset);
    if (value.empty() || ec != std::errc{} || end != value.data() + value.size() || offset > 10000)
        return Error{ErrorCode::cursor_mismatch, "invalid discovery offset", false, {}};
    return offset;
}

domain::Result<std::vector<domain::Chart>> charts_of(const Client& client, const ports::ProviderContext& c,
                                                 std::string_view operation) {
    auto request = protocol::charts(call_for());
    request.operation = std::string{operation};
    auto body = client.body(std::move(request), c);
    if (!body) return body.error();
    return parsing::parse_charts(*body);
}

class Categories final : public ports::ICategories {
public:
    explicit Categories(const ports::ProviderSetup& s) : client_(s) {}
    domain::Result<std::vector<domain::Category>> categories(const ports::ProviderContext& c) override {
        auto body = client_.body(protocol::categories(call_for()), c);
        if (!body) return body.error();
        return parsing::parse_categories(*body);
    }
private:
    Client client_;
};

class Charts final : public ports::ICharts, public ports::ChartDetail {
public:
    explicit Charts(const ports::ProviderSetup& s) : client_(s) {}
    domain::Result<std::vector<domain::Chart>> charts(const ports::ProviderContext& c) override {
        return charts_of(client_, c, "charts");
    }
    domain::Result<domain::Chart> detail(const ports::ProviderContext& c, const domain::PlatformRef& ref) override {
        if (ref.source.value() != protocol::source || !parsing::is_valid_song_id(ref.id.value()))
            return Error::validation("id", "is not a netease chart id");
        auto found = charts_of(client_, c, "chart_detail");
        if (!found) return found.error();
        for (const auto& chart : *found) if (chart.ref == ref) return chart;
        return Error{ErrorCode::not_found, "netease chart not found", false, {}};
    }
private:
    Client client_;
};

class CategoryPlaylists final : public ports::CategoryPlaylists {
public:
    explicit CategoryPlaylists(const ports::ProviderSetup& s) : client_(s) {}
    domain::Result<ports::CatalogPage<domain::Playlist>> list(const ports::ProviderContext& c,
        const domain::PlatformRef& ref, const ports::ListQuery& q) override {
        if (ref.source.value() != protocol::source) return Error::validation("source", "must be netease");
        if (q.limit < 1 || q.limit > 50) return Error::validation("limit", "must be between 1 and 50");
        auto offset = offset_of(q.cursor.value_or("0"));
        if (!offset) return offset.error();
        auto request = protocol::categories(call_for());
        request.operation = "category_playlists";
        auto directory = client_.body(std::move(request), c);
        if (!directory) return directory.error();
        auto categories = parsing::parse_categories(*directory);
        if (!categories) return categories.error();
        if (std::none_of(categories->begin(), categories->end(), [&](const auto& v) { return v.id == ref.id.value(); }))
            return Error{ErrorCode::not_found, "netease category not found", false, {}};
        auto body = client_.body(protocol::category_playlists(ref.id.value(), *offset, q.limit, call_for()), c);
        if (!body) return body.error();
        auto found = parsing::parse_discovery_playlists(*body, false);
        if (!found) return found.error();
        const auto count = static_cast<std::uint32_t>(found->items.size());
        if (count > q.limit) return Error{ErrorCode::upstream_failed, "category exceeded requested page", false, {}};
        const auto next = *offset + count;
        const bool more = found->more && next <= 10000;
        ports::CatalogPage<domain::Playlist> page;
        page.page.items = std::move(found->items);
        page.page.page = {q.limit, count, found->total, more};
        if (more) page.next_cursor = std::to_string(next);
        return page;
    }
private:
    Client client_;
};

class Recommendations final : public ports::IRecommendedPlaylists {
public:
    explicit Recommendations(const ports::ProviderSetup& s) : client_(s) {}
    domain::Result<ports::CatalogPage<domain::Playlist>> recommend_playlists(
        const ports::ProviderContext& c, const ports::ListQuery& q) override {
        if (q.limit < 1 || q.limit > 50) return Error::validation("limit", "must be between 1 and 50");
        std::uint32_t offset{};
        std::string token;
        const auto scope = c.identity.canonical() + ":" + std::to_string(q.limit) + ":";
        std::shared_ptr<const parsing::DiscoveryPlaylists> found;
        if (q.cursor) {
            auto colon = q.cursor->find(':');
            if (colon == std::string::npos) return Error{ErrorCode::cursor_mismatch, "invalid recommendation cursor", false, {}};
            auto parsed = offset_of(std::string_view{*q.cursor}.substr(0, colon));
            if (!parsed) return parsed.error();
            offset = *parsed;
            token = q.cursor->substr(colon + 1);
            const std::lock_guard lock{mutex_};
            purge();
            for (const auto& snapshot : snapshots_) if (snapshot.key == scope + token) found = snapshot.data;
            if (!found) return Error{ErrorCode::cursor_expired, "public recommendation snapshot expired; restart pagination", false, {}};
        } else {
            auto body = client_.body(protocol::recommendations(call_for()), c);
            if (!body) return body.error();
            auto parsed = parsing::parse_discovery_playlists(*body, true);
            if (!parsed) return parsed.error();
            const auto charge = body->size() + parsed->items.size() * sizeof(domain::Playlist) + scope.size();
            found = std::make_shared<const parsing::DiscoveryPlaylists>(std::move(*parsed));
            const std::lock_guard lock{mutex_};
            purge();
            token = std::to_string(++sequence_);
            snapshots_.push_back({scope + token, found, std::chrono::steady_clock::now(), charge});
            bytes_ += charge;
            while (snapshots_.size() > 64 || bytes_ > 8 * 1024 * 1024) pop_front();
        }
        if (offset > found->items.size()) return Error{ErrorCode::cursor_mismatch, "offset exceeds recommendation set", false, {}};
        const auto end = std::min<std::size_t>(offset + q.limit, found->items.size());
        ports::CatalogPage<domain::Playlist> page;
        page.page.items.assign(found->items.begin() + offset, found->items.begin() + static_cast<std::ptrdiff_t>(end));
        page.page.page = {q.limit, static_cast<std::uint32_t>(end - offset), std::nullopt,
                          end < found->items.size(), found->items.size(), 0};
        if (end < found->items.size()) page.next_cursor = std::to_string(end) + ":" + token;
        return page;
    }
private:
    struct Snapshot {
        std::string key;
        std::shared_ptr<const parsing::DiscoveryPlaylists> data;
        std::chrono::steady_clock::time_point created;
        std::size_t charge;
    };
    void pop_front() { bytes_ -= snapshots_.front().charge; snapshots_.pop_front(); }
    void purge() {
        const auto now = std::chrono::steady_clock::now();
        while (!snapshots_.empty() && snapshots_.front().created + std::chrono::minutes(10) <= now) pop_front();
    }
    Client client_;
    std::mutex mutex_;
    std::deque<Snapshot> snapshots_;
    std::size_t bytes_{};
    std::uint64_t sequence_{};
};

template<class Port, class Impl> domain::Result<std::shared_ptr<Port>> make(const ports::ProviderSetup& s) {
    if (!s.upstream) return Error{ErrorCode::internal, "netease needs the upstream client", false, {}};
    return std::shared_ptr<Port>{std::make_shared<Impl>(s)};
}
} // namespace
domain::Result<std::shared_ptr<ports::ICategories>> make_categories(const ports::ProviderSetup& s) {
    return make<ports::ICategories, Categories>(s);
}
domain::Result<std::shared_ptr<ports::CategoryPlaylists>> make_category_playlists(const ports::ProviderSetup& s) {
    return make<ports::CategoryPlaylists, CategoryPlaylists>(s);
}
domain::Result<std::shared_ptr<ports::IRecommendedPlaylists>> make_recommendations(const ports::ProviderSetup& s) {
    return make<ports::IRecommendedPlaylists, Recommendations>(s);
}
domain::Result<std::shared_ptr<ports::ICharts>> make_charts(const ports::ProviderSetup& s) {
    return make<ports::ICharts, Charts>(s);
}
domain::Result<std::shared_ptr<ports::ChartDetail>> make_chart_detail(const ports::ProviderSetup& s) {
    return make<ports::ChartDetail, Charts>(s);
}
} // namespace music_service::providers::netease
