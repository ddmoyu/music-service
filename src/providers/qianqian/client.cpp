#include "providers/qianqian/client.hpp"
#include "application/transport/url.hpp"
#include <mbedtls/md5.h>
#include <mbedtls/sha256.h>
#include <algorithm>
#include <array>

namespace music_service::providers::qianqian {
namespace {
template<std::size_t N> std::string hex(const std::array<unsigned char, N>& bytes) {
    std::string result;
    for (auto b : bytes) { result += "0123456789abcdef"[b >> 4]; result += "0123456789abcdef"[b & 15]; }
    return result;
}
}
bool identifier(std::string_view value, char prefix) {
    if (prefix) {
        if (value.empty() || value.front() != prefix) return false;
        value.remove_prefix(1);
    }
    return !value.empty() && value.size() <= 18 && value.front() != '0' &&
        std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; });
}
void check_ref(const domain::PlatformRef& ref, char prefix) {
    if (ref.source.value() != "qianqian" || !identifier(ref.id.value(), prefix))
        throw Error::validation("id", "invalid Qianqian entity identifier");
}
std::string signed_query(Params params, std::int64_t unix_seconds) {
    if (unix_seconds < 0 || unix_seconds > 253402300799LL)
        throw Error{ErrorCode::internal, "invalid Qianqian signing clock", false, {}};
    params.erase("sign");
    params["appid"] = "16073360";
    params["timestamp"] = std::to_string(unix_seconds);
    std::string canonical;
    for (const auto& [key, value] : params) {
        if (!canonical.empty()) canonical += '&';
        canonical += key + "=" + value;
    }
    // 官方网页公开的协议常量，不是用户凭证；先签原始 UTF-8，再编码查询参数。
    canonical += "0b50b02fd0d73a9c4c8c3a781c30845f";
    std::array<unsigned char, 16> hash{};
    if (mbedtls_md5(reinterpret_cast<const unsigned char*>(canonical.data()), canonical.size(), hash.data()) != 0)
        throw Error{ErrorCode::internal, "Qianqian signing failed", false, {}};
    params["sign"] = hex(hash);
    std::string result;
    for (const auto& [key, value] : params) {
        if (!result.empty()) result += '&';
        result += application::transport::encode_query_component(key) + "=" + application::transport::encode_query_component(value);
    }
    return result;
}
std::string digest(std::string_view value) {
    std::array<unsigned char, 32> bytes{};
    if (mbedtls_sha256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), bytes.data(), 0) != 0)
        throw Error{ErrorCode::internal, "Qianqian cursor hashing failed", false, {}};
    return hex(bytes);
}
Json parse_response(std::string_view body) {
    if (body.size() > max_body) throw std::invalid_argument{"body limit"};
    bool deep = false;
    auto doc = Json::parse(body, [&](int depth, Json::parse_event_t, Json&) {
        if (depth > 32) deep = true;
        return !deep;
    }, false);
    if (deep || !doc.is_object() || !doc.at("errno").is_number_integer() || !doc.at("state").is_boolean())
        throw std::invalid_argument{"response envelope"};
    const auto code = doc.at("errno").get<std::int64_t>();
    if (code != 22000 || !doc.at("state").get<bool>()) {
        // 23001 也表示不支持的搜索类型，不能一律误报账号失效。
        throw Error{code == 23010 ? ErrorCode::not_found : ErrorCode::upstream_failed,
            code == 22001 ? "Qianqian rejected the request signature" : "Qianqian rejected the operation",
            false, {{"qianqian_code", std::to_string(code)}}};
    }
    return doc.at("data");
}
std::string Client::fetch(ports::HttpRequest request, const ports::ProviderContext& context) const {
    if (context.operation.stop.stop_requested()) throw Error{ErrorCode::canceled, "Qianqian operation canceled", false, {}};
    if (std::chrono::steady_clock::now() >= context.operation.deadline)
        throw Error{ErrorCode::deadline_exceeded, "Qianqian operation deadline exceeded", false, {}};
    request.headers = {{"User-Agent", std::string{user_agent}}, {"Referer", "https://music.91q.com/player"},
        {"Accept", "*/*"}, {"Accept-Encoding", "gzip"}};
    request.preserve_content_encoding = true;
    const auto limit = request.max_response_bytes;
    ports::AdmissionRequest admission{context.identity.binding().principal_id(), setup_.source,
        request.operation, ports::TrafficClass::short_request, context.identity, {}, setup_.rate_limit_group, context.operation.deadline};
    auto reply = take(setup_.upstream->fetch(std::move(request), admission, context.operation));
    if (auto pending = std::get_if<ports::PendingHop>(&reply))
        throw Error{ErrorCode::service_busy, "Qianqian capacity is not available", true, {{"admission", pending->deferred.blocked_dimension}}};
    auto response = std::get<ports::HttpResponse>(std::move(reply));
    const auto status = response.head.status;
    if (status != 200) throw Error{status == 429 ? ErrorCode::upstream_rate_limited :
        status == 401 || status == 403 ? ErrorCode::credentials_required : ErrorCode::upstream_failed,
        "unexpected Qianqian HTTP status", status == 429 || status >= 500, {{"http_status", std::to_string(status)}}};
    return decode_body(std::move(response.body), response.head.header("Content-Encoding").value_or(""), limit, context.operation);
}
Json Client::get(std::string_view operation, std::string_view endpoint, Params params, const ports::ProviderContext& context) const {
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    ports::HttpRequest request;
    request.url = "https://music.91q.com/v1/" + std::string{endpoint} + "?" + signed_query(std::move(params), seconds);
    request.operation = operation;
    request.max_response_bytes = max_body;
    return parse_response(fetch(std::move(request), context));
}
std::string Client::lyrics(std::string url, const ports::ProviderContext& context) const {
    ports::HttpRequest request;
    request.url = std::move(url);
    request.operation = "lyrics";
    request.max_response_bytes = 256 * 1024;
    auto body = fetch(std::move(request), context);
    if (body.size() > 256 * 1024 || !domain::is_valid_utf8(body) || body.find('\0') != body.npos)
        throw std::invalid_argument{"lyric encoding or limit"};
    if (body.starts_with("\xEF\xBB\xBF")) body.erase(0, 3);
    const auto first = body.find_first_not_of(" \t\r\n");
    if (first == body.npos) return {};
    // 官方也提供无时间戳的 TXT 歌词；只拒绝错误文档，不伪造 LRC 时间。
    auto folded = body;
    std::transform(folded.begin(), folded.end(), folded.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    if (folded.find("<html") != folded.npos || folded.find("<!doctype") != folded.npos ||
        folded.find("<?xml") != folded.npos || folded.find("<error") != folded.npos)
        throw std::invalid_argument{"HTML or XML lyrics"};
    if ((body[first] == '{' || body[first] == '[') && Json::accept(body))
        throw std::invalid_argument{"JSON lyrics"};
    return body;
}
}
