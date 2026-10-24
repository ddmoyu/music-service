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

ports::HttpRequest song_details(const std::vector<std::string>& ids, const Call& call) {
    auto list = json::array();
    for (const auto& id : ids) list.push_back({{"id", numeric(id)}});
    auto request = weapi("v3/song/detail", {{"c", list.dump()}}, "playlist_tracks", call);
    request.max_response_bytes = 2 * 1024 * 1024;
    return request;
}

ports::HttpRequest search_collections(bool playlists, std::string_view keyword, std::uint32_t offset,
                                      std::uint32_t limit, const Call& call) {
    auto request = weapi("cloudsearch/pc",
        {{"s", keyword}, {"type", playlists ? 1000 : 10}, {"limit", limit}, {"offset", offset}, {"total", true}},
        playlists ? "search_playlists" : "search_albums", call);
    request.max_response_bytes = 2 * 1024 * 1024;
    return request;
}

ports::HttpRequest playlist(std::string_view id, const Call& call) {
    // n=0：只请求曲目 ID 清单，当前页详情由调用方另取。
    auto request = weapi("v3/playlist/detail", {{"id", numeric(id)}, {"n", 0}, {"s", 0}}, "playlist_detail", call);
    request.max_response_bytes = 4 * 1024 * 1024;
    return request;
}

ports::HttpRequest album(std::string_view id, const Call& call) {
    // 上游没有专辑曲目分页参数，响应及解析数量均设上限。
    auto request = weapi("v1/album/" + std::string{id}, json::object(), "album_detail", call);
    request.max_response_bytes = 8 * 1024 * 1024;
    return request;
}

ports::HttpRequest lyric(std::string_view id, const Call& call) {
    auto request = weapi("song/lyric", json{{"id", numeric(id)}, {"lv", -1}, {"kv", -1}, {"tv", -1},
        {"rv", -1}, {"yv", -1}}, "lyrics", call);
    request.max_response_bytes = 2 * 1024 * 1024;
    return request;
}

ports::HttpRequest user_playlists(std::string_view uid, std::uint32_t offset, std::uint32_t limit, const Call& call) {
    auto r = weapi("user/playlist", {{"uid", numeric(uid)}, {"offset", offset}, {"limit", limit},
        {"includeVideo", false}}, "user_playlists", call);
    r.max_response_bytes = 2 * 1024 * 1024;
    return r;
}

ports::HttpRequest artist_tracks(std::string_view id, const Call& call) {
    auto r = weapi("artist/" + std::string{id}, json::object(), "artist_tracks", call);
    r.max_response_bytes = 2 * 1024 * 1024;
    return r;
}

ports::HttpRequest search_artists(std::string_view keyword, std::uint32_t offset, std::uint32_t limit, const Call& call) {
    auto r = weapi("cloudsearch/pc", {{"s", keyword}, {"type", 100}, {"offset", offset}, {"limit", limit},
        {"total", true}}, "search_artists", call);
    r.max_response_bytes = 2 * 1024 * 1024;
    return r;
}

ports::HttpRequest artist_albums(std::string_view id, std::uint32_t offset, std::uint32_t limit, const Call& call) {
    auto r = weapi("artist/albums/" + std::string{id}, {{"offset", offset}, {"limit", limit}, {"total", true}},
        "artist_albums", call);
    r.max_response_bytes = 2 * 1024 * 1024;
    return r;
}

ports::HttpRequest podcast(std::string_view id, const Call& call) {
    auto r = weapi("djradio/v2/get", {{"id", numeric(id)}}, "podcast_detail", call);
    r.max_response_bytes = 512 * 1024;
    return r;
}

ports::HttpRequest episodes(std::string_view id, std::uint32_t offset, std::uint32_t limit, const Call& call) {
    auto r = weapi("dj/program/byradio", {{"radioId", numeric(id)}, {"offset", offset}, {"limit", limit},
        {"asc", true}}, "podcast_episodes", call);
    r.max_response_bytes = 4 * 1024 * 1024;
    return r;
}

ports::HttpRequest episode(std::string_view id, const Call& call) {
    auto r = weapi("dj/program/detail", {{"id", numeric(id)}}, "episode_detail", call);
    r.max_response_bytes = 512 * 1024;
    return r;
}

ports::HttpRequest categories(const Call& call) {
    auto request = weapi("playlist/catalogue", json::object(), "categories", call);
    request.max_response_bytes = 512 * 1024;
    return request;
}

ports::HttpRequest category_playlists(std::string_view category, std::uint32_t offset,
                                     std::uint32_t limit, const Call& call) {
    auto request = weapi("playlist/list", {{"cat", category}, {"order", "hot"}, {"limit", limit},
        {"offset", offset}, {"total", true}}, "category_playlists", call);
    request.max_response_bytes = 2 * 1024 * 1024;
    return request;
}

ports::HttpRequest recommendations(const Call& call) {
    // Public fixed-size candidate set, not the account's personalized feed.
    auto request = weapi("personalized/playlist", {{"limit", 30}, {"total", true}, {"n", 1000}},
                         "recommend_playlists", call);
    request.max_response_bytes = 2 * 1024 * 1024;
    return request;
}

ports::HttpRequest charts(const Call& call) {
    auto request = weapi("toplist", json::object(), "charts", call);
    request.max_response_bytes = 2 * 1024 * 1024;
    return request;
}

ports::HttpRequest account(const Call& call) {
    return weapi("w/nuser/account/get", json::object(), "verify_credentials", call);
}

ports::HttpRequest membership_info(const Call& call) {
    auto request = weapi("music-vip-membership/front/vip/info", {{"userId", ""}}, "verify_credentials", call);
    request.max_response_bytes = 256 * 1024;
    request.allow_redirects = false;
    return request;
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
