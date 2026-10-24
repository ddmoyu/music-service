#include "providers/bilibili/internal.hpp"

#include <set>

namespace music_service::providers::bilibili {
VideoPart view(const Client& client, const ports::ProviderContext& context,
               const domain::TrackRef& ref, std::string_view operation) {
    const auto id = parse_id(ref);
    const auto doc = client.get(operation, "/x/web-interface/view?bvid=" + id.bvid, context);
    const auto& data = doc.at("data");
    if (text(data, "bvid") != id.bvid)
        throw std::invalid_argument{"video identity"};
    const auto& pages = data.at("pages");
    if (!pages.is_array() || pages.empty() || pages.size() > max_parts)
        throw std::invalid_argument{"video pages"};
    std::set<std::int64_t> cids, numbers;
    const Json* selected = nullptr;
    for (const auto& page : pages) {
        const auto cid = integer(page.at("cid"), 1, INT64_MAX);
        const auto number = integer(page.at("page"), 1, max_parts);
        if (!cids.insert(cid).second || !numbers.insert(number).second)
            throw std::invalid_argument{"duplicate part"};
        if (id.cid ? *id.cid == static_cast<std::uint64_t>(cid) : number == id.page)
            selected = &page;
    }
    if (!selected)
        throw Error{ErrorCode::not_found, "Bilibili video part was not found", false, {}};
    const auto cid = static_cast<std::uint64_t>(integer(selected->at("cid"), 1, INT64_MAX));
    auto title = text(data, "title");
    const auto part_title = text(*selected, "part", false);
    if (pages.size() > 1 && !part_title.empty() && part_title != title)
        title += " - " + part_title;
    domain::Track track{
        make_ref(id.bvid + ":cid" + std::to_string(cid)), std::move(title), {}, {}, {}, {}};
    const auto duration = integer(selected->at("duration"), 0, 7 * 86400);
    if (duration > 0)
        track.duration_ms = duration * 1000;
    track.artwork = artwork(data);
    // owner 是上传者，不是歌手；视频分 P 也不冒充专辑曲号。
    bool paid_preview = false;
    if (data.contains("rights")) {
        const auto& rights = data.at("rights");
        for (const char* key : {"pay", "ugc_pay", "ugc_pay_preview", "arc_pay"})
            if (rights.contains(key) && integer(rights.at(key), 0, 1) == 1)
                paid_preview = true;
    }
    if (paid_preview) track.pay.pay_play = 1;
    // A paid UGC video is not necessarily covered by the platform VIP subscription.
    return {std::move(track), id.bvid, cid, paid_preview};
}

namespace {
class Catalog final : public ports::ITrackSearch, public ports::ITrackDetail {
  public:
    explicit Catalog(const ports::ProviderSetup& setup) : client_(setup) {}
    domain::Result<ports::TrackSearchPage>
    search_tracks(const ports::ProviderContext& context,
                  const ports::TrackSearchQuery& query) override {
        return checked([&] {
            if (query.limit == 0 || query.limit > 50 || query.text.empty() ||
                query.text.size() > 512 || !domain::is_valid_utf8(query.text))
                throw Error::validation("query",
                                        "Bilibili requires a keyword and limit between 1 and 50");
            std::uint64_t page = 1;
            if (query.cursor) {
                try {
                    const auto cursor = std::string_view{*query.cursor};
                    const auto at = cursor.find(':');
                    if (at == cursor.npos)
                        throw std::invalid_argument{"cursor"};
                    page = decimal(cursor.substr(0, at), 1000);
                    if (decimal(cursor.substr(at + 1), 50) != query.limit)
                        throw std::invalid_argument{"cursor limit"};
                } catch (const std::invalid_argument&) {
                    throw Error{
                        ErrorCode::cursor_mismatch, "invalid Bilibili search cursor", false, {}};
                }
            }
            auto doc = client_.get("search_tracks",
                                   "/x/web-interface/search/type?search_type=video&keyword=" +
                                       application::transport::encode_query_component(query.text) +
                                       "&page=" + std::to_string(page) +
                                       "&page_size=" + std::to_string(query.limit),
                                   context);
            const auto& data = doc.at("data");
            if (integer(data.at("page"), 1, 1000) != static_cast<std::int64_t>(page) ||
                integer(data.at("pagesize"), 1, 50) != query.limit)
                throw std::invalid_argument{"search window"};
            const auto total = integer(data.at("numResults"), 0, 10000000);
            const auto pages = integer(data.at("numPages"), 0, 10000000);
            const Json empty = Json::array();
            const auto& rows = data.contains("result") ? data.at("result") : empty;
            if (!rows.is_array() || rows.size() > query.limit || (!rows.empty() && total == 0) ||
                (rows.empty() && total > 0 && page <= static_cast<std::uint64_t>(pages)))
                throw std::invalid_argument{"search result count"};
            ports::TrackSearchPage result;
            for (const auto& row : rows) {
                const auto bvid = text(row, "bvid");
                if (!valid_bvid(bvid))
                    throw std::invalid_argument{"search bvid"};
                domain::Track track{
                    make_ref(bvid + ":p1"), clean_title(text(row, "title")), {}, {}, {}, {}};
                track.artwork = artwork(row);
                // 搜索 duration 可能是多 P 总时长，不能作为第一 P 的整曲证据。
                result.page.items.push_back(std::move(track));
            }
            const bool more =
                !rows.empty() && page < static_cast<std::uint64_t>(pages) && page < 1000;
            result.page.page = {query.limit, static_cast<std::uint32_t>(rows.size()),
                                static_cast<std::uint64_t>(total), more};
            if (more)
                result.next_cursor = std::to_string(page + 1) + ":" + std::to_string(query.limit);
            return result;
        });
    }
    domain::Result<domain::Track> track_detail(const ports::ProviderContext& context,
                                               const domain::TrackRef& ref) override {
        return checked([&] { return view(client_, context, ref, "track_detail").track; });
    }

  private:
    Client client_;
};

class Links final : public ports::ILinkParser {
  public:
    domain::Result<ports::ParsedLink> parse_link(std::string_view input) override {
        try {
            if (input.starts_with("bilibili:"))
                input.remove_prefix(9);
            if (input.starts_with("BV")) {
                auto ref = make_ref(std::string{input});
                auto id = parse_id(ref);
                if (!id.cid)
                    ref = make_ref(id.bvid + ":p" + std::to_string(id.page));
                return ports::ParsedLink{"track", std::move(ref)};
            }
            auto url = application::transport::Url::parse(input);
            if (!url ||
                (url->host() != "www.bilibili.com" && url->host() != "bilibili.com" &&
                 url->host() != "m.bilibili.com") ||
                url->port() != (url->is_https() ? 443 : 80) || !url->path().starts_with("/video/"))
                throw std::invalid_argument{"link origin"};
            auto bvid = url->path().substr(7);
            if (bvid.ends_with('/'))
                bvid.remove_suffix(1);
            if (!valid_bvid(bvid))
                throw std::invalid_argument{"video path"};
            std::uint64_t page = 1;
            bool seen_page = false;
            auto query = std::string_view{url->target()}.substr(url->path().size());
            if (!query.empty())
                query.remove_prefix(1);
            while (!query.empty()) {
                const auto end = query.find('&');
                const auto part = query.substr(0, end);
                query = end == query.npos ? std::string_view{} : query.substr(end + 1);
                const auto at = part.find('=');
                const auto key = part.substr(0, at);
                // 编码的身份键、重复 p 及其他身份覆盖参数都属于歧义链接。
                if (key.find('%') != key.npos || key == "bvid" || key == "aid" || key == "cid")
                    throw std::invalid_argument{"ambiguous identity"};
                if (key == "p") {
                    if (seen_page || at == part.npos)
                        throw std::invalid_argument{"duplicate page"};
                    page = decimal(part.substr(at + 1), max_parts);
                    seen_page = true;
                }
            }
            return ports::ParsedLink{"track",
                                     make_ref(std::string{bvid} + ":p" + std::to_string(page))};
        } catch (const Error&) {
            return Error::validation("link", "invalid Bilibili video link");
        } catch (const std::invalid_argument&) {
            return Error::validation("link", "invalid or ambiguous Bilibili video link");
        }
    }
};

template <class Port> void bind_catalog(ports::ProviderBundle& bundle) {
    bundle.bindings.push_back(ports::CapabilityBinding::bind<Port>(
        [](const ports::ProviderSetup& setup) -> domain::Result<std::shared_ptr<Port>> {
            if (!setup.upstream)
                return Error{
                    ErrorCode::internal, "Bilibili requires the upstream client", false, {}};
            return std::shared_ptr<Port>{std::make_shared<Catalog>(setup)};
        }));
}
} // namespace

void add_catalog(ports::ProviderBundle& bundle) {
    bind_catalog<ports::ITrackSearch>(bundle);
    bind_catalog<ports::ITrackDetail>(bundle);
    bundle.bindings.push_back(ports::CapabilityBinding::bind<ports::ILinkParser>(
        [](const ports::ProviderSetup&) -> domain::Result<std::shared_ptr<ports::ILinkParser>> {
            return std::shared_ptr<ports::ILinkParser>{std::make_shared<Links>()};
        }));
}
} // namespace music_service::providers::bilibili
