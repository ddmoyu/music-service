#include "providers/kugou/client.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <mbedtls/sha256.h>

namespace music_service::providers::kugou {
bool decimal(std::string_view v, bool zero) {
    std::uint64_t n{};
    auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
    return !v.empty() && v.size() <= 19 && (v.front() != '0' || (zero && v == "0")) &&
           ec == std::errc{} && end == v.data() + v.size();
}
bool hash(std::string_view v) {
    return v.size() == 32 && v != "00000000000000000000000000000000" &&
           std::all_of(v.begin(), v.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
           });
}
std::string lower(std::string v) {
    for (auto& c : v)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + ('a' - 'A'));
    return v;
}
std::string digest(std::string_view v) {
    std::array<unsigned char, 32> bytes{};
    if (mbedtls_sha256(reinterpret_cast<const unsigned char*>(v.data()), v.size(), bytes.data(),
                       0) != 0)
        throw Error{ErrorCode::internal, "Kugou cursor hashing failed", false, {}};
    std::string out;
    for (auto b : bytes) {
        out += "0123456789abcdef"[b >> 4];
        out += "0123456789abcdef"[b & 15];
    }
    return out;
}
void check_ref(const domain::PlatformRef& ref, bool track) {
    if (ref.source.value() != "kugou" ||
        !(track ? hash(ref.id.value()) && lower(ref.id.value()) == ref.id.value()
                : decimal(ref.id.value())))
        throw Error::validation("id", "invalid Kugou identifier; track hashes must be lowercase");
}
std::uint32_t offset(const std::optional<std::string>& cursor, std::uint32_t limit,
                     bool collection) {
    if (!limit || limit > 50)
        throw Error::validation("limit", "must be between 1 and 50");
    if (!cursor)
        return 0;
    auto sep = cursor->find(':');
    if ((collection && (sep == cursor->npos || cursor->size() - sep - 1 != 64)) ||
        (!collection && sep != cursor->npos))
        throw Error{ErrorCode::cursor_mismatch, "invalid Kugou cursor", false, {}};
    auto value = std::string_view{*cursor}.substr(0, sep);
    std::uint32_t n{};
    auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), n);
    if (value.empty() || ec != std::errc{} || end != value.data() + value.size() || n > 10000 ||
        n % limit)
        throw Error{ErrorCode::cursor_mismatch, "invalid Kugou page offset", false, {}};
    return n;
}
void same_revision(const std::optional<std::string>& cursor, std::string_view rev) {
    if (cursor && std::string_view{*cursor}.substr(cursor->find(':') + 1) != rev)
        throw Error{
            ErrorCode::cursor_expired, "Kugou collection changed; restart pagination", false, {}};
}
std::vector<ports::PlaintextEndpoint> plaintext_endpoints() {
    // 2026-10-23：这两个主机的 HTTPS 证书不匹配；仅登记匿名目录的精确路径。
    return {
        {"mobilecdn.kugou.com", 80, "/api/v3/search/special", "search_playlists", false},
        {"mobilecdn.kugou.com", 80, "/api/v3/search/album", "search_albums", false},
        {"mobilecdn.kugou.com", 80, "/api/v3/special/info", "playlist_detail", false},
        {"mobilecdn.kugou.com", 80, "/api/v3/special/info", "playlist_tracks", false},
        {"mobilecdn.kugou.com", 80, "/api/v3/special/song", "playlist_tracks", false},
        {"mobilecdn.kugou.com", 80, "/api/v3/album/info", "album_detail", false},
        {"mobilecdn.kugou.com", 80, "/api/v3/album/info", "album_tracks", false},
        {"mobilecdn.kugou.com", 80, "/api/v3/album/info", "track_detail", false},
        {"mobilecdn.kugou.com", 80, "/api/v3/album/song", "album_tracks", false},
        {"mobilecdn.kugou.com", 80, "/api/v3/rank/list", "charts", false},
        {"mobilecdn.kugou.com", 80, "/api/v3/rank/list", "chart_detail", false},
        {"mobilecdn.kugou.com", 80, "/api/v3/rank/list", "chart_tracks", false},
        {"mobilecdn.kugou.com", 80, "/api/v3/rank/song", "chart_tracks", false},
        {"mobilecdnbj.kugou.com", 80, "/api/v3/tag/list", "categories", false},
        {"mobilecdnbj.kugou.com", 80, "/api/v3/tag/specialList", "category_playlists", false},
        {"mobilecdn.kugou.com", 80, "/api/v3/singer/info", "artist_detail", false},
        {"mobilecdn.kugou.com", 80, "/api/v3/singer/info", "artist_tracks", false},
        {"mobilecdn.kugou.com", 80, "/api/v3/singer/info", "artist_albums", false},
        {"mobilecdn.kugou.com", 80, "/api/v3/singer/song", "artist_tracks", false},
        {"mobilecdn.kugou.com", 80, "/api/v3/singer/album", "artist_albums", false},
    };
}
Json parse_response(std::string_view body) {
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
void api_success(const Json& doc) {
    const auto status = doc.at("status").get<std::int64_t>();
    const auto code = doc.contains("errcode") ? doc.at("errcode").get<std::int64_t>()
                                              : doc.at("error_code").get<std::int64_t>();
    if (status == 1 && code == 0)
        return;
    if (code == 1002)
        throw Error{ErrorCode::upstream_rate_limited, "Kugou request rate limited", true, {}};
    throw Error{ErrorCode::upstream_failed,
                "Kugou rejected the operation",
                false,
                {{"kugou_code", std::to_string(code)}}};
}
Json Client::get(std::string_view op, std::string url,
                 const ports::ProviderContext& context) const {
    ports::HttpRequest request;
    request.url = std::move(url);
    request.operation = op;
    request.headers = {{"User-Agent", std::string{user_agent}},
                       {"Referer", "https://m.kugou.com/"},
                       {"Accept", "application/json"}};
    request.max_response_bytes = 4 * 1024 * 1024;
    request.plaintext_endpoints = plaintext_endpoints();
    ports::AdmissionRequest admission{context.identity.binding().principal_id(),
                                      setup_.source,
                                      request.operation,
                                      ports::TrafficClass::short_request,
                                      context.identity,
                                      {},
                                      setup_.rate_limit_group,
                                      context.operation.deadline};
    auto reply = take(setup_.upstream->fetch(std::move(request), admission, context.operation));
    if (auto pending = std::get_if<ports::PendingHop>(&reply))
        throw Error{ErrorCode::service_busy,
                    "Kugou capacity is not available",
                    true,
                    {{"admission", pending->deferred.blocked_dimension}}};
    auto response = std::get<ports::HttpResponse>(std::move(reply));
    const auto status = response.head.status;
    if (status != 200)
        throw Error{status == 429 ? ErrorCode::upstream_rate_limited : ErrorCode::upstream_failed,
                    "unexpected Kugou HTTP status",
                    status == 429 || status >= 500,
                    {{"http_status", std::to_string(status)}}};
    return parse_response(response.body);
}
Json Client::api(std::string_view op, std::string path, const ports::ProviderContext& c) const {
    auto doc = get(op, std::string{mobile_api} + path, c);
    api_success(doc);
    if (doc.at("data").is_null() || (doc.at("data").is_array() && doc.at("data").empty()))
        throw Error{ErrorCode::not_found, "Kugou entity was not found", false, {}};
    if (!doc.at("data").is_object())
        throw std::invalid_argument{"data"};
    return doc.at("data");
}
} // namespace music_service::providers::kugou
