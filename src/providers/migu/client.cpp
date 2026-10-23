#include "providers/migu/client.hpp"
#include "application/transport/url.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <mbedtls/sha256.h>

namespace music_service::providers::migu {
bool decimal(std::string_view v) {
    return !v.empty() && v.size() <= 20 && v.front() != '0' &&
           std::all_of(v.begin(), v.end(), [](char c) { return c >= '0' && c <= '9'; });
}
void check_ref(const domain::PlatformRef& ref, bool album) {
    std::string_view id = ref.id.value();
    if (album && id.starts_with("d:"))
        id.remove_prefix(2);
    if (ref.source.value() != "migu" || !decimal(id))
        throw Error::validation("id", "invalid Migu entity identifier");
}
std::string digest(std::string_view value) {
    std::array<unsigned char, 32> bytes{};
    if (mbedtls_sha256(reinterpret_cast<const unsigned char*>(value.data()), value.size(),
                       bytes.data(), 0) != 0)
        throw Error{ErrorCode::internal, "Migu cursor hashing failed", false, {}};
    std::string out;
    for (auto b : bytes) {
        out += "0123456789abcdef"[b >> 4];
        out += "0123456789abcdef"[b & 15];
    }
    return out;
}
Json parse_json(std::string_view body) {
    if (body.size() > 4 * 1024 * 1024)
        throw std::invalid_argument{"body limit"};
    bool deep = false;
    auto doc = Json::parse(
        body,
        [&](int depth, Json::parse_event_t, Json&) {
            if (depth > 32)
                deep = true;
            return !deep;
        },
        false);
    if (deep || !doc.is_object())
        throw std::invalid_argument{"JSON"};
    return doc;
}
void array_shape(const Json& value, std::size_t max) {
    if (!value.is_array() || value.size() > max)
        throw std::invalid_argument{"array limit"};
}
Cursor cursor(const std::optional<std::string>& value, std::uint32_t limit) {
    if (limit == 0 || limit > 50)
        throw Error::validation("limit", "must be between 1 and 50");
    if (!value)
        return {};
    Cursor out;
    const auto colon = value->find(':');
    const auto part = std::string_view{*value}.substr(0, colon);
    const auto [end, ec] = std::from_chars(part.data(), part.data() + part.size(), out.offset);
    if (part.empty() || part.front() == '0' || ec != std::errc{} ||
        end != part.data() + part.size() || out.offset > 10000)
        throw Error{ErrorCode::cursor_mismatch, "invalid Migu page offset", false, {}};
    if (colon != value->npos) {
        out.revision = value->substr(colon + 1);
        if (out.revision.size() != 64 ||
            out.revision.find_first_not_of("0123456789abcdef") != out.revision.npos)
            throw Error{ErrorCode::cursor_mismatch, "invalid Migu page revision", false, {}};
    }
    return out;
}
void same_revision(const Cursor& value, std::string_view revision) {
    if (value.offset && value.revision != revision)
        throw Error{ErrorCode::cursor_expired,
                    "Migu collection changed; restart from its first page",
                    false,
                    {}};
}
std::string Client::fetch(std::string_view op, std::string url, const ports::ProviderContext& c,
                          bool auth) const {
    ports::HttpRequest request;
    request.url = std::move(url);
    request.operation = op;
    request.max_response_bytes = 4 * 1024 * 1024;
    request.headers = {{"User-Agent", std::string{user_agent}},
                       {"Referer", "https://music.migu.cn/"},
                       {"Accept", "application/json"},
                       {"channel", "0146832"},
                       {"version", "7.41.13"},
                       {"recommendstatus", "1"}};
    if (auth && c.secret)
        request.secret_headers.push_back({"Cookie", std::string{c.secret->reveal()}});
    ports::AdmissionRequest admission{c.identity.binding().principal_id(),
                                      setup_.source,
                                      std::string{op},
                                      ports::TrafficClass::short_request,
                                      c.identity,
                                      {},
                                      setup_.rate_limit_group,
                                      c.operation.deadline};
    auto reply = take(setup_.upstream->fetch(std::move(request), admission, c.operation));
    if (auto pending = std::get_if<ports::PendingHop>(&reply))
        throw Error{ErrorCode::service_busy,
                    "Migu capacity is not available",
                    true,
                    {{"admission", pending->deferred.blocked_dimension}}};
    auto response = std::get<ports::HttpResponse>(std::move(reply));
    const auto status = response.head.status;
    if (status != 200)
        throw Error{status == 429 ? ErrorCode::upstream_rate_limited : ErrorCode::upstream_failed,
                    "unexpected Migu HTTP status",
                    status == 429 || status >= 500,
                    {{"http_status", std::to_string(status)}}};
    return response.body;
}
Json Client::get(std::string_view op, std::string url, const ports::ProviderContext& c,
                 bool auth) const {
    auto doc = parse_json(fetch(op, std::move(url), c, auth));
    const auto code = doc.at("code").get<std::string>();
    if (code != "000000")
        throw Error{
            ErrorCode::upstream_failed,
            "Migu rejected the operation",
            false,
            {{"migu_code", code.size() <= 16 && code.find_first_not_of("0123456789") == code.npos
                               ? code
                               : "unknown"}}};
    return doc;
}
std::string Client::lyrics(std::string url, const ports::ProviderContext& c) const {
    auto body = fetch("lyrics", std::move(url), c, false);
    if (body.size() > 1024 * 1024 || body.find('\0') != body.npos ||
        body.find("<html") != body.npos || body.find("<!DOCTYPE") != body.npos)
        throw std::invalid_argument{"lyric body"};
    return body;
}
Json Client::resource(std::string_view op, const domain::PlatformRef& ref, std::string_view type,
                      const ports::ProviderContext& c) const {
    check_ref(ref, type == "2003");
    auto id = ref.id.value();
    if (type == "2003" && id.starts_with("d:")) {
        type = "5";
        id.erase(0, 2);
    }
    auto doc = get(op,
                   std::string{api} +
                       "/MIGUM2.0/v1.0/content/resourceinfo.do?needSimple=00&resourceType=" +
                       std::string{type} + "&resourceId=" + id,
                   c);
    const auto& values = doc.at("resource");
    array_shape(values, 1);
    if (values.empty())
        throw Error{ErrorCode::not_found, "Migu entity was not found", false, {}};
    const auto& item = values.front();
    const auto key = type == "2021" ? "musicListId" : type == "2003" ? "albumId" : "contentId";
    if (item.at(key).get<std::string>() != id || item.at("resourceType").get<std::string>() != type)
        throw std::invalid_argument{"resource identity"};
    return item;
}
} // namespace music_service::providers::migu
