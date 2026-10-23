#include "providers/qq/client.hpp"
#include <mbedtls/sha256.h>
#include <algorithm>
#include <array>

namespace music_service::providers::qq {
bool mid(std::string_view v) {
    return v.size() == 14 && std::all_of(v.begin(), v.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    });
}
bool decimal(std::string_view v) {
    std::uint64_t n{};
    auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
    return !v.empty() && v.size() <= 19 && v.front() != '0' && ec == std::errc{} && end == v.data() + v.size();
}
std::string cookie_value(std::string_view cookie, std::string_view name) {
    while (!cookie.empty()) {
        auto pos = cookie.find(';');
        auto part = cookie.substr(0, pos);
        cookie = pos == cookie.npos ? std::string_view{} : cookie.substr(pos + 1);
        while (!part.empty() && part.front() == ' ') part.remove_prefix(1);
        while (!part.empty() && part.back() == ' ') part.remove_suffix(1);
        if (part.starts_with(std::string{name} + "=")) return std::string{part.substr(name.size() + 1)};
    }
    return {};
}
std::string digest(std::string_view v) {
    std::array<unsigned char, 32> bytes{};
    if (mbedtls_sha256(reinterpret_cast<const unsigned char*>(v.data()), v.size(), bytes.data(), 0) != 0)
        throw Error{ErrorCode::internal, "QQ cursor hashing failed", false, {}};
    std::string out;
    for (auto b : bytes) { out += "0123456789abcdef"[b >> 4]; out += "0123456789abcdef"[b & 15]; }
    return out;
}
void check_ref(const domain::PlatformRef& ref, bool numeric) {
    if (ref.source.value() != "qq" || !(numeric ? decimal(ref.id.value()) : mid(ref.id.value())))
        throw Error::validation("id", "invalid QQ entity identifier");
}
std::uint32_t offset(std::optional<std::string> cursor, std::uint32_t limit) {
    if (limit == 0 || limit > 50) throw Error::validation("limit", "must be between 1 and 50");
    if (!cursor) return 0;
    auto text = std::string_view{*cursor}.substr(0, cursor->find(':'));
    std::uint32_t n{};
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), n);
    if (text.empty() || ec != std::errc{} || end != text.data() + text.size() || n > 10000)
        throw Error{ErrorCode::cursor_mismatch, "invalid QQ page offset", false, {}};
    return n;
}
std::string revision(std::optional<std::string> cursor) {
    if (!cursor) return {};
    auto at = cursor->find(':');
    if (at == cursor->npos || cursor->size() - at - 1 != 64)
        throw Error{ErrorCode::cursor_mismatch, "invalid QQ collection cursor", false, {}};
    return cursor->substr(at + 1);
}
void api_code(const Json& object) {
    const auto code = object.at("code").get<std::int64_t>();
    if (code == 0) return;
    if (code == 1000)
        throw Error{ErrorCode::credentials_required, "QQ login is required or expired", false, {}};
    throw Error{ErrorCode::upstream_failed, "QQ rejected the operation", false, {{"qq_code", std::to_string(code)}}};
}
Json parse_response(std::string_view body) {
    if (body.size() > 4 * 1024 * 1024) throw std::invalid_argument{"body limit"};
    bool deep = false;
    auto doc = Json::parse(body, [&](int depth, Json::parse_event_t, Json&) {
        if (depth > 32) deep = true;
        return !deep;
    }, false);
    if (deep || !doc.is_object()) throw std::invalid_argument{"JSON"};
    api_code(doc);
    return doc;
}
Json Client::fetch(ports::HttpRequest request, const ports::ProviderContext& context, bool auth) const {
    request.headers = {{"User-Agent", std::string{user_agent}}, {"Referer", "https://y.qq.com/"},
                       {"Content-Type", "application/json"}, {"Accept", "application/json"}};
    request.max_response_bytes = 4 * 1024 * 1024;
    if (auth && context.secret) request.secret_headers.push_back({"Cookie", std::string{context.secret->reveal()}});
    ports::AdmissionRequest admission{context.identity.binding().principal_id(), setup_.source,
        request.operation, ports::TrafficClass::short_request, context.identity, {},
        setup_.rate_limit_group, context.operation.deadline};
    auto reply = take(setup_.upstream->fetch(std::move(request), admission, context.operation));
    if (auto pending = std::get_if<ports::PendingHop>(&reply))
        throw Error{ErrorCode::service_busy, "QQ capacity is not available", true,
                    {{"admission", pending->deferred.blocked_dimension}}};
    auto response = std::get<ports::HttpResponse>(std::move(reply));
    if (response.head.status != 200) {
        const auto status = response.head.status;
        throw Error{status == 429 ? ErrorCode::upstream_rate_limited : ErrorCode::upstream_failed,
                    "unexpected QQ HTTP status", status == 429 || status >= 500,
                    {{"http_status", std::to_string(status)}}};
    }
    return parse_response(response.body);
}
Json Client::rpc(std::string_view op, std::string_view module, std::string_view method, Json param,
                 const ports::ProviderContext& c, bool auth) const {
    auto uin = auth && c.secret ? cookie_value(c.secret->reveal(), "uin") : "0";
    if (!decimal(uin)) uin = "0";
    // 会话密钥只进入 secret_headers；不把 authst 放进可重定向的正文或 URL。
    Json body{{"comm", {{"ct", 24}, {"cv", 0}, {"format", "json"}, {"uin", uin}}},
              {"req_0", {{"module", module}, {"method", method}, {"param", std::move(param)}}}};
    ports::HttpRequest request;
    request.method = ports::HttpMethod::post;
    request.url = "https://u.y.qq.com/cgi-bin/musicu.fcg";
    request.operation = op;
    request.body = body.dump();
    auto result = fetch(std::move(request), c, auth).at("req_0");
    api_code(result);
    auto data = result.at("data");
    if (!data.is_object()) throw std::invalid_argument{"data"};
    if (data.contains("code")) api_code(data);
    return data;
}
Json Client::get(std::string_view op, std::string path, const ports::ProviderContext& c, bool auth) const {
    ports::HttpRequest request;
    request.url = "https://c.y.qq.com" + path;
    request.operation = op;
    return fetch(std::move(request), c, auth);
}
}
