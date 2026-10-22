#include "providers/kuwo/parsing/collections.hpp"

#include "providers/kuwo/parsing/parsers.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <set>
#include <utility>

namespace music_service::providers::kuwo::parsing {

namespace {

using json = nlohmann::json;
constexpr std::string_view source_name = "kuwo";

domain::Error protocol_error(std::string reason) {
    domain::Error error{domain::ErrorCode::upstream_failed, "the kuwo response could not be read", false, {}};
    error.details.push_back({"protocol", std::move(reason)});
    return error;
}

domain::Error not_found(std::string message) {
    return domain::Error{domain::ErrorCode::not_found, std::move(message), false, {}};
}

domain::Result<json> parse_json(std::string_view body, const char* what) {
    auto document = json::parse(body, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return protocol_error(std::string{what} + " is not a JSON object");
    }
    return document;
}

domain::Result<json> parse_legacy(std::string_view body, const char* what) {
    auto text = legacy_json(body);
    if (!text) {
        return std::move(text).error();
    }
    return parse_json(*text, what);
}

// A member that is a decimal string or a number, as text; empty otherwise.
std::string text_of(const json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end()) {
        return {};
    }
    if (it->is_string()) {
        return it->get<std::string>();
    }
    if (it->is_number_unsigned()) {
        return std::to_string(it->get<std::uint64_t>());
    }
    if (it->is_number_integer()) {
        return std::to_string(it->get<std::int64_t>());
    }
    return {};
}

std::optional<std::uint64_t> count_of(const json& object, const char* key) {
    const auto text = text_of(object, key);
    std::uint64_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || ec != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::string> nonempty(std::string text) {
    return text.empty() ? std::nullopt : std::optional<std::string>{std::move(text)};
}

std::optional<std::string> clean(const json& object, const char* key) { return nonempty(decode_text(text_of(object, key))); }

domain::PlatformRef ref_of(std::string_view id) {
    return {domain::SourceId::parse(source_name).value(), domain::PlatformId::parse(id).value()};
}

std::optional<domain::PlatformRef> optional_ref(std::string_view id) {
    if (!is_valid_collection_id(id)) {
        return std::nullopt;
    }
    return ref_of(id);
}

// The first non-empty image address of the keys, preferring the order given. Kuwo also
// answers bare paths such as "120/s3s94/93/211513640.jpg"; those are left out.
domain::Artwork artwork_of(const json& object, std::initializer_list<const char*> keys) {
    for (const auto* key : keys) {
        auto text = text_of(object, key);
        if (text.rfind("https://", 0) == 0 || text.rfind("http://", 0) == 0) {
            return text;
        }
    }
    return std::nullopt;
}

// YYYY, YYYY-MM or YYYY-MM-DD; Kuwo's "0000-00-00" placeholder is no date.
std::optional<std::string> date_of(const json& object, const char* key) {
    auto text = text_of(object, key);
    const auto digits = [&](std::size_t from, std::size_t count) {
        return text.size() >= from + count &&
               std::all_of(text.begin() + static_cast<std::ptrdiff_t>(from),
                           text.begin() + static_cast<std::ptrdiff_t>(from + count),
                           [](char c) { return c >= '0' && c <= '9'; });
    };
    const bool shaped = (text.size() == 4 && digits(0, 4)) ||
                        (text.size() == 7 && digits(0, 4) && text[4] == '-' && digits(5, 2)) ||
                        (text.size() == 10 && digits(0, 4) && text[4] == '-' && digits(5, 2) && text[7] == '-' &&
                         digits(8, 2));
    if (!shaped || text.rfind("0000", 0) == 0) {
        return std::nullopt;
    }
    return text;
}

// A song of a playlist, album or chart. Entries without a usable rid or name are left
// out (Kuwo lists removed songs that way); the duration key differs by endpoint.
std::optional<domain::Track> track_of(const json& item, const char* id_key, const char* duration_key) {
    if (!item.is_object()) {
        return std::nullopt;
    }
    auto rid = text_of(item, id_key);
    if (rid.rfind("MUSIC_", 0) == 0) {
        rid.erase(0, 6);
    }
    auto title = decode_text(text_of(item, "name"));
    if (!is_valid_rid(rid) || title.empty()) {
        return std::nullopt;
    }
    domain::Track track{ref_of(rid), std::move(title), parse_artists(text_of(item, "artist"), text_of(item, "artistid")),
                        std::nullopt, std::nullopt, {}};
    if (auto album = decode_text(text_of(item, "album")); !album.empty()) {
        auto album_id = text_of(item, "albumid");
        if (album_id.empty()) {
            album_id = text_of(item, "albumId");
        }
        track.album = domain::AlbumSummary{std::move(album), optional_ref(album_id)};
    }
    if (const auto seconds = count_of(item, duration_key); seconds && *seconds > 0) {
        track.duration_ms = static_cast<std::int64_t>(*seconds) * 1000;
    }
    return track;
}

std::vector<domain::Track> tracks_of(const json& document, const char* id_key, const char* duration_key) {
    std::vector<domain::Track> tracks;
    const auto list = document.find("musiclist");
    if (list == document.end() || !list->is_array()) {
        return tracks;
    }
    for (const auto& item : *list) {
        if (auto track = track_of(item, id_key, duration_key)) {
            tracks.push_back(std::move(*track));
        }
    }
    return tracks;
}

const json* array_at(const json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_array() ? &*it : nullptr;
}

void collect_charts(const json& node, std::vector<domain::Chart>& charts, std::set<std::string>& seen, int depth) {
    if (!node.is_object() || depth > 8) {
        return;
    }
    const auto* children = array_at(node, "child");
    const auto id = text_of(node, "sourceid");
    if ((children == nullptr || children->empty()) && is_valid_collection_id(id) && seen.insert(id).second) {
        domain::Chart chart{ref_of(id)};
        chart.name = decode_text(text_of(node, "disname"));
        if (chart.name.empty()) {
            chart.name = decode_text(text_of(node, "name"));
        }
        chart.description = clean(node, "intro");
        chart.updated_at = date_of(node, "pubTime");
        if (!chart.name.empty()) {
            charts.push_back(std::move(chart));
        }
    }
    if (children != nullptr) {
        for (const auto& child : *children) {
            collect_charts(child, charts, seen, depth + 1);
        }
    }
}

// wapi answers {"code":200,"data":...}; any other code is a refusal.
domain::Result<json> wapi_data(std::string_view body, const char* what) {
    auto document = parse_json(body, what);
    if (!document) {
        return std::move(document).error();
    }
    const auto code = text_of(*document, "code");
    if (code != "200") {
        domain::Error error{domain::ErrorCode::upstream_failed, "kuwo refused the request", false, {}};
        error.details.push_back({"kuwo_code", code.empty() ? std::string{"none"} : code});
        return error;
    }
    const auto data = document->find("data");
    if (data == document->end()) {
        return protocol_error(std::string{what} + " lacks data");
    }
    return *data;
}

domain::Playlist listed_playlist(const json& item, const std::string& id) {
    domain::Playlist playlist{ref_of(id)};
    playlist.name = decode_text(text_of(item, "name"));
    playlist.creator = clean(item, "uname");
    playlist.description = clean(item, "desc");
    playlist.artwork = artwork_of(item, {"img"});
    playlist.track_count = count_of(item, "total");
    playlist.play_count = count_of(item, "listencnt");
    return playlist;
}

} // namespace

bool is_valid_collection_id(std::string_view id) noexcept {
    return !id.empty() && id.size() <= 18 && id.front() != '0' &&
           std::all_of(id.begin(), id.end(), [](char c) { return c >= '0' && c <= '9'; });
}

domain::Result<std::string> legacy_json(std::string_view body) {
    std::string out;
    out.reserve(body.size() + body.size() / 16);
    for (std::size_t i = 0; i < body.size(); ++i) {
        const char c = body[i];
        if (c != '\'' && c != '"') {
            out.push_back(c);
            continue;
        }
        // A string in either quote style, rewritten with double quotes.
        const char quote = c;
        out.push_back('"');
        bool closed = false;
        for (++i; i < body.size(); ++i) {
            const char d = body[i];
            if (d == '\\') {
                if (i + 1 >= body.size()) {
                    break;
                }
                const char next = body[++i];
                if (next == '\'') {
                    out.push_back('\'');
                } else {
                    out.push_back('\\');
                    out.push_back(next);
                }
            } else if (d == quote) {
                closed = true;
                break;
            } else if (d == '"') {
                out += "\\\"";
            } else {
                out.push_back(d);
            }
        }
        if (!closed) {
            return protocol_error("legacy response has an unterminated string");
        }
        out.push_back('"');
    }
    return out;
}

domain::Result<ListPage<domain::Playlist>> parse_playlist_search(std::string_view body, std::uint32_t page) {
    auto document = parse_legacy(body, "playlist search response");
    if (!document) {
        return std::move(document).error();
    }
    const auto* list = array_at(*document, "abslist");
    const auto total = count_of(*document, "TOTAL");
    const auto pn = count_of(*document, "PN");
    if (list == nullptr || !total || !pn) {
        return protocol_error("playlist search response lacks abslist, TOTAL or PN");
    }
    if (*pn != page) {
        return protocol_error("search response is for another page");
    }
    ListPage<domain::Playlist> result;
    result.total = *total;
    for (const auto& item : *list) {
        if (!item.is_object()) {
            return protocol_error("playlist search item is not an object");
        }
        auto id = text_of(item, "playlistid");
        if (!is_valid_collection_id(id)) {
            continue; // another kind of hit
        }
        domain::Playlist playlist{ref_of(id)};
        playlist.name = decode_text(text_of(item, "name"));
        playlist.creator = clean(item, "nickname");
        playlist.description = clean(item, "intro");
        playlist.artwork = artwork_of(item, {"hts_pic", "pic"});
        playlist.track_count = count_of(item, "songnum");
        playlist.play_count = count_of(item, "playcnt");
        result.items.push_back(std::move(playlist));
    }
    return result;
}

domain::Result<ListPage<domain::Album>> parse_album_search(std::string_view body, std::uint32_t page) {
    auto document = parse_legacy(body, "album search response");
    if (!document) {
        return std::move(document).error();
    }
    const auto* list = array_at(*document, "albumlist");
    const auto total = count_of(*document, "total");
    const auto pn = count_of(*document, "pn");
    if (list == nullptr || !total || !pn) {
        return protocol_error("album search response lacks albumlist, total or pn");
    }
    if (*pn != page) {
        return protocol_error("search response is for another page");
    }
    ListPage<domain::Album> result;
    result.total = *total;
    for (const auto& item : *list) {
        if (!item.is_object()) {
            return protocol_error("album search item is not an object");
        }
        auto id = text_of(item, "albumid");
        if (!is_valid_collection_id(id)) {
            continue;
        }
        domain::Album album{ref_of(id)};
        album.title = decode_text(text_of(item, "name"));
        album.artists = parse_artists(text_of(item, "artist"), text_of(item, "artistid"));
        album.release_date = date_of(item, "pub");
        album.track_count = count_of(item, "musiccnt");
        album.artwork = artwork_of(item, {"hts_img", "img"});
        result.items.push_back(std::move(album));
    }
    return result;
}

domain::Result<PlaylistPage> parse_playlist_page(std::string_view body, std::string_view id) {
    auto document = parse_json(body, "playlist response");
    if (!document) {
        return std::move(document).error();
    }
    if (text_of(*document, "result") != "ok" || array_at(*document, "musiclist") == nullptr) {
        return protocol_error("playlist response lacks result or musiclist");
    }
    if (text_of(*document, "id") != id) {
        return not_found("kuwo has no such playlist");
    }
    PlaylistPage page{domain::Playlist{ref_of(id)}, tracks_of(*document, "id", "duration")};
    auto& playlist = page.playlist;
    playlist.name = decode_text(text_of(*document, "title"));
    playlist.creator = clean(*document, "uname");
    playlist.description = clean(*document, "info");
    playlist.artwork = artwork_of(*document, {"pic"});
    playlist.track_count = count_of(*document, "total");
    playlist.play_count = count_of(*document, "playnum");
    return page;
}

domain::Result<AlbumPage> parse_album_page(std::string_view body, std::string_view id) {
    auto document = parse_legacy(body, "album response");
    if (!document) {
        return std::move(document).error();
    }
    if (array_at(*document, "musiclist") == nullptr) {
        return protocol_error("album response lacks musiclist");
    }
    const auto album_id = text_of(*document, "albumid");
    if (album_id.empty()) {
        return not_found("kuwo has no such album");
    }
    if (album_id != id) {
        return protocol_error("album response is about another album");
    }
    AlbumPage page{domain::Album{ref_of(id)}, tracks_of(*document, "id", "duration")};
    auto& album = page.album;
    album.title = decode_text(text_of(*document, "name"));
    album.artists = parse_artists(text_of(*document, "artist"), text_of(*document, "artistid"));
    album.release_date = date_of(*document, "pub");
    album.track_count = count_of(*document, "songnum");
    album.artwork = artwork_of(*document, {"hts_img", "img"});
    return page;
}

domain::Result<std::vector<domain::Category>> parse_categories(std::string_view body) {
    auto data = wapi_data(body, "category response");
    if (!data) {
        return std::move(data).error();
    }
    if (!data->is_array()) {
        return protocol_error("category response data is not a list");
    }
    std::vector<domain::Category> categories;
    std::set<std::string> seen;
    for (const auto& group : *data) {
        const auto* items = group.is_object() ? array_at(group, "data") : nullptr;
        if (items == nullptr) {
            continue;
        }
        const auto group_name = clean(group, "name");
        for (const auto& item : *items) {
            // digest 10000 marks a playlist tag; the others (43) are zones that link to
            // other content and list no playlists.
            if (!item.is_object() || text_of(item, "digest") != "10000") {
                continue;
            }
            auto id = text_of(item, "id");
            auto name = decode_text(text_of(item, "name"));
            if (is_valid_collection_id(id) && !name.empty() && seen.insert(id).second) {
                categories.push_back({std::move(id), std::move(name), group_name, domain::ListedKind::playlist});
            }
        }
    }
    return categories;
}

domain::Result<ListPage<domain::Playlist>> parse_playlist_list(std::string_view body) {
    auto data = wapi_data(body, "playlist list response");
    if (!data) {
        return std::move(data).error();
    }
    const auto* list = data->is_object() ? array_at(*data, "data") : nullptr;
    const auto total = data->is_object() ? count_of(*data, "total") : std::nullopt;
    if (list == nullptr || !total) {
        return protocol_error("playlist list response lacks data or total");
    }
    ListPage<domain::Playlist> result;
    result.total = *total;
    for (const auto& item : *list) {
        if (!item.is_object()) {
            return protocol_error("playlist list item is not an object");
        }
        const auto id = text_of(item, "id");
        if (is_valid_collection_id(id)) {
            result.items.push_back(listed_playlist(item, id));
        }
    }
    return result;
}

domain::Result<std::vector<domain::Chart>> parse_charts(std::string_view body) {
    auto document = parse_json(body, "chart list response");
    if (!document) {
        return std::move(document).error();
    }
    if (array_at(*document, "child") == nullptr) {
        return protocol_error("chart list response lacks child");
    }
    std::vector<domain::Chart> charts;
    std::set<std::string> seen;
    collect_charts(*document, charts, seen, 0);
    return charts;
}

domain::Result<ChartPage> parse_chart_page(std::string_view body, std::string_view id) {
    auto document = parse_json(body, "chart response");
    if (!document) {
        return std::move(document).error();
    }
    if (document->empty()) {
        return not_found("kuwo has no such chart");
    }
    if (array_at(*document, "musiclist") == nullptr) {
        return protocol_error("chart response lacks musiclist");
    }
    ChartPage page{domain::Chart{ref_of(id)}, tracks_of(*document, "id", "song_duration")};
    auto& chart = page.chart;
    chart.name = decode_text(text_of(*document, "name"));
    chart.description = clean(*document, "info");
    chart.updated_at = date_of(*document, "pub");
    chart.track_count = count_of(*document, "num");
    return page;
}

} // namespace music_service::providers::kuwo::parsing
