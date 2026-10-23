#include "providers/bilibili/internal.hpp"

#include <algorithm>
#include <charconv>
#include <limits>

namespace music_service::providers::bilibili {
bool valid_bvid(std::string_view value) {
    constexpr std::string_view alphabet =
        "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    return value.size() == 12 && value.starts_with("BV1") &&
           std::all_of(value.begin() + 3, value.end(),
                       [&](char c) { return alphabet.find(c) != alphabet.npos; });
}
std::uint64_t decimal(std::string_view value, std::uint64_t maximum) {
    std::uint64_t result{};
    auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (value.empty() || value.size() > 19 || value.front() == '0' || ec != std::errc{} ||
        end != value.data() + value.size() || result > maximum)
        throw std::invalid_argument{"identifier"};
    return result;
}
std::int64_t integer(const Json& value, std::int64_t minimum, std::int64_t maximum) {
    if (!value.is_number_integer() ||
        (value.is_number_unsigned() &&
         value.get<std::uint64_t>() > static_cast<std::uint64_t>(maximum)))
        throw std::invalid_argument{"integer"};
    const auto n = value.get<std::int64_t>();
    if (n < minimum || n > maximum)
        throw std::invalid_argument{"integer range"};
    return n;
}
std::string text(const Json& object, const char* key, bool required) {
    if (!required && (!object.contains(key) || object.at(key).is_null()))
        return {};
    auto value = object.at(key).get<std::string>();
    if (value.size() > 8192 || (required && value.empty()) || !domain::is_valid_utf8(value) ||
        std::any_of(value.begin(), value.end(),
                    [](char c) { return static_cast<unsigned char>(c) < 0x20; }))
        throw std::invalid_argument{"text"};
    return value;
}
std::string clean_title(std::string title) {
    std::string result;
    bool tag = false;
    for (const char c : title) {
        if (c == '<')
            tag = true;
        else if (c == '>')
            tag = false;
        else if (!tag)
            result += c;
    }
    for (const auto& [encoded, decoded] : {std::pair{"&quot;", "\""},
                                           {"&#39;", "'"},
                                           {"&apos;", "'"},
                                           {"&lt;", "<"},
                                           {"&gt;", ">"},
                                           {"&nbsp;", " "},
                                           {"&amp;", "&"}}) {
        for (std::size_t at = 0; (at = result.find(encoded, at)) != result.npos;
             at += std::string_view{decoded}.size())
            result.replace(at, std::string_view{encoded}.size(), decoded);
    }
    if (tag || result.empty())
        throw std::invalid_argument{"title"};
    return result;
}
std::optional<std::string> artwork(const Json& object) {
    auto value = text(object, "pic", false);
    if (value.empty())
        return {};
    if (value.starts_with("//"))
        value = "https:" + value;
    if (value.starts_with("http://"))
        value.replace(0, 7, "https://");
    auto url = application::transport::Url::parse(value);
    if (!url || !url->is_https() || url->port() != 443 || !url->host().ends_with(".hdslb.com"))
        throw std::invalid_argument{"artwork origin"};
    return url->str();
}
domain::TrackRef make_ref(std::string id) {
    return {take(domain::SourceId::parse("bilibili")), take(domain::PlatformId::parse(id))};
}
VideoId parse_id(const domain::TrackRef& ref) {
    try {
        const auto& value = ref.id.value();
        const auto pos = value.find(':');
        auto bvid = value.substr(0, pos);
        if (ref.source.value() != "bilibili" || !valid_bvid(bvid))
            throw std::invalid_argument{"bvid"};
        VideoId result{bvid, 1, {}};
        if (pos != value.npos) {
            const auto suffix = std::string_view{value}.substr(pos + 1);
            if (suffix.starts_with("cid"))
                result.cid = decimal(suffix.substr(3), INT64_MAX);
            else if (suffix.starts_with('p'))
                result.page = static_cast<std::uint32_t>(decimal(suffix.substr(1), max_parts));
            else
                throw std::invalid_argument{"part"};
        }
        return result;
    } catch (const std::invalid_argument&) {
        throw Error::validation("id", "expected a Bilibili BV id, BV id:pN or BV id:cidN");
    }
}
std::string cookie_value(std::string_view cookie, std::string_view key) {
    while (!cookie.empty()) {
        auto at = cookie.find(';');
        auto part = cookie.substr(0, at);
        cookie = at == cookie.npos ? std::string_view{} : cookie.substr(at + 1);
        while (!part.empty() && part.front() == ' ')
            part.remove_prefix(1);
        while (!part.empty() && part.back() == ' ')
            part.remove_suffix(1);
        if (part.starts_with(std::string{key} + "="))
            return std::string{part.substr(key.size() + 1)};
    }
    return {};
}
Json Client::get(std::string_view operation, std::string path,
                 const ports::ProviderContext& context, bool allow_logged_out) const {
    if (context.operation.stop.stop_requested())
        throw Error{ErrorCode::canceled, "Bilibili request canceled", false, {}};
    if (context.operation.expired(std::chrono::steady_clock::now()))
        throw Error{ErrorCode::deadline_exceeded, "Bilibili request deadline exceeded", true, {}};
    auto request_secret = context.secret;
    if (path.starts_with("/x/web-interface/search/type?") &&
        (!request_secret || cookie_value(request_secret->reveal(), "buvid3").empty())) {
        // 官方匿名浏览器会话，按操作取得，不写入凭证库或跨调用方缓存。
        const auto fingerprint = get(operation, "/x/frontend/finger/spi", context).at("data");
        const auto buvid3 = text(fingerprint, "b_3");
        const auto buvid4 = text(fingerprint, "b_4", false);
        const auto safe_value = [](const std::string& value) {
            return value.size() <= 128 && std::all_of(value.begin(), value.end(), [](char c) {
                       return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '+' ||
                              c == '/' || c == '=';
                   });
        };
        if (!safe_value(buvid3) || !safe_value(buvid4))
            throw std::invalid_argument{"anonymous session"};
        std::string cookie = request_secret ? std::string{request_secret->reveal()} : "";
        if (!cookie.empty())
            cookie += "; ";
        cookie += "buvid3=" + buvid3;
        if (!buvid4.empty() && cookie_value(cookie, "buvid4").empty())
            cookie += "; buvid4=" + buvid4;
        request_secret = std::make_shared<ports::SecretBytes>(std::move(cookie));
    }
    ports::HttpRequest request;
    request.url = "https://api.bilibili.com" + path;
    request.operation = operation;
    request.headers = {{"User-Agent", std::string{user_agent}},
                       {"Referer", "https://www.bilibili.com/"},
                       {"Accept", "application/json"}};
    request.max_response_bytes = 4 * 1024 * 1024;
    if (request_secret)
        request.secret_headers.push_back({"Cookie", std::string{request_secret->reveal()}});
    ports::AdmissionRequest admission{context.identity.binding().principal_id(),
                                      setup_.source,
                                      request.operation,
                                      ports::TrafficClass::short_request,
                                      context.identity,
                                      {},
                                      setup_.rate_limit_group,
                                      context.operation.deadline};
    auto reply = take(setup_.upstream->fetch(std::move(request), admission, context.operation));
    if (const auto pending = std::get_if<ports::PendingHop>(&reply))
        throw Error{ErrorCode::service_busy,
                    "Bilibili capacity is not available",
                    true,
                    {{"admission", pending->deferred.blocked_dimension}}};
    const auto response = std::get<ports::HttpResponse>(std::move(reply));
    if (response.head.status != 200) {
        const auto status = response.head.status;
        const bool limited = status == 429 || status == 412;
        throw Error{limited ? ErrorCode::upstream_rate_limited : ErrorCode::upstream_failed,
                    "unexpected Bilibili HTTP status",
                    limited || status >= 500,
                    {{"http_status", std::to_string(status)}}};
    }
    if (response.body.size() > 4 * 1024 * 1024)
        throw std::invalid_argument{"response size"};
    bool deep = false;
    auto doc = Json::parse(
        response.body,
        [&](int depth, Json::parse_event_t, Json&) {
            if (depth > 32)
                deep = true;
            return !deep;
        },
        false);
    if (deep || !doc.is_object())
        throw std::invalid_argument{"JSON"};
    const auto code = integer(doc.at("code"), INT32_MIN, INT32_MAX);
    if (code == 0 || (allow_logged_out && code == -101))
        return doc;
    auto category = ErrorCode::upstream_failed;
    if (code == -101 || code == -403 || code == -10403)
        category = ErrorCode::credentials_required;
    if (code == -404 || code == 62002 || code == 62004)
        category = ErrorCode::not_found;
    if (code == -412 || code == -352 || code == -509)
        category = ErrorCode::upstream_rate_limited;
    throw Error{category,
                "Bilibili rejected the operation",
                category == ErrorCode::upstream_rate_limited,
                {{"bilibili_code", std::to_string(code)}}};
}
} // namespace music_service::providers::bilibili
