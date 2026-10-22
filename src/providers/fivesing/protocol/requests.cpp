#include "providers/fivesing/protocol/requests.hpp"

#include "application/transport/url.hpp"

#include <utility>

namespace music_service::providers::fivesing::protocol {

namespace ports = application::ports;
using application::transport::encode_query_component;

namespace {

constexpr std::string_view search_host = "search.5sing.kugou.com";
constexpr std::string_view api_host = "mobileapi.5sing.kugou.com";

ports::HttpRequest get(std::string url, std::string_view operation) {
    ports::HttpRequest request;
    request.url = std::move(url);
    request.operation = std::string{operation};
    request.headers = {{"User-Agent", std::string{user_agent}}, {"Accept", "*/*"}};
    request.plaintext_endpoints = plaintext_endpoints();
    return request;
}

std::string work_query(std::string_view type, std::string_view id) {
    return "songid=" + std::string{id} + "&songtype=" + std::string{type};
}

} // namespace

std::vector<ports::PlaintextEndpoint> plaintext_endpoints() {
    return {
        {std::string{search_host}, 80, "/home/json", "search_tracks", false},
        {std::string{api_host}, 80, "/song/newget", "track_detail", false},
        {std::string{api_host}, 80, "/song/newget", "lyrics", false},
        {std::string{api_host}, 80, "/song/getSongUrl", "resolve_audio", false},
    };
}

ports::HttpRequest search_tracks(std::string_view keyword, std::uint32_t page) {
    // sort=1 is relevance, filter=0 keeps every work type, type=0 searches songs.
    std::string url = "http://" + std::string{search_host} + "/home/json?keyword=" + encode_query_component(keyword);
    url += "&sort=1&page=" + std::to_string(page) + "&filter=0&type=0";
    return get(std::move(url), "search_tracks");
}

ports::HttpRequest song_info(std::string_view type, std::string_view id, std::string_view operation) {
    return get("http://" + std::string{api_host} + "/song/newget?" + work_query(type, id), operation);
}

ports::HttpRequest song_urls(std::string_view type, std::string_view id) {
    return get("http://" + std::string{api_host} + "/song/getSongUrl?" + work_query(type, id), "resolve_audio");
}

ports::HttpRequest audio_probe(std::string url) {
    auto request = get(std::move(url), "resolve_audio");
    request.method = ports::HttpMethod::head;
    request.plaintext_endpoints.clear(); // audio addresses are HTTPS only
    return request;
}

} // namespace music_service::providers::fivesing::protocol
