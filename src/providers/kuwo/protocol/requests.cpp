#include "providers/kuwo/protocol/requests.hpp"

#include "application/transport/url.hpp"

#include <utility>
#include <mbedtls/base64.h>

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

namespace {
ports::HttpRequest web_json(std::string path, std::string_view operation) {
    auto request = get("https://wapi.kuwo.cn/api/www/" + std::move(path) + "&httpsStatus=1&plat=web_www", operation);
    request.headers.push_back({"Referer", "https://www.kuwo.cn/"});
    return request;
}
std::string web_page(std::uint32_t page, std::uint32_t size) {
    return "&pn=" + std::to_string(page + 1) + "&rn=" + std::to_string(size);
}
}

ports::HttpRequest track_info(std::string_view rid) {
    return web_json("music/musicInfo?mid=" + std::string{rid}, "track_detail");
}
ports::HttpRequest artist_search(std::string_view keyword, std::uint32_t page, std::uint32_t size) {
    return web_json("search/searchArtistBykeyWord?key=" + encode_query_component(keyword) + web_page(page, size),
                    "search_artists");
}
ports::HttpRequest artist_detail(std::string_view id) {
    return web_json("artist/artist?artistid=" + std::string{id}, "artist_detail");
}
ports::HttpRequest artist_tracks(std::string_view id, std::uint32_t page, std::uint32_t size) {
    return web_json("artist/artistMusic?artistid=" + std::string{id} + web_page(page, size), "artist_tracks");
}
ports::HttpRequest artist_albums(std::string_view id, std::uint32_t page, std::uint32_t size) {
    return web_json("artist/artistAlbum?artistid=" + std::string{id} + web_page(page, size), "artist_albums");
}

ports::HttpRequest audio(std::string_view rid, std::string_view client_id, std::string_view bitrate) {
    std::string url = "https://mobi.kuwo.cn/mobi.s?f=web&source=kwplayercar_ar_6.0.0.9_B_jiakong_vh.apk&from=PC"
                      "&type=convert_url_with_sign&br=" + std::string{bitrate};
    url += "&rid=" + std::string{rid};
    url += "&user=" + encode_query_component(client_id);
    return get(std::move(url), "resolve_audio");
}

ports::HttpRequest lyrics(std::string_view rid) {
    std::string raw = "user=12345,web,web,web&requester=localhost&req=1&rid=MUSIC_" + std::string{rid} + "&lrcx=1";
    constexpr std::string_view key = "yeelion";
    for (std::size_t i = 0; i < raw.size(); ++i) raw[i] ^= key[i % key.size()];
    std::string encoded(4 * ((raw.size() + 2) / 3) + 1, '\0');
    std::size_t size{};
    mbedtls_base64_encode(reinterpret_cast<unsigned char*>(encoded.data()), encoded.size(), &size,
        reinterpret_cast<const unsigned char*>(raw.data()), raw.size());
    encoded.resize(size);
    auto r = get("https://newlyric.kuwo.cn/newlyric.lrc?" + encoded, "lyrics");
    r.max_response_bytes = 512 * 1024;
    return r;
}

namespace {

std::string pages(std::uint32_t page, std::uint32_t page_size) {
    return "&pn=" + std::to_string(page) + "&rn=" + std::to_string(page_size);
}

// The anonymous web client identity wapi.kuwo.cn expects.
constexpr std::string_view wapi_client = "loginUid=0&loginSid=0&appUid=38668888";

} // namespace

ports::HttpRequest search_collections(std::string_view kind, std::string_view keyword, std::uint32_t page,
                                      std::uint32_t page_size, std::string_view operation) {
    std::string url = "https://search.kuwo.cn/r.s?all=" + encode_query_component(keyword) + "&ft=" + std::string{kind};
    url += "&itemset=web_2013&client=kt&pcmp4=1&geo=c&vipver=1" + pages(page, page_size);
    url += "&rformat=json&encoding=utf8";
    return get(std::move(url), operation);
}

ports::HttpRequest playlist_page(std::string_view id, std::uint32_t page, std::uint32_t page_size,
                                 std::string_view operation) {
    std::string url = "https://nplserver.kuwo.cn/pl.svc?op=getlistinfo&pid=" + std::string{id} + pages(page, page_size);
    url += "&encode=utf8&keyset=pl2012&identity=kuwo&pcmp4=1&vipver=1&newver=1";
    return get(std::move(url), operation);
}

ports::HttpRequest album_page(std::string_view id, std::uint32_t page, std::uint32_t page_size,
                              std::string_view operation) {
    std::string url = "https://search.kuwo.cn/r.s?stype=albuminfo&albumid=" + std::string{id} + pages(page, page_size);
    url += "&sortby=0&alflac=1&show_copyright_off=1&pcmp4=1&encoding=utf8";
    return get(std::move(url), operation);
}

ports::HttpRequest playlist_tags() {
    return get("https://wapi.kuwo.cn/api/pc/classify/playlist/getTagList?cmd=rcm_keyword_playlist&user=0"
               "&prod=kwplayer_pc_9.1.1.2&vipver=9.1.1.2&source=kwplayer_pc_9.1.1.2&" +
                   std::string{wapi_client},
               "categories");
}

ports::HttpRequest tag_playlists(std::string_view tag_id, std::uint32_t page, std::uint32_t page_size) {
    return get("https://wapi.kuwo.cn/api/pc/classify/playlist/getTagPlayList?" + std::string{wapi_client} +
                   pages(page, page_size) + "&id=" + std::string{tag_id},
               "category_playlists");
}

ports::HttpRequest recommended_playlists(std::uint32_t page, std::uint32_t page_size) {
    return get("https://wapi.kuwo.cn/api/pc/classify/playlist/getRcmPlayList?" + std::string{wapi_client} +
                   pages(page, page_size) + "&order=hot",
               "recommend_playlists");
}

ports::HttpRequest chart_list() {
    return get("https://wapi.kuwo.cn/api/pc/bang/list?" + std::string{wapi_client}, "charts");
}

ports::HttpRequest chart_page(std::string_view id, std::uint32_t page, std::uint32_t page_size,
                              std::string_view operation) {
    std::string url = "https://kbangserver.kuwo.cn/ksong.s?from=pc&fmt=json" + pages(page, page_size);
    url += "&type=bang&data=content&id=" + std::string{id} + "&show_copyright_off=0&pcmp4=1&isbang=1&encode=utf8";
    return get(std::move(url), operation);
}

} // namespace music_service::providers::kuwo::protocol
