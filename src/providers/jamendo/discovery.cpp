#include "providers/jamendo/parsing.hpp"
#include <algorithm>
#include <set>

namespace music_service::providers::jamendo {
namespace {
constexpr std::uint32_t chart_size = 100;

std::string folded(std::string text) {
    // ASCII 忽略大小写，其他 UTF-8 字节保留；不依赖进程区域设置。
    for (auto& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}

std::vector<domain::Category> tags(const Client& client, const ports::ProviderContext& c, std::string_view operation) {
    auto data = client.get(operation, "/api/tags",
        "category%5B%5D=genre&category%5B%5D=mood&category%5B%5D=theme&order=featuredRank&lang=en&limit=100", c);
    // 此目录忽略 offset；接近请求上限时不能声称目录完整。
    if (data.size() >= 100) throw std::invalid_argument{"tag directory exceeds bound"};
    std::vector<domain::Category> result;
    std::set<std::string> seen;
    for (const auto& row : data) {
        auto id = parsing::id(row);
        auto name = parsing::clean_text(parsing::text(row, "name", true));
        auto group = parsing::text(row, "category", true);
        if (name.empty() || !seen.insert(id).second || (group != "genre" && group != "mood" && group != "theme"))
            throw std::invalid_argument{"tag directory"};
        result.push_back({std::move(id), std::move(name), std::move(group), domain::ListedKind::track});
    }
    return result;
}

class Categories final : public ports::ICategories, public ports::CategoryTracks {
public:
    explicit Categories(const ports::ProviderSetup& setup) : client_(setup) {}
    domain::Result<std::vector<domain::Category>> categories(const ports::ProviderContext& c) override {
        return checked([&] { return tags(client_, c, "categories"); });
    }
    domain::Result<ports::CatalogPage<domain::Track>> list(const ports::ProviderContext& c,
        const domain::PlatformRef& ref, const ports::ListQuery& q) override {
        return checked([&] {
            check_ref(ref);
            const auto start = offset(q);
            const auto directory = tags(client_, c, "category_tracks");
            if (std::none_of(directory.begin(), directory.end(), [&](const auto& item) { return item.id == ref.id.value(); }))
                throw Error{ErrorCode::not_found, "Jamendo category was not found", false, {}};
            auto data = client_.get("category_tracks", "/api/tracks", "tagId=" + ref.id.value() +
                "&order=hotness&limit=" + std::to_string(q.limit + 1) + "&offset=" + std::to_string(start), c);
            if (data.size() > q.limit + 1) throw std::invalid_argument{"category page limit"};
            const auto consumed = std::min<std::size_t>(data.size(), q.limit);
            std::vector<Json> rows;
            std::set<std::string> seen;
            for (const auto& row : data) {
                // /api/tracks 的原生详情不带 type；出现显式类型时仍须匹配。
                if ((row.contains("type") && parsing::text(row, "type", true) != "track") || !seen.insert(parsing::id(row)).second)
                    throw std::invalid_argument{"category track identity"};
            }
            for (std::size_t i = 0; i < consumed; ++i) if (parsing::available(data[i])) rows.push_back(data[i]);
            const bool more = data.size() > q.limit;
            if (more && start + q.limit >= max_collection) throw std::invalid_argument{"category pagination bound"};
            ports::CatalogPage<domain::Track> result;
            result.page.items = parsing::tracks(client_, c, "category_tracks", rows);
            const auto count = static_cast<std::uint32_t>(result.page.items.size());
            result.page.page = {q.limit, count, {}, more, {}, consumed - count};
            if (more) result.next_cursor = std::to_string(start + q.limit);
            return result;
        });
    }
private: Client client_;
};

class PlaylistSearch final : public ports::PlaylistSearch {
public:
    explicit PlaylistSearch(const ports::ProviderSetup& setup) : client_(setup) {}
    domain::Result<ports::CatalogPage<domain::Playlist>> search(const ports::ProviderContext& c,
        const ports::SearchQuery& q) override {
        return checked([&] {
            check_search(q.text);
            if (q.limit > 50) throw Error::validation("limit", "must be between 1 and 50");
            const ports::ListQuery page{q.limit, q.cursor};
            const auto start = offset(page, true);
            const auto keyword = folded(q.text);
            std::vector<domain::Playlist> matches;
            Json revision_rows = Json::array();
            std::set<std::string> seen;
            bool complete = false;
            // 官网没有可用的全站歌单搜索。仅检索官方精选目录，不展开歌单曲目详情。
            for (std::uint32_t at = 0; at < 1000; at += 100) {
                auto data = client_.get("search_playlists", "/api/playlists/featured",
                    "limit=100&offset=" + std::to_string(at), c);
                if (data.size() > 100) throw std::invalid_argument{"featured page limit"};
                for (const auto& row : data) {
                    if (parsing::text(row, "type", true) != "playlist" || !seen.insert(parsing::id(row)).second)
                        throw std::invalid_argument{"featured directory identity"};
                    if (!parsing::available(row)) continue;
                    auto item = parsing::playlist(row);
                    if (folded(item.name + "\n" + item.description.value_or("")).find(keyword) != std::string::npos) {
                        revision_rows.push_back({item.ref.id.value(), item.name, item.description.value_or("")});
                        matches.push_back(std::move(item));
                    }
                }
                if (data.size() < 100) { complete = true; break; }
            }
            if (!complete) throw std::invalid_argument{"featured directory exceeds bound"};
            const auto revision = digest(Json{"featured_search", keyword, revision_rows}.dump());
            same_revision(page, revision);
            if (start > matches.size()) throw Error{ErrorCode::cursor_mismatch, "Jamendo search offset exceeds matches", false, {}};
            const auto end = std::min<std::size_t>(start + q.limit, matches.size());
            ports::CatalogPage<domain::Playlist> result;
            result.page.items.assign(matches.begin() + start, matches.begin() + end);
            // 上游没有声明全站总数；listed_total 仅为精选范围内的匹配数量。
            result.page.page = {q.limit, static_cast<std::uint32_t>(end - start), {}, end < matches.size(), matches.size(), 0};
            if (end < matches.size()) result.next_cursor = std::to_string(end) + ":" + revision;
            return result;
        });
    }
private: Client client_;
};

domain::Chart chart(std::string id, std::string name) {
    domain::Chart result{{take(domain::SourceId::parse("jamendo")), take(domain::PlatformId::parse(id))}, std::move(name)};
    result.description = "Jamendo 官方歌曲榜前 100 项；榜单顺序来自网站接口，未知总数和更新时间保持空值";
    return result;
}
std::vector<domain::Chart> chart_directory(const Client& client, const ports::ProviderContext& c, std::string_view op) {
    std::vector<domain::Chart> result{chart("bestof", "Jamendo Best Of · Top 100"), chart("trending", "Jamendo Trending · Top 100")};
    for (const auto& tag : tags(client, c, op)) if (tag.group == "genre")
        result.push_back(chart("genre:" + tag.id, tag.name + " · Best Of · Top 100"));
    return result;
}
void check_chart_ref(const domain::PlatformRef& ref) {
    const auto& id = ref.id.value();
    if (ref.source.value() != "jamendo" || (id != "bestof" && id != "trending" &&
        (!id.starts_with("genre:") || !decimal(std::string_view{id}.substr(6)))))
        throw Error::validation("id", "invalid Jamendo chart identifier");
}
domain::Chart chart_detail(const Client& client, const ports::ProviderContext& c, std::string_view op,
    const domain::PlatformRef& ref) {
    check_chart_ref(ref);
    const auto directory = chart_directory(client, c, op);
    for (const auto& item : directory) if (item.ref == ref) return item;
    throw Error{ErrorCode::not_found, "Jamendo chart was not found", false, {}};
}

class Charts final : public ports::ICharts, public ports::ChartDetail, public ports::ChartTracks {
public:
    explicit Charts(const ports::ProviderSetup& setup) : client_(setup) {}
    domain::Result<std::vector<domain::Chart>> charts(const ports::ProviderContext& c) override {
        return checked([&] { return chart_directory(client_, c, "charts"); });
    }
    domain::Result<domain::Chart> detail(const ports::ProviderContext& c, const domain::PlatformRef& ref) override {
        return checked([&] { return chart_detail(client_, c, "chart_detail", ref); });
    }
    domain::Result<ports::CatalogPage<domain::Track>> list(const ports::ProviderContext& c,
        const domain::PlatformRef& ref, const ports::ListQuery& q) override {
        return checked([&] {
            check_chart_ref(ref);
            const auto start = offset(q, true);
            chart_detail(client_, c, "chart_tracks", ref);
            std::string query = "limit=" + std::to_string(chart_size) + "&offset=0";
            if (ref.id.value() == "trending") query += "&order=trending";
            else if (ref.id.value().starts_with("genre:")) query += "&tagId=" + ref.id.value().substr(6);
            auto data = client_.get("chart_tracks", "/api/charts/track", std::move(query), c);
            if (data.size() > chart_size) throw std::invalid_argument{"chart size"};
            std::set<std::string> seen;
            Json members = Json::array();
            std::uint64_t previous = 0;
            for (const auto& row : data) {
                const auto id = parsing::id(row);
                const auto position = parsing::number(row, "position");
                if (parsing::text(row, "type", true) != "track" || position <= previous ||
                    position > chart_size || !seen.insert(id).second) throw std::invalid_argument{"chart ordering"};
                members.push_back({{"id", id}, {"position", position}});
                previous = position;
            }
            const auto revision = digest(Json{ref.id.value(), members}.dump());
            same_revision(q, revision);
            if (start > members.size()) throw Error{ErrorCode::cursor_mismatch, "Jamendo offset exceeds chart", false, {}};
            const auto end = std::min<std::size_t>(start + q.limit, members.size());
            std::vector<std::string> ids;
            for (std::size_t i = start; i < end; ++i) ids.push_back(parsing::id(members[i]));
            const auto records = client_.batch("chart_tracks", "tracks", ids, c);
            std::vector<Json> rows;
            for (const auto& id : ids) if (auto it = records.find(id); it != records.end() && parsing::available(it->second))
                rows.push_back(it->second);
            ports::CatalogPage<domain::Track> result;
            result.page.items = parsing::tracks(client_, c, "chart_tracks", rows);
            const auto count = static_cast<std::uint32_t>(result.page.items.size());
            result.page.page = {q.limit, count, {}, end < members.size(), members.size(), end - start - count};
            if (end < members.size()) result.next_cursor = std::to_string(end) + ":" + revision;
            return result;
        });
    }
private: Client client_;
};
}
void add_discovery(ports::ProviderBundle& bundle) {
    bind<ports::PlaylistSearch, PlaylistSearch>(bundle);
    bind<ports::ICategories, Categories>(bundle);
    bind<ports::CategoryTracks, Categories>(bundle);
    bind<ports::ICharts, Charts>(bundle);
    bind<ports::ChartDetail, Charts>(bundle);
    bind<ports::ChartTracks, Charts>(bundle);
}
}
