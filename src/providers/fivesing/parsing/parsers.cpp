#include "providers/fivesing/parsing/parsers.hpp"

#include "application/transport/url.hpp"
#include "domain/catalog/refs.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <utility>

namespace music_service::providers::fivesing::parsing {

namespace {

using json = nlohmann::json;
constexpr std::string_view source_name = "fivesing";
constexpr std::array work_types{std::string_view{"yc"}, std::string_view{"fc"}, std::string_view{"bz"}};

domain::Error protocol_error(std::string reason) {
    domain::Error error{domain::ErrorCode::upstream_failed, "the 5sing response could not be read", false, {}};
    error.details.push_back({"protocol", std::move(reason)});
    return error;
}

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

void append_utf8(std::string& out, std::uint32_t code) {
    if (code < 0x80) {
        out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}

std::optional<std::uint32_t> entity_code(std::string_view entity) {
    if (entity == "amp") {
        return '&';
    }
    if (entity == "lt") {
        return '<';
    }
    if (entity == "gt") {
        return '>';
    }
    if (entity == "quot") {
        return '"';
    }
    if (entity == "apos" || entity == "#39") {
        return '\'';
    }
    if (entity == "nbsp") {
        return ' ';
    }
    if (entity.size() > 1 && entity.front() == '#') {
        const bool hex = entity[1] == 'x' || entity[1] == 'X';
        const auto digits = entity.substr(hex ? 2 : 1);
        std::uint32_t value = 0;
        const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value, hex ? 16 : 10);
        if (ec == std::errc{} && end == digits.data() + digits.size() && value > 0 && value <= 0x10FFFF &&
            (value < 0xD800 || value > 0xDFFF)) {
            return value;
        }
    }
    return std::nullopt;
}

// A string or integer member as an unsigned integer.
std::optional<std::uint64_t> as_uint(const json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end()) {
        return std::nullopt;
    }
    if (it->is_number_unsigned()) {
        return it->get<std::uint64_t>();
    }
    if (it->is_number_integer()) {
        const auto value = it->get<std::int64_t>();
        return value >= 0 ? std::optional<std::uint64_t>{static_cast<std::uint64_t>(value)} : std::nullopt;
    }
    if (it->is_string()) {
        const auto& text = it->get_ref<const std::string&>();
        std::uint64_t parsed = 0;
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (!text.empty() && ec == std::errc{} && end == text.data() + text.size()) {
            return parsed;
        }
    }
    return std::nullopt;
}

std::string as_string(const json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

bool is_work_type(std::string_view type) {
    return std::find(work_types.begin(), work_types.end(), type) != work_types.end();
}

bool is_work_number(std::string_view id) {
    return !id.empty() && id.size() <= 12 && id.front() != '0' &&
           std::all_of(id.begin(), id.end(), [](char c) { return c >= '0' && c <= '9'; });
}

domain::TrackRef ref_of(const WorkKey& key) {
    return {domain::SourceId::parse(source_name).value(), domain::PlatformId::parse(key.platform_id()).value()};
}

// The work type says what the recording is; covers are never the original recording.
domain::VersionInfo version_of(std::string_view type) {
    domain::VersionInfo version;
    version.evidence.push_back("5sing work type " + std::string{type});
    if (type == "fc") {
        version.kind = domain::VersionKind::cover;
        version.label = "翻唱";
    } else if (type == "bz") {
        version.kind = domain::VersionKind::instrumental;
        version.label = "伴奏";
    } else {
        version.label = "原创"; // an original work; whether studio or live is not stated
    }
    return version;
}

// A song/newget or getSongUrl answer that is not a success: the missing-work codes are
// NOT_FOUND, anything else an upstream failure carrying the code.
domain::Error refusal(const json& document, std::initializer_list<std::int64_t> missing_codes) {
    const auto code = as_uint(document, "code");
    if (code && *code == 13) { // "操作太频繁": too many requests
        domain::Error error{domain::ErrorCode::upstream_rate_limited, "5sing is limiting requests", true, {}};
        error.details.push_back({"fivesing_code", "13"});
        return error;
    }
    if (code && std::find(missing_codes.begin(), missing_codes.end(), static_cast<std::int64_t>(*code)) !=
                    missing_codes.end()) {
        return domain::Error{domain::ErrorCode::not_found, "5sing has no such work", false, {}};
    }
    domain::Error error{domain::ErrorCode::upstream_failed, "5sing refused the request", false, {}};
    error.details.push_back({"fivesing_code", code ? std::to_string(*code) : std::string{"none"}});
    return error;
}

// 保留上游的定时歌词或纯文本；公共 TrackLyrics 明确允许无时间戳文本。
std::optional<std::string> lyric_text(std::string_view text) {
    std::string out;
    while (!text.empty()) {
        const auto end = text.find('\n');
        auto line = text.substr(0, end);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        out.append(line);
        out.push_back('\n');
        if (end == std::string_view::npos) {
            break;
        }
        text.remove_prefix(end + 1);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) {
        out.pop_back();
    }
    if (out.find_first_not_of(" \t\r\n") == std::string::npos) return std::nullopt;
    return out;
}

} // namespace

std::string clean_text(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    bool in_tag = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (in_tag) {
            in_tag = c != '>';
            continue;
        }
        if (c == '<') {
            in_tag = true;
            continue;
        }
        if (c != '&') {
            out.push_back(c);
            continue;
        }
        const auto semicolon = text.find(';', i);
        const auto code = semicolon != std::string_view::npos && semicolon - i <= 10
                              ? entity_code(text.substr(i + 1, semicolon - i - 1))
                              : std::nullopt;
        if (!code) {
            out.push_back('&');
            continue;
        }
        append_utf8(out, *code);
        i = semicolon;
    }
    return std::string{trim(out)};
}

domain::Result<WorkKey> parse_work_key(std::string_view platform_id) {
    const auto dash = platform_id.find('-');
    if (dash == std::string_view::npos || !is_work_type(platform_id.substr(0, dash)) ||
        !is_work_number(platform_id.substr(dash + 1))) {
        return domain::Error::validation("track", "is not a 5sing work id (<yc|fc|bz>-<number>)");
    }
    return WorkKey{std::string{platform_id.substr(0, dash)}, std::string{platform_id.substr(dash + 1)}};
}

domain::Result<SearchPage> parse_search(std::string_view body) {
    auto document = json::parse(body, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return protocol_error("search response is not a JSON object");
    }
    const auto info = document.find("pageInfo");
    if (info == document.end() || !info->is_object()) {
        return protocol_error("search response lacks pageInfo");
    }
    const auto total = as_uint(*info, "totalCount");
    const auto page = as_uint(*info, "cur");
    const auto pages = as_uint(*info, "totalPages");
    const auto list = document.find("list");
    // An empty result has "list": null.
    if (!total || !page || !pages || *page > 100000 || *pages > 100000000 || list == document.end() ||
        !(list->is_array() || list->is_null())) {
        return protocol_error("search response lacks list, totalCount, cur or totalPages");
    }
    SearchPage result;
    result.total = *total;
    result.page = static_cast<std::uint32_t>(*page);
    result.total_pages = static_cast<std::uint32_t>(*pages);
    if (list->is_null()) {
        return result;
    }
    for (const auto& item : *list) {
        if (!item.is_object()) {
            return protocol_error("search item is not an object");
        }
        const auto number = as_uint(item, "songId");
        const auto type = as_string(item, "typeEname");
        auto title = clean_text(as_string(item, "songName"));
        if (!number || title.empty()) {
            return protocol_error("search item lacks songId or songName");
        }
        WorkKey key{type, std::to_string(*number)};
        if (!is_work_type(type) || !is_work_number(key.id)) {
            continue; // another kind of entry (not a song work)
        }
        domain::Track track{ref_of(key), std::move(title), {}, std::nullopt, std::nullopt, version_of(type)};
        // The uploader performs the work; the original singer of a cover is not an artist
        // of this recording. Uploaders are 5sing users, not artist pages: no ref.
        if (auto singer = clean_text(as_string(item, "singer")); !singer.empty()) {
            track.artists.push_back({std::move(singer), std::nullopt});
        }
        // No duration: songSize is a file size and the bit rate is unknown here.
        result.tracks.push_back(std::move(track));
    }
    return result;
}

domain::Result<SongInfo> parse_song_info(std::string_view body, const WorkKey& key) {
    auto document = json::parse(body, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return protocol_error("song response is not a JSON object");
    }
    const auto success = document.find("success");
    if (success == document.end() || !success->is_boolean()) {
        return protocol_error("song response lacks success");
    }
    if (!success->get<bool>()) {
        return refusal(document, {200062});
    }
    const auto data = document.find("data");
    if (data == document.end() || !data->is_object()) {
        return protocol_error("song response lacks data");
    }
    const auto number = as_uint(*data, "ID");
    if (!number || std::to_string(*number) != key.id || as_string(*data, "SK") != key.type) {
        return protocol_error("song response is about another work");
    }
    SongInfo info{domain::Track{ref_of(key), clean_text(as_string(*data, "SN")), {}, std::nullopt, std::nullopt,
                                version_of(key.type)},
                  {}};
    if (const auto user = data->find("user"); user != data->end() && user->is_object()) {
        if (auto name = clean_text(as_string(*user, "NN")); !name.empty()) {
            info.track.artists.push_back({std::move(name), std::nullopt});
        }
    }
    if (auto album = clean_text(as_string(*data, "albumName")); !album.empty()) {
        info.track.album = domain::AlbumSummary{std::move(album), std::nullopt};
    }
    if (auto lrc = lyric_text(as_string(*data, "dynamicWords"))) {
        info.lyrics.lrc = std::move(lrc);
    } else {
        info.lyrics.unavailable = true;
    }
    return info;
}

domain::Result<std::vector<AudioTier>> parse_song_urls(std::string_view body, const WorkKey& key) {
    auto document = json::parse(body, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return protocol_error("audio response is not a JSON object");
    }
    const auto code = as_uint(document, "code");
    if (!code) {
        return protocol_error("audio response lacks code");
    }
    if (*code != 1000) {
        return refusal(document, {28});
    }
    const auto data = document.find("data");
    if (data == document.end() || !data->is_object()) {
        return protocol_error("audio response lacks data");
    }
    const auto number = as_uint(*data, "songid");
    if (!number || std::to_string(*number) != key.id || as_string(*data, "songtype") != key.type) {
        return protocol_error("audio response is about another work");
    }
    std::vector<AudioTier> tiers;
    for (const std::string_view tier_name : {"sq", "hq", "lq"}) {
        const std::string name{tier_name};
        AudioTier tier;
        tier.name = name;
        tier.primary = as_string(*data, (name + "url").c_str());
        tier.backup = as_string(*data, (name + "url_backup").c_str());
        tier.md5 = as_string(*data, (name + "urlmd5").c_str());
        tier.ext = as_string(*data, (name + "ext").c_str());
        if (const auto size = as_uint(*data, (name + "size").c_str()); size && *size > 0) {
            tier.size = size;
        }
        if (!tier.primary.empty() || !tier.backup.empty()) {
            tiers.push_back(std::move(tier));
        }
    }
    return tiers;
}

bool is_playlist_id(std::string_view id) noexcept {
    return !id.empty() && id.size() <= 32 && std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    });
}

domain::Result<PlaylistSearchPage> parse_playlist_search(std::string_view body) {
    auto document = json::parse(body, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return protocol_error("playlist search response is not a JSON object");
    }
    const auto info = document.find("pageInfo");
    const auto list = document.find("list");
    if (info == document.end() || !info->is_object() || list == document.end() ||
        !(list->is_array() || list->is_null())) {
        return protocol_error("playlist search response lacks list or pageInfo");
    }
    const auto total = as_uint(*info, "totalCount");
    const auto page = as_uint(*info, "cur");
    const auto pages = as_uint(*info, "totalPages");
    if (!total || !page || !pages || *page > 100000 || *pages > 100000000) {
        return protocol_error("playlist search response lacks totalCount, cur or totalPages");
    }
    PlaylistSearchPage result;
    result.total = *total;
    result.page = static_cast<std::uint32_t>(*page);
    result.total_pages = static_cast<std::uint32_t>(*pages);
    if (list->is_null()) {
        return result;
    }
    for (const auto& item : *list) {
        if (!item.is_object()) {
            return protocol_error("playlist search item is not an object");
        }
        const auto id = as_string(item, "songListId");
        if (!is_playlist_id(id)) {
            continue;
        }
        domain::Playlist playlist{domain::PlatformRef{domain::SourceId::parse(source_name).value(),
                                                      domain::PlatformId::parse(id).value()}};
        playlist.name = clean_text(as_string(item, "title"));
        if (auto creator = clean_text(as_string(item, "userName")); !creator.empty()) {
            playlist.creator = std::move(creator);
        }
        // "0" stands for no description.
        if (auto content = clean_text(as_string(item, "content")); !content.empty() && content != "0") {
            playlist.description = std::move(content);
        }
        if (auto picture = as_string(item, "pictureUrl"); picture.rfind("http", 0) == 0) {
            playlist.artwork = std::move(picture);
        }
        playlist.track_count = as_uint(item, "songCnt");
        playlist.play_count = as_uint(item, "playCount");
        result.playlists.push_back(std::move(playlist));
    }
    return result;
}

domain::Result<domain::Playlist> parse_playlist_info(std::string_view body, std::string_view id) {
    auto document = json::parse(body, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return protocol_error("playlist response is not a JSON object");
    }
    const auto success = document.find("success");
    if (success == document.end() || !success->is_boolean()) {
        return protocol_error("playlist response lacks success");
    }
    if (!success->get<bool>()) {
        auto error = refusal(document, {24});
        if (error.code == domain::ErrorCode::not_found) {
            error.message = "5sing has no such playlist";
        }
        return error;
    }
    const auto data = document.find("data");
    if (data == document.end() || !data->is_object()) {
        return protocol_error("playlist response lacks data");
    }
    if (as_string(*data, "ID") != id) {
        return protocol_error("playlist response is about another playlist");
    }
    domain::Playlist playlist{domain::PlatformRef{domain::SourceId::parse(source_name).value(),
                                                  domain::PlatformId::parse(id).value()}};
    playlist.name = clean_text(as_string(*data, "T"));
    // "暂无简介" is the placeholder for no description.
    if (auto content = clean_text(as_string(*data, "C")); !content.empty() && content != "暂无简介") {
        playlist.description = std::move(content);
    }
    if (auto picture = as_string(*data, "P"); picture.rfind("http", 0) == 0) {
        playlist.artwork = std::move(picture);
    }
    playlist.track_count = as_uint(*data, "E");
    playlist.play_count = as_uint(*data, "H");
    if (const auto user = data->find("user"); user != data->end() && user->is_object()) {
        if (auto name = clean_text(as_string(*user, "NN")); !name.empty()) {
            playlist.creator = std::move(name);
        }
    }
    return playlist;
}

domain::Result<std::vector<domain::Track>> parse_playlist_songs(std::string_view body) {
    auto document = json::parse(body, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return protocol_error("playlist songs response is not a JSON object");
    }
    const auto success = document.find("success");
    if (success == document.end() || !success->is_boolean()) {
        return protocol_error("playlist songs response lacks success");
    }
    if (!success->get<bool>()) {
        auto error = refusal(document, {0, 24});
        if (error.code == domain::ErrorCode::not_found) {
            error.message = "5sing has no such playlist";
        }
        return error;
    }
    const auto data = document.find("data");
    if (data == document.end() || !data->is_array()) {
        return protocol_error("playlist songs response lacks data");
    }
    std::vector<domain::Track> tracks;
    for (const auto& item : *data) {
        if (!item.is_object()) {
            return protocol_error("playlist song is not an object");
        }
        const auto number = as_uint(item, "ID");
        WorkKey key{as_string(item, "SK"), number ? std::to_string(*number) : std::string{}};
        auto title = clean_text(as_string(item, "SN"));
        if (!is_work_type(key.type) || !is_work_number(key.id) || title.empty()) {
            continue;
        }
        domain::Track track{ref_of(key), std::move(title), {}, std::nullopt, std::nullopt, version_of(key.type)};
        if (const auto user = item.find("user"); user != item.end() && user->is_object()) {
            if (auto name = clean_text(as_string(*user, "NN")); !name.empty()) {
                track.artists.push_back({std::move(name), std::nullopt});
            }
        }
        tracks.push_back(std::move(track));
    }
    return tracks;
}

domain::Result<WorkKey> parse_track_link(std::string_view link) {
    auto url = application::transport::Url::parse(link);
    if (!url || url->host() != "5sing.kugou.com") {
        return domain::Error::validation("link", "is not a 5sing song link");
    }
    // /{type}/{number}.html
    auto path = url->path();
    if (path.size() < 2 || path.front() != '/') {
        return domain::Error::validation("link", "is not a 5sing song link");
    }
    path.remove_prefix(1);
    const auto slash = path.find('/');
    constexpr std::string_view suffix = ".html";
    if (slash == std::string_view::npos || path.size() < slash + 1 + suffix.size() ||
        path.substr(path.size() - suffix.size()) != suffix) {
        return domain::Error::validation("link", "is not a 5sing song link");
    }
    const auto type = path.substr(0, slash);
    const auto number = path.substr(slash + 1, path.size() - slash - 1 - suffix.size());
    if (!is_work_type(type) || !is_work_number(number)) {
        return domain::Error::validation("link", "does not name a 5sing work");
    }
    return WorkKey{std::string{type}, std::string{number}};
}

domain::Result<Link> parse_link(std::string_view link) {
    if (auto song = parse_track_link(link)) return Link{"track", song->platform_id()};
    auto url = application::transport::Url::parse(link);
    if (!url || url->host() != "5sing.kugou.com" || url->port() != (url->is_https() ? 443 : 80))
        return domain::Error::validation("link", "is not a supported 5sing link");
    // 平台搜索返回 /{userId}/dj/{songListId}.html；用户 ID 不是歌单 ID。
    const auto path = url->path();
    const auto marker = path.find("/dj/");
    if (path.starts_with('/') && path.ends_with(".html") && marker != std::string_view::npos && marker > 1 &&
        is_work_number(path.substr(1, marker - 1))) {
        const auto id = path.substr(marker + 4, path.size() - marker - 4 - 5);
        if (is_playlist_id(id)) return Link{"playlist", std::string{id}};
    }
    return domain::Error::validation("link", "is not a supported 5sing playlist link");
}

} // namespace music_service::providers::fivesing::parsing
