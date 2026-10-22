#include "providers/netease/protocol/requests.hpp"

#include "application/transport/url.hpp"
#include "providers/netease/protocol/weapi.hpp"

#include <nlohmann/json.hpp>

#include <utility>

namespace music_service::providers::netease::protocol {

namespace ports = application::ports;
using application::transport::encode_query_component;
// Keys keep their insertion order, as the web client sends them.
using json = nlohmann::ordered_json;

namespace {

ports::HttpRequest weapi(std::string_view path, json payload, std::string_view operation, const Call& call) {
    payload["csrf_token"] = call.csrf;
    ports::HttpRequest request;
    request.method = ports::HttpMethod::post;
    request.url = "https://music.163.com/weapi/" + std::string{path} + "?csrf_token=" + encode_query_component(call.csrf);
    request.operation = std::string{operation};
    request.headers = {{"User-Agent", std::string{user_agent}},
                       {"Referer", "https://music.163.com/"},
                       {"Origin", "https://music.163.com"},
                       {"Content-Type", "application/x-www-form-urlencoded"},
                       {"Accept", "*/*"}};
    request.body = weapi_body(weapi_encrypt(payload.dump(), call.secret_key));
    return request;
}

// Splits "a=1; b=2" into pairs; whitespace around names and values is dropped.
template <class Visit> void each_pair(std::string_view cookie, Visit&& visit) {
    while (!cookie.empty()) {
        const auto end = cookie.find(';');
        auto pair = cookie.substr(0, end);
        cookie = end == std::string_view::npos ? std::string_view{} : cookie.substr(end + 1);
        const auto eq = pair.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }
        auto trim = [](std::string_view s) {
            while (!s.empty() && s.front() == ' ') {
                s.remove_prefix(1);
            }
            while (!s.empty() && s.back() == ' ') {
                s.remove_suffix(1);
            }
            return s;
        };
        visit(trim(pair.substr(0, eq)), trim(pair.substr(eq + 1)));
    }
}

std::optional<std::string> cookie_value(std::string_view cookie, std::string_view name) {
    std::optional<std::string> found;
    each_pair(cookie, [&](std::string_view key, std::string_view value) {
        if (key == name && !value.empty()) {
            found = std::string{value};
        }
    });
    return found;
}

// Song ids are sent as JSON numbers, as the web client does. Callers pass ids already
// checked to be 1..19 decimal digits.
std::uint64_t numeric(std::string_view id) {
    std::uint64_t value = 0;
    for (const char c : id) {
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return value;
}

} // namespace

ports::HttpRequest search_tracks(std::string_view keyword, std::uint32_t offset, std::uint32_t limit,
                                 const Call& call) {
    return weapi("cloudsearch/pc",
                 json{{"s", keyword}, {"type", 1}, {"limit", limit}, {"offset", offset}, {"total", true}},
                 "search_tracks", call);
}

ports::HttpRequest song_detail(std::string_view id, const Call& call) {
    const auto list = json::array({json{{"id", numeric(id)}}});
    return weapi("v3/song/detail", json{{"c", list.dump()}}, "track_detail", call);
}

ports::HttpRequest song_url(std::string_view id, std::string_view level, const Call& call) {
    return weapi("song/enhance/player/url/v1",
                 json{{"ids", "[" + std::string{id} + "]"}, {"level", level}, {"encodeType", "flac"}},
                 "resolve_audio", call);
}

ports::HttpRequest lyric(std::string_view id, const Call& call) {
    return weapi("song/lyric", json{{"id", numeric(id)}, {"lv", -1}, {"kv", -1}, {"tv", -1}}, "lyrics", call);
}

ports::HttpRequest account(const Call& call) {
    return weapi("w/nuser/account/get", json::object(), "verify_credentials", call);
}

void with_cookie(ports::HttpRequest& request, std::string_view cookie) {
    std::string clean;
    each_pair(cookie, [&](std::string_view key, std::string_view value) {
        if (key.empty()) {
            return;
        }
        if (!clean.empty()) {
            clean += "; ";
        }
        clean.append(key).append("=").append(value);
    });
    if (!clean.empty()) {
        request.secret_headers.push_back({"Cookie", std::move(clean)});
    }
}

std::optional<std::string> csrf_of(std::string_view cookie) {
    return cookie_value(cookie, "__csrf");
}

std::optional<std::string> music_u_of(std::string_view cookie) {
    return cookie_value(cookie, "MUSIC_U");
}

} // namespace music_service::providers::netease::protocol
