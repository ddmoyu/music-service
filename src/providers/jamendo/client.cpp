#include "providers/jamendo/client.hpp"
#include "providers/jamendo/parsing.hpp"
#include "application/transport/url.hpp"
#include <mbedtls/sha1.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <random>
#include <set>

namespace music_service::providers::jamendo {
bool decimal(std::string_view value) {
    std::uint64_t number{};
    auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), number);
    return !value.empty() && value.size() <= 19 && value.front() != '0' &&
           ec == std::errc{} && end == value.data() + value.size();
}
void check_ref(const domain::PlatformRef& ref) {
    if (ref.source.value() != "jamendo" || !decimal(ref.id.value()))
        throw Error::validation("id", "invalid Jamendo entity identifier");
}
void check_search(std::string_view text) {
    if (text.empty() || text.size() > 200 || !domain::is_valid_utf8(text) ||
        text.find_first_not_of(" \r\n\t") == text.npos ||
        std::any_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c == 127; }))
        throw Error::validation("q", "must contain 1 to 200 UTF-8 bytes without control characters");
}
std::string digest(std::string_view value) {
    std::array<unsigned char, 20> bytes{};
    if (mbedtls_sha1(reinterpret_cast<const unsigned char*>(value.data()), value.size(), bytes.data()) != 0)
        throw Error{ErrorCode::internal, "Jamendo protocol hashing failed", false, {}};
    std::string result;
    for (auto byte : bytes) { result += "0123456789abcdef"[byte >> 4]; result += "0123456789abcdef"[byte & 15]; }
    return result;
}
std::string call_header(std::string_view path, std::string_view nonce) {
    return "$" + digest(std::string{path} + std::string{nonce}) + "*" + std::string{nonce} + "~";
}
std::uint32_t offset(const ports::ListQuery& query, bool collection) {
    if (query.limit == 0 || query.limit > (collection ? 100U : 50U))
        throw Error::validation("limit", collection ? "must be between 1 and 100" : "must be between 1 and 50");
    if (!query.cursor) return 0;
    const auto colon = query.cursor->find(':');
    auto number = std::string_view{*query.cursor}.substr(0, colon);
    std::uint32_t value{};
    const auto [end, ec] = std::from_chars(number.data(), number.data() + number.size(), value);
    if (!decimal(number) || ec != std::errc{} || end != number.data() + number.size() || value >= max_collection ||
        (collection ? colon == query.cursor->npos || query.cursor->size() - colon - 1 != 40 : colon != query.cursor->npos))
        throw Error{ErrorCode::cursor_mismatch, "invalid Jamendo page cursor", false, {}};
    if (collection) {
        const auto rev = std::string_view{*query.cursor}.substr(colon + 1);
        if (rev.find_first_not_of("0123456789abcdef") != rev.npos)
            throw Error{ErrorCode::cursor_mismatch, "invalid Jamendo revision", false, {}};
    }
    return value;
}
void same_revision(const ports::ListQuery& query, std::string_view revision) {
    if (query.cursor && std::string_view{*query.cursor}.substr(query.cursor->find(':') + 1) != revision)
        throw Error{ErrorCode::cursor_expired, "Jamendo collection changed; restart pagination", false, {}};
}
Json Client::get(std::string_view operation, std::string_view path, std::string query,
                 const ports::ProviderContext& context) const {
    std::random_device random;
    const auto nonce = "0." + std::to_string(1000000000ULL + static_cast<std::uint64_t>(random()));
    ports::HttpRequest request;
    request.url = "https://www.jamendo.com" + std::string{path} + "?" + query;
    request.operation = operation;
    request.max_response_bytes = 4 * 1024 * 1024;
    request.headers = {{"User-Agent", std::string{user_agent}}, {"Referer", "https://www.jamendo.com/"},
        {"Accept", "application/json"}, {"x-requested-with", "XMLHttpRequest"},
        {"x-jam-version", "4rkl5f"}, {"x-jam-call", call_header(path, nonce)}};
    ports::AdmissionRequest admission{context.identity.binding().principal_id(), setup_.source, request.operation,
        ports::TrafficClass::short_request, context.identity, {}, setup_.rate_limit_group, context.operation.deadline};
    auto result = take(setup_.upstream->fetch(std::move(request), admission, context.operation));
    if (auto pending = std::get_if<ports::PendingHop>(&result))
        throw Error{ErrorCode::service_busy, "Jamendo capacity is not available", true,
                    {{"admission", pending->deferred.blocked_dimension}}};
    auto response = std::get<ports::HttpResponse>(std::move(result));
    if (response.head.status != 200) {
        const auto status = response.head.status;
        Error error{status == 429 ? ErrorCode::upstream_rate_limited : ErrorCode::upstream_failed,
                    "unexpected Jamendo HTTP status", status == 429 || status >= 500,
                    {{"http_status", std::to_string(status)}}};
        if (auto wait = response.head.header("Retry-After"); wait && !wait->empty() && wait->size() <= 9 &&
            wait->find_first_not_of("0123456789") == wait->npos)
            error.details.push_back({"retry_after_seconds", *wait});
        throw error;
    }
    if (response.body.size() > 4 * 1024 * 1024) throw std::invalid_argument{"body limit"};
    bool deep = false;
    auto document = Json::parse(response.body, [&](int depth, Json::parse_event_t, Json&) {
        if (depth > 32) deep = true;
        return !deep;
    }, false);
    if (deep || !document.is_array() || document.size() > max_collection)
        throw std::invalid_argument{"response array"};
    return document;
}
Json Client::one(std::string_view operation, std::string_view kind, const domain::PlatformRef& ref,
                 const ports::ProviderContext& context) const {
    check_ref(ref);
    auto data = get(operation, "/api/" + std::string{kind}, "id=" + ref.id.value(), context);
    if (data.empty()) throw Error{ErrorCode::not_found, "Jamendo entity was not found", false, {}};
    if (data.size() != 1 || parsing::id(data.front()) != ref.id.value())
        throw std::invalid_argument{"entity identity"};
    if (!parsing::available(data.front())) throw Error{ErrorCode::not_found, "Jamendo entity is unavailable", false, {}};
    return std::move(data.front());
}
std::map<std::string, Json> Client::batch(std::string_view operation, std::string_view kind,
    const std::vector<std::string>& ids, const ports::ProviderContext& context) const {
    if (ids.size() > 100) throw std::invalid_argument{"batch limit"};
    std::set<std::string> requested(ids.begin(), ids.end());
    std::map<std::string, Json> result;
    if (requested.empty()) return result;
    std::string query;
    for (const auto& value : requested) {
        if (!decimal(value)) throw std::invalid_argument{"batch identity"};
        if (!query.empty()) query += '&';
        query += "id%5B%5D=" + value;
    }
    auto data = get(operation, "/api/" + std::string{kind}, std::move(query), context);
    if (data.size() > requested.size()) throw std::invalid_argument{"batch size"};
    for (auto& item : data) {
        auto value = parsing::id(item);
        if (!requested.contains(value) || !result.emplace(value, std::move(item)).second)
            throw std::invalid_argument{"batch identity"};
    }
    return result;
}
}
