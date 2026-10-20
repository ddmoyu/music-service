#include "providers/kuwo/protocol/requests.hpp"

#include "application/transport/url.hpp"

#include <utility>

namespace music_service::providers::kuwo::protocol {

namespace ports = application::ports;
using application::transport::encode_query_component;

namespace {

ports::HttpRequest get(std::string url, std::string_view operation) {
    ports::HttpRequest request;
    request.url = std::move(url);
    request.operation = std::string{operation};
    request.headers = {{"User-Agent", std::string{user_agent}}, {"Accept", "*/*"}};
    return request;
}

} // namespace

ports::HttpRequest search_tracks(std::string_view keyword, std::uint32_t page, std::uint32_t page_size) {
    // The fixed parameters are what the web client sends; pn/rn page and all is the query.
    std::string url = "https://www.kuwo.cn/search/searchMusicBykeyWord?vipver=1&client=kt&ft=music&cluster=0"
                      "&strategy=2012&encoding=utf8&rformat=json&mobi=1&issubtitle=1&show_copyright_off=1";
    url += "&pn=" + std::to_string(page);
    url += "&rn=" + std::to_string(page_size);
    url += "&all=" + encode_query_component(keyword);
    return get(std::move(url), "search_tracks");
}

ports::HttpRequest track_page(std::string_view rid) {
    return get("https://www.kuwo.cn/play_detail/" + std::string{rid}, "track_detail");
}

ports::HttpRequest standard_audio(std::string_view rid, std::string_view client_id) {
    std::string url = "https://mobi.kuwo.cn/mobi.s?f=web&source=kwplayercar_ar_6.0.0.9_B_jiakong_vh.apk&from=PC"
                      "&type=convert_url_with_sign&br=128kmp3";
    url += "&rid=" + std::string{rid};
    url += "&user=" + encode_query_component(client_id);
    return get(std::move(url), "resolve_audio");
}

} // namespace music_service::providers::kuwo::protocol
