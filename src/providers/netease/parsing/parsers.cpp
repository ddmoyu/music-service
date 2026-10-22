#include "providers/netease/parsing/parsers.hpp"

#include "application/transport/url.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <format>
#include <set>
#include <utility>

namespace music_service::providers::netease::parsing {

namespace {

using domain::Error;
using domain::ErrorCode;
using json = nlohmann::json;

constexpr std::string_view source_name = "netease";

Error protocol_error(std::string reason) {
    Error error{ErrorCode::upstream_failed, "the netease response could not be read", false, {}};
    error.details.push_back({"protocol", std::move(reason)});
    return error;
}

domain::Result<json> parse_body(std::string_view body) {
    auto value = json::parse(body.begin(), body.end(), nullptr, false);
    if (value.is_discarded() || !value.is_object()) {
        return protocol_error("body is not a JSON object");
    }
    // Every weapi answer carries a top-level code; 200 is success.
    const auto code = value.find("code");
    if (code == value.end() || !code->is_number_integer()) {
        return protocol_error("body has no code");
    }
    const auto n = code->get<std::int64_t>();
    if (n == 200) {
        return value;
    }
    if (n == 301) {
        return Error{ErrorCode::credentials_required, "netease requires a signed-in account", false, {}};
    }
    if (n == 403) return Error{ErrorCode::credentials_required, "netease denied access to the resource", false,
                              {{"netease_code", "403"}}};
    if (n == 404) return Error{ErrorCode::not_found, "netease has no such resource", false, {}};
    Error error{ErrorCode::upstream_failed, "netease refused the request", false, {}};
    if (n == 405) {
        error = Error{ErrorCode::upstream_rate_limited, "netease is limiting requests", true, {}};
    }
    error.details.push_back({"netease_code", std::to_string(n)});
    return error;
}

std::optional<std::string> text(const json& object, const char* name) {
    const auto it = object.find(name);
    if (it == object.end() || !it->is_string()) {
        return std::nullopt;
    }
    return it->get<std::string>();
}

std::optional<std::int64_t> integer(const json& object, const char* name) {
    const auto it = object.find(name);
    if (it == object.end() || !it->is_number_integer()) {
        return std::nullopt;
    }
    return it->get<std::int64_t>();
}

// An id the platform sends as a JSON number; positive values only.
std::optional<std::string> id_of(const json& object, const char* name = "id") {
    const auto value = integer(object, name);
    if (!value || *value <= 0) {
        return std::nullopt;
    }
    return std::to_string(*value);
}

std::optional<domain::PlatformRef> ref_of(const std::string& id) {
    auto source = domain::SourceId::parse(source_name);
    auto platform_id = domain::PlatformId::parse(id);
    if (!source || !platform_id) {
        return std::nullopt;
    }
    return domain::PlatformRef{std::move(source).value(), std::move(platform_id).value()};
}

// One song object as cloudsearch and v3/song/detail return it.
domain::Result<domain::Track> track_of(const json& song) {
    if (!song.is_object()) {
        return protocol_error("song is not an object");
    }
    const auto id = id_of(song);
    auto name = text(song, "name");
    if (!id || !name) {
        return protocol_error("song has no id or name");
    }
    auto ref = ref_of(*id);
    if (!ref) {
        return protocol_error("song id is not usable");
    }
    domain::Track track{*ref, std::move(*name), {}, std::nullopt, std::nullopt, {}};
    if (const auto artists = song.find(song.contains("ar") ? "ar" : "artists"); artists != song.end() && artists->is_array()) {
        for (const auto& artist : *artists) {
            if (!artist.is_object()) {
                continue;
            }
            auto artist_name = text(artist, "name");
            if (!artist_name || artist_name->empty()) {
                continue;
            }
            domain::ArtistSummary summary{std::move(*artist_name), std::nullopt};
            if (const auto artist_id = id_of(artist)) {
                summary.ref = ref_of(*artist_id);
            }
            track.artists.push_back(std::move(summary));
        }
    }
    if (const auto album = song.find(song.contains("al") ? "al" : "album"); album != song.end() && album->is_object()) {
        if (auto picture = text(*album, "picUrl"); picture && !picture->empty()) {
            auto url = application::transport::Url::parse(*picture);
            if (url && url->host().ends_with(".music.126.net")) {
                track.artwork = "https://" + url->host() + url->target();
            }
        }
        if (const auto count = integer(*album, "size"); count && *count > 0 && *count <= 65535) {
            track.track_total = static_cast<std::uint32_t>(*count);
        }
        if (auto album_name = text(*album, "name"); album_name && !album_name->empty()) {
            domain::AlbumSummary summary{std::move(*album_name), std::nullopt};
            if (const auto album_id = id_of(*album)) {
                summary.ref = ref_of(*album_id);
            }
            track.album = std::move(summary);
        }
    }
    // A missing or zero duration stays unknown.
    if (const auto number = integer(song, "no"); number && *number > 0 && *number <= 65535) {
        track.track_number = static_cast<std::uint32_t>(*number);
    }
    if (track.track_total && track.track_number && *track.track_total < *track.track_number) {
        track.track_total.reset();
    }
    if (const auto duration = integer(song, song.contains("dt") ? "dt" : "duration"); duration && *duration > 0) {
        track.duration_ms = *duration;
    }
    return track;
}

std::optional<std::string> lyric_text(const json& body, const char* name) {
    const auto it = body.find(name);
    if (it == body.end() || !it->is_object()) {
        return std::nullopt;
    }
    auto value = text(*it, "lyric");
    if (!value || value->find_first_not_of(" \r\n\t") == std::string::npos) {
        return std::nullopt;
    }
    return value;
}

bool flag(const json& body, const char* name) {
    const auto it = body.find(name);
    return it != body.end() && it->is_boolean() && it->get<bool>();
}

// The value of a query parameter in an already parsed target ("/path?a=1&b=2").
std::optional<std::string_view> query_value(std::string_view target, std::string_view name) {
    const auto question = target.find('?');
    if (question == std::string_view::npos) {
        return std::nullopt;
    }
    auto query = target.substr(question + 1);
    std::optional<std::string_view> result;
    while (!query.empty()) {
        const auto end = query.find('&');
        const auto pair = query.substr(0, end);
        query = end == std::string_view::npos ? std::string_view{} : query.substr(end + 1);
        if (pair.size() > name.size() && pair.substr(0, name.size()) == name && pair[name.size()] == '=') {
            if (result) return std::nullopt; // 同名参数不能产生歧义引用。
            result = pair.substr(name.size() + 1);
        }
    }
    return result;
}

} // namespace

bool is_valid_song_id(std::string_view id) noexcept {
    return !id.empty() && id.size() <= 19 && id.front() != '0' &&
           std::all_of(id.begin(), id.end(), [](char c) { return c >= '0' && c <= '9'; });
}

domain::Result<SearchPage> parse_search(std::string_view body) {
    auto parsed = parse_body(body);
    if (!parsed) {
        return std::move(parsed).error();
    }
    const auto result = parsed->find("result");
    if (result == parsed->end() || !result->is_object()) {
        return protocol_error("search has no result");
    }
    SearchPage page;
    const auto count = integer(*result, "songCount");
    if (!count || *count < 0) {
        return protocol_error("search has no songCount");
    }
    page.total = static_cast<std::uint64_t>(*count);
    const auto songs = result->find("songs");
    if (songs == result->end()) {
        return page; // no matches: only songCount
    }
    if (!songs->is_array()) {
        return protocol_error("songs is not a list");
    }
    for (const auto& song : *songs) {
        auto track = track_of(song);
        if (!track) {
            return std::move(track).error();
        }
        page.tracks.push_back(std::move(track).value());
    }
    return page;
}

domain::Result<domain::Track> parse_detail(std::string_view body, std::string_view id) {
    auto parsed = parse_body(body);
    if (!parsed) {
        return std::move(parsed).error();
    }
    const auto songs = parsed->find("songs");
    if (songs == parsed->end() || !songs->is_array()) {
        return protocol_error("detail has no songs");
    }
    if (songs->empty()) {
        return Error{ErrorCode::not_found, "netease has no such song", false, {}};
    }
    auto track = track_of(songs->front());
    if (!track) {
        return std::move(track).error();
    }
    if (track->ref.id.value() != id) {
        return protocol_error("detail is about another song");
    }
    return track;
}

domain::Result<AudioEvidence> parse_audio(std::string_view body, std::string_view id) {
    auto parsed = parse_body(body);
    if (!parsed) {
        return std::move(parsed).error();
    }
    const auto data = parsed->find("data");
    if (data == parsed->end() || !data->is_array() || data->size() != 1 || !data->front().is_object()) {
        return protocol_error("player url has no single entry");
    }
    const auto& entry = data->front();
    if (id_of(entry) != std::string{id}) {
        return protocol_error("player url is about another song");
    }
    AudioEvidence evidence;
    const auto code = integer(entry, "code");
    if (!code) {
        return protocol_error("player url has no code");
    }
    evidence.code = static_cast<int>(*code);
    evidence.fee = integer(entry, "fee");
    if (evidence.code != 200) {
        return evidence;
    }
    auto url = text(entry, "url");
    if (!url) {
        return protocol_error("playable entry has no url");
    }
    auto parsed_url = application::transport::Url::parse(*url);
    if (!parsed_url) {
        return protocol_error("player url is not a URL");
    }
    const auto& host = parsed_url->host();
    const std::string_view cdn = ".music.126.net";
    if (host.size() <= cdn.size() || host.compare(host.size() - cdn.size(), cdn.size(), cdn) != 0) {
        return protocol_error("player url is not on the netease CDN");
    }
    // The CDN answers the same file over HTTPS; plain HTTP is never used.
    auto https = application::transport::Url::parse("https://" + host + parsed_url->target());
    if (!https) {
        return protocol_error("player url cannot be used over https");
    }
    evidence.url = https->str();
    evidence.container = text(entry, "type").value_or("");
    std::transform(evidence.container.begin(), evidence.container.end(), evidence.container.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    evidence.level = text(entry, "level").value_or("");
    evidence.md5 = text(entry, "md5").value_or("");
    if (const auto br = integer(entry, "br"); br && *br > 0) {
        evidence.bitrate_bps = *br;
    }
    if (const auto size = integer(entry, "size"); size && *size > 0) {
        evidence.size_bytes = *size;
    }
    if (const auto time = integer(entry, "time"); time && *time > 0) {
        evidence.duration_ms = *time;
    }
    if (const auto expi = integer(entry, "expi"); expi && *expi > 0) {
        evidence.expires_in_seconds = *expi;
    }
    const auto trial = entry.find("freeTrialInfo");
    evidence.trial = trial != entry.end() && !trial->is_null();
    return evidence;
}

domain::Result<domain::TrackLyrics> parse_lyrics(std::string_view body) {
    auto parsed = parse_body(body);
    if (!parsed) {
        return std::move(parsed).error();
    }
    domain::TrackLyrics lyrics;
    if (flag(*parsed, "pureMusic") || flag(*parsed, "nolyric") || flag(*parsed, "uncollected")) {
        lyrics.unavailable = true;
        return lyrics;
    }
    lyrics.lrc = lyric_text(*parsed, "lrc");
    lyrics.translated_lrc = lyric_text(*parsed, "tlyric");
    lyrics.romanized_lrc = lyric_text(*parsed, "romalrc");
    for (const auto* key : {"lrc", "tlyric", "romalrc", "yrc"}) {
        const auto item = parsed->find(key);
        if (item != parsed->end() && !item->is_null() &&
            (!item->is_object() || (item->contains("lyric") && !(*item)["lyric"].is_null() && !(*item)["lyric"].is_string())))
            lyrics.warnings.push_back(std::string{key} == "yrc" ? "YRC_MALFORMED" : "LYRICS_MALFORMED");
    }
    if (const auto yrc = lyric_text(*parsed, "yrc")) {
        // Bounded single-pass parser. Invalid timing never becomes wrapped/negative timestamps.
        const auto number = [](std::string_view s, std::int64_t& n) {
            const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
            return !s.empty() && ec == std::errc{} && end == s.data() + s.size() && n >= 0 && n <= 86400000;
        };
        std::string_view rest{*yrc};
        bool malformed = false;
        std::size_t words = 0, lines = 0;
        while (!rest.empty()) {
            const auto newline = rest.find('\n');
            auto line = rest.substr(0, newline);
            rest = newline == rest.npos ? std::string_view{} : rest.substr(newline + 1);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (line.find_first_not_of(" \t\r") == line.npos || line.front() == '{') continue; // Credit metadata is not timed words.
            if (++lines > 10000 || words > 100000) { malformed = true; break; }
            const auto comma = line.find(','), close = line.find(']');
            domain::KaraokeLine k;
            if (line.front() != '[' || comma == line.npos || close == line.npos || comma > close ||
                !number(line.substr(1, comma - 1), k.start_ms) ||
                !number(line.substr(comma + 1, close - comma - 1), k.duration_ms) ||
                k.duration_ms > 86400000 - k.start_ms) { malformed = true; continue; }
            line.remove_prefix(close + 1);
            bool valid = true;
            while (!line.empty()) {
                const auto a = line.find(','), b = a == line.npos ? line.npos : line.find(',', a + 1);
                const auto end = line.find(')');
                domain::KaraokeWord word;
                std::int64_t flags{};
                if (line.front() != '(' || a == line.npos || b == line.npos || end == line.npos || b >= end ||
                    !number(line.substr(1, a - 1), word.start_ms) ||
                    !number(line.substr(a + 1, b - a - 1), word.duration_ms) ||
                    !number(line.substr(b + 1, end - b - 1), flags) ||
                    word.start_ms < k.start_ms || word.start_ms > k.start_ms + k.duration_ms ||
                    word.duration_ms > k.start_ms + k.duration_ms - word.start_ms ||
                    (!k.words.empty() && word.start_ms < k.words.back().start_ms) || ++words > 100000) {
                    valid = false; break;
                }
                line.remove_prefix(end + 1);
                // A literal '(' in a lyric is text unless it starts a timing token.
                auto next = line.find('(');
                while (next != line.npos && (next + 1 == line.size() || line[next + 1] < '0' || line[next + 1] > '9'))
                    next = line.find('(', next + 1);
                word.text = line.substr(0, next);
                k.words.push_back(std::move(word));
                line = next == line.npos ? std::string_view{} : line.substr(next);
            }
            if (valid && !k.words.empty()) lyrics.karaoke.push_back(std::move(k));
            else malformed = true;
        }
        if (malformed) lyrics.warnings.push_back("YRC_MALFORMED");
    }
    lyrics.unavailable = !lyrics.lrc && !lyrics.translated_lrc && !lyrics.romanized_lrc && lyrics.karaoke.empty();
    return lyrics;
}

domain::Result<Account> parse_account(std::string_view body) {
    auto parsed = parse_body(body);
    if (!parsed) {
        return std::move(parsed).error();
    }
    Account account;
    const auto record = parsed->find("account");
    if (record == parsed->end() || record->is_null()) {
        return account;
    }
    if (!record->is_object() || !id_of(*record)) {
        return protocol_error("account has no id");
    }
    account.signed_in = true;
    account.user_id = id_of(*record);
    if (const auto profile = parsed->find("profile"); profile != parsed->end() && profile->is_object()) {
        account.nickname = text(*profile, "nickname");
        account.vip_type = integer(*profile, "vipType");
        if (auto uid = id_of(*profile, "userId")) account.user_id = std::move(uid);
    }
    if (!account.vip_type) {
        account.vip_type = integer(*record, "vipType");
    }
    return account;
}

namespace {
std::optional<std::uint64_t> count_of(const json& value, const char* key) {
    auto n = integer(value, key);
    return n && *n >= 0 ? std::optional<std::uint64_t>{static_cast<std::uint64_t>(*n)} : std::nullopt;
}

domain::Artwork picture_of(const json& value, const char* key) {
    auto s = text(value, key);
    if (!s) return {};
    auto u = application::transport::Url::parse(*s);
    if (!u || !u->host().ends_with(".music.126.net")) return {};
    return "https://" + u->host() + u->target();
}

std::optional<std::string> date_of(const json& value, const char* key, bool date_only) {
    auto n = integer(value, key);
    if (!n || *n <= 0 || *n > 4102444800000LL) return {};
    auto time = std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{*n}};
    if (date_only) return std::format("{:%F}", std::chrono::floor<std::chrono::days>(time));
    return std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(time));
}

domain::Result<domain::Playlist> playlist_of(const json& item) {
    auto id = id_of(item);
    auto name = text(item, "name");
    if (!id || !name || name->empty()) return protocol_error("playlist lacks id or name");
    domain::Playlist p{*ref_of(*id)};
    p.name = *name;
    p.description = text(item, "description");
    p.artwork = picture_of(item, "coverImgUrl");
    if (!p.artwork) p.artwork = picture_of(item, "picUrl");
    p.track_count = count_of(item, "trackCount");
    p.play_count = count_of(item, "playCount");
    if (!p.play_count) {
        const auto n = item.find("playCount");
        if (n != item.end() && n->is_number_float()) {
            const auto value = n->get<double>();
            if (std::isfinite(value) && value >= 0 && value < 9.22e18 && std::floor(value) == value)
                p.play_count = static_cast<std::uint64_t>(value);
        }
    }
    p.updated_at = date_of(item, "updateTime", false);
    if (auto creator = item.find("creator"); creator != item.end() && creator->is_object())
        p.creator = text(*creator, "nickname");
    return p;
}

domain::Result<domain::Album> album_of(const json& item) {
    auto id = id_of(item);
    auto title = text(item, "name");
    if (!id || !title || title->empty()) return protocol_error("album lacks id or name");
    domain::Album a{*ref_of(*id)};
    a.title = *title;
    a.description = text(item, "description");
    a.artwork = picture_of(item, "picUrl");
    a.track_count = count_of(item, "size");
    a.release_date = date_of(item, "publishTime", true);
    auto artists = item.find("artists");
    if (artists != item.end() && artists->is_array()) {
        for (const auto& v : *artists) {
            auto name = text(v, "name");
            if (name && !name->empty()) a.artists.push_back({*name, id_of(v) ? ref_of(*id_of(v)) : std::nullopt});
        }
    }
    if (a.artists.empty()) {
        auto v = item.find("artist");
        if (v != item.end() && v->is_object()) {
            if (auto name = text(*v, "name"); name && !name->empty())
                a.artists.push_back({*name, id_of(*v) ? ref_of(*id_of(*v)) : std::nullopt});
        }
    }
    return a;
}

std::string revision_of(const json& header, const std::vector<std::string>& ids) {
    // 非秘密检测标记；公开游标由应用层绑定集合、身份和页大小。
    const auto encoded = json{{"ids", ids}, {"count", header.value("trackCount", json{})},
        {"updated", header.value("trackUpdateTime", json{})}, {"size", header.value("size", json{})}}.dump();
    std::uint64_t h = 14695981039346656037ULL;
    for (const unsigned char c : encoded) { h ^= c; h *= 1099511628211ULL; }
    return std::format("{:016x}", h);
}

template<class T, class Parse>
domain::Result<CollectionSearch<T>> search_collection(std::string_view body, const char* list_key,
                                                     const char* count_key, Parse parse) {
    auto doc = parse_body(body);
    if (!doc) return doc.error();
    auto r = doc->find("result");
    if (r == doc->end() || !r->is_object()) return protocol_error("search has no result");
    auto total = count_of(*r, count_key);
    if (!total) return protocol_error("search has no count");
    CollectionSearch<T> found{{}, *total};
    auto list = r->find(list_key);
    if (list == r->end() && *total == 0) return found;
    if (list == r->end() || !list->is_array() || list->size() > 50) return protocol_error("invalid search list");
    for (const auto& value : *list) {
        auto item = parse(value);
        if (!item) return item.error();
        found.items.push_back(std::move(*item));
    }
    return found;
}
} // namespace

domain::Result<CollectionSearch<domain::Playlist>> parse_playlist_search(std::string_view body) {
    return search_collection<domain::Playlist>(body, "playlists", "playlistCount", playlist_of);
}
domain::Result<CollectionSearch<domain::Album>> parse_album_search(std::string_view body) {
    return search_collection<domain::Album>(body, "albums", "albumCount", album_of);
}

domain::Result<CollectionSearch<domain::Artist>> parse_artist_search(std::string_view body) {
    return search_collection<domain::Artist>(body, "artists", "artistCount", [](const json& item) -> domain::Result<domain::Artist> {
        auto id = id_of(item);
        auto name = text(item, "name");
        if (!id || !name || name->empty()) return protocol_error("artist lacks id or name");
        return domain::Artist{*ref_of(*id), *name, picture_of(item, "picUrl")};
    });
}

domain::Result<ArtistAlbums> parse_artist_albums(std::string_view body, std::string_view id) {
    auto doc = parse_body(body);
    if (!doc) return doc.error();
    auto artist = doc->find("artist");
    auto albums = doc->find("hotAlbums");
    auto more = doc->find("more");
    if (artist == doc->end() || !artist->is_object() || id_of(*artist) != id ||
        albums == doc->end() || !albums->is_array() || albums->size() > 50 ||
        more == doc->end() || !more->is_boolean()) return protocol_error("invalid artist albums page");
    ArtistAlbums result{{}, more->get<bool>()};
    for (const auto& value : *albums) {
        auto a = album_of(value);
        if (!a) return a.error();
        result.items.push_back(std::move(*a));
    }
    if (result.more && result.items.empty()) return protocol_error("empty continuing artist page");
    return result;
}

domain::Result<std::vector<domain::Category>> parse_categories(std::string_view body) {
    auto doc = parse_body(body);
    if (!doc) return doc.error();
    auto sub = doc->find("sub");
    auto groups = doc->find("categories");
    if (sub == doc->end() || !sub->is_array() || sub->size() > 256 ||
        groups == doc->end() || !groups->is_object()) return protocol_error("invalid category directory");
    std::vector<domain::Category> result;
    std::set<std::string> ids;
    if (auto all = doc->find("all"); all != doc->end() && !all->is_null()) {
        auto name = text(*all, "name");
        if (!name || name->empty()) return protocol_error("invalid all category");
        result.push_back({"全部", *name}); // Upstream cat parameter, display name is 全部歌单.
        ids.insert("全部");
    }
    for (const auto& item : *sub) {
        auto name = text(item, "name");
        auto group = integer(item, "category");
        if (!name || !domain::PlatformId::parse(*name) || !ids.insert(*name).second || !group)
            return protocol_error("invalid or duplicate category");
        auto label = text(*groups, std::to_string(*group).c_str());
        result.push_back({*name, *name, std::move(label)});
    }
    return result;
}

domain::Result<std::vector<domain::Chart>> parse_charts(std::string_view body) {
    auto doc = parse_body(body);
    if (!doc) return doc.error();
    auto list = doc->find("list");
    if (list == doc->end() || !list->is_array() || list->size() > 256) return protocol_error("invalid chart directory");
    std::vector<domain::Chart> result;
    std::set<std::string> ids;
    for (const auto& item : *list) {
        auto id = id_of(item);
        auto name = text(item, "name");
        if (!id || !name || name->empty() || !ids.insert(*id).second) return protocol_error("invalid or duplicate chart");
        domain::Chart chart{*ref_of(*id)};
        chart.name = *name;
        chart.description = text(item, "description");
        chart.update_hint = text(item, "updateFrequency");
        chart.updated_at = date_of(item, "trackUpdateTime", false);
        chart.track_count = count_of(item, "trackCount");
        result.push_back(std::move(chart));
    }
    return result;
}

domain::Result<DiscoveryPlaylists> parse_discovery_playlists(std::string_view body, bool recommendations) {
    auto doc = parse_body(body);
    if (!doc) return doc.error();
    DiscoveryPlaylists result;
    if (!recommendations) {
        result.total = count_of(*doc, "total");
        auto more = doc->find("more");
        if (!result.total || more == doc->end() || !more->is_boolean()) return protocol_error("missing playlist paging facts");
        result.more = more->get<bool>();
    }
    auto list = doc->find(recommendations ? "result" : "playlists");
    if (list == doc->end() || !list->is_array() || list->size() > (recommendations ? 30U : 50U))
        return protocol_error("invalid discovery playlist list");
    std::set<std::string> seen;
    for (const auto& item : *list) {
        auto playlist = playlist_of(item);
        if (!playlist) return playlist.error();
        if (!seen.insert(playlist->ref.id.value()).second) return protocol_error("duplicate discovery playlist");
        if (recommendations) playlist->description = text(item, "copywriter");
        result.items.push_back(std::move(*playlist));
    }
    if (result.more && result.items.empty()) return protocol_error("empty page cannot have more items");
    return result;
}

domain::Result<PlaylistData> parse_playlist(std::string_view body, std::string_view id) {
    auto doc = parse_body(body);
    if (!doc) return doc.error();
    auto p = doc->find("playlist");
    if (p != doc->end() && p->is_null()) return Error{ErrorCode::not_found, "playlist not found", false, {}};
    if (p == doc->end() || !p->is_object()) return protocol_error("response lacks playlist");
    auto detail = playlist_of(*p);
    if (!detail) return detail.error();
    if (detail->ref.id.value() != id) return protocol_error("playlist id mismatch");
    auto ids = p->find("trackIds");
    if (ids == p->end() || !ids->is_array() || ids->size() > max_playlist_ids)
        return protocol_error("missing or oversized trackIds (maximum 10000)");
    PlaylistData result{std::move(*detail), {}, {}};
    for (const auto& v : *ids) {
        auto song = id_of(v);
        if (!song) return protocol_error("invalid playlist track id");
        result.ids.push_back(*song);
    }
    result.revision = revision_of(*p, result.ids);
    return result;
}

domain::Result<AlbumData> parse_album(std::string_view body, std::string_view id) {
    auto doc = parse_body(body);
    if (!doc) return doc.error();
    auto a = doc->find("album");
    if (a != doc->end() && a->is_null()) return Error{ErrorCode::not_found, "album not found", false, {}};
    if (a == doc->end() || !a->is_object()) return protocol_error("response lacks album");
    auto detail = album_of(*a);
    if (!detail) return detail.error();
    if (detail->ref.id.value() != id) return protocol_error("album id mismatch");
    auto songs = doc->find("songs");
    if (songs == doc->end() || !songs->is_array() || songs->size() > max_album_tracks)
        return protocol_error("missing or oversized album songs (maximum 2000)");
    AlbumData result{std::move(*detail), {}, {}};
    std::vector<std::string> ids;
    for (const auto& song : *songs) {
        auto track = track_of(song);
        if (!track) return track.error();
        // 总数来自歌曲确属的专辑；不使用歌单位置或当前页大小填曲号。
        if (track->album && track->album->ref && track->album->ref->id.value() == id && result.detail.track_count &&
            *result.detail.track_count <= 65535 && (!track->track_number || *result.detail.track_count >= *track->track_number))
            track->track_total = static_cast<std::uint32_t>(*result.detail.track_count);
        ids.push_back(track->ref.id.value());
        result.tracks.push_back(std::move(*track));
    }
    result.revision = revision_of(*a, ids);
    return result;
}

domain::Result<std::vector<domain::Track>> parse_tracks(std::string_view body) {
    auto doc = parse_body(body);
    if (!doc) return doc.error();
    auto songs = doc->find("songs");
    if (songs == doc->end() || !songs->is_array() || songs->size() > 50) return protocol_error("invalid song detail batch");
    std::vector<domain::Track> tracks;
    for (const auto& song : *songs) {
        auto track = track_of(song);
        if (!track) return track.error();
        tracks.push_back(std::move(*track));
    }
    return tracks;
}

domain::Result<DiscoveryPlaylists> parse_user_playlists(std::string_view body, std::string_view uid) {
    auto parsed = parse_body(body);
    if (!parsed) return parsed.error();
    const auto list = parsed->find("playlist");
    if (list == parsed->end() || !list->is_array() || list->size() > 50 ||
        !parsed->contains("more") || !(*parsed)["more"].is_boolean())
        return protocol_error("personal playlist array missing or too large");
    DiscoveryPlaylists result;
    result.more = flag(*parsed, "more");
    for (const auto& v : *list) {
        auto p = playlist_of(v);
        if (!p) return p.error();
        const auto creator = v.find("creator");
        const auto owner = creator != v.end() && creator->is_object() ? id_of(*creator, "userId") : std::nullopt;
        if (owner && *owner == uid) p->relationship = "created";
        else if (flag(v, "subscribed")) p->relationship = "subscribed";
        else return protocol_error("personal playlist relationship unknown");
        result.items.push_back(std::move(*p));
    }
    if (result.more && result.items.empty()) return protocol_error("empty personal page claims more");
    return result;
}

domain::Result<ArtistTracks> parse_artist_tracks(std::string_view body, std::string_view id) {
    auto parsed = parse_body(body);
    if (!parsed) return parsed.error();
    const auto artist = parsed->find("artist"), list = parsed->find("hotSongs");
    if (artist == parsed->end() || artist->is_null()) return Error{ErrorCode::not_found, "artist not found", false, {}};
    if (id_of(*artist) != id || list == parsed->end() || !list->is_array() || list->size() > 50)
        return protocol_error("invalid artist hot songs");
    ArtistTracks result;
    std::vector<std::string> ids;
    for (const auto& item : *list) {
        auto track = track_of(item);
        if (!track) return track.error();
        ids.push_back(track->ref.id.value());
        result.tracks.push_back(std::move(*track));
    }
    result.revision = revision_of(*artist, ids);
    return result;
}

namespace {
domain::Result<domain::Podcast> podcast_of(const json& item) {
    const auto id = id_of(item), name = text(item, "name");
    if (!id || !name || name->empty()) return protocol_error("podcast lacks id or name");
    domain::Podcast p{*ref_of(*id), *name};
    p.artwork = picture_of(item, "picUrl");
    p.description = text(item, "desc");
    p.episode_count = count_of(item, "programCount");
    if (const auto dj = item.find("dj"); dj != item.end()) p.presenter = text(*dj, "nickname");
    return p;
}

domain::Result<domain::Episode> episode_of(const json& item) {
    const auto id = id_of(item), name = text(item, "name");
    const auto radio = item.find("radio");
    if (!id || !name || radio == item.end()) return protocol_error("episode lacks id, name or radio");
    auto p = podcast_of(*radio);
    if (!p) return p.error();
    domain::Episode e{*ref_of(*id), p->ref, *name, p->title};
    e.artwork = picture_of(item, "coverUrl");
    if (!e.artwork) e.artwork = p->artwork;
    e.presenter = p->presenter;
    if (const auto dj = item.find("dj"); dj != item.end()) e.presenter = text(*dj, "nickname");
    e.description = text(item, "description");
    e.published_at = date_of(item, "createTime", false);
    if (auto duration = integer(item, "duration"); duration && *duration > 0) e.duration_ms = duration;
    const auto fee = integer(item, "programFeeType");
    e.requires_purchase = fee && *fee != 0 && !flag(item, "buyed");
    if (const auto song = item.find("mainSong"); song != item.end()) {
        if (const auto sid = id_of(*song)) e.audio_ref = ref_of("program:" + *id + ":" + *sid);
    }
    return e;
}
} // namespace

std::optional<ProgramAudio> program_audio(std::string_view id) {
    if (!id.starts_with("program:")) return std::nullopt;
    id.remove_prefix(8);
    const auto colon = id.find(':');
    if (colon == id.npos || !is_valid_song_id(id.substr(0, colon)) || !is_valid_song_id(id.substr(colon + 1)))
        return std::nullopt;
    return ProgramAudio{std::string{id.substr(0, colon)}, std::string{id.substr(colon + 1)}};
}

domain::Result<domain::Podcast> parse_podcast(std::string_view body, std::string_view id) {
    auto parsed = parse_body(body);
    if (!parsed) return parsed.error();
    const auto p = parsed->find("data");
    if (p == parsed->end() || p->is_null()) return Error{ErrorCode::not_found, "podcast not found", false, {}};
    if (id_of(*p) != id) return protocol_error("podcast id mismatch");
    return podcast_of(*p);
}

domain::Result<domain::Episode> parse_episode(std::string_view body, std::string_view id) {
    auto parsed = parse_body(body);
    if (!parsed) return parsed.error();
    const auto p = parsed->find("program");
    if (p == parsed->end() || p->is_null()) return Error{ErrorCode::not_found, "episode not found", false, {}};
    if (id_of(*p) != id) return protocol_error("episode id mismatch");
    return episode_of(*p);
}

domain::Result<Episodes> parse_episodes(std::string_view body, std::string_view radio) {
    auto parsed = parse_body(body);
    if (!parsed) return parsed.error();
    const auto list = parsed->find("programs");
    if (list == parsed->end() || !list->is_array() || list->size() > 100 ||
        !parsed->contains("more") || !(*parsed)["more"].is_boolean()) return protocol_error("invalid episode array or paging flag");
    Episodes result;
    result.total = count_of(*parsed, "count");
    result.more = flag(*parsed, "more");
    for (const auto& item : *list) {
        auto e = episode_of(item);
        if (!e) return e.error();
        if (e->podcast.id.value() != radio) return protocol_error("episode belongs to another radio");
        result.items.push_back(std::move(*e));
    }
    if (result.more && result.items.empty()) return protocol_error("empty episode page claims more");
    return result;
}

domain::Result<Link> parse_link(std::string_view link) {
    const auto refuse = [] { return Error::validation("link", "is not a supported netease link"); };
    // The web player keeps its route in the fragment: /#/song?id=N.
    std::string text{link};
    if (const auto hash = text.find("/#/"); hash != std::string::npos) {
        text.erase(hash + 1, 2);
    }
    auto url = application::transport::Url::parse(text);
    if (!url || url->port() != (url->is_https() ? 443 : 80)) {
        return refuse();
    }
    const auto& host = url->host();
    if (host != "music.163.com" && host != "y.music.163.com" && host != "www.music.163.com") {
        return refuse();
    }
    auto path = url->path();
    if (!path.empty() && path.back() == '/' && path.size() > 1) {
        path.remove_suffix(1);
    }
    if (path.starts_with("/m/")) path.remove_prefix(2);
    for (const auto* kind : {"song", "playlist", "album", "artist", "djradio", "program"}) {
        const auto prefix = "/" + std::string{kind};
        std::optional<std::string_view> id;
        if (path == prefix) id = query_value(url->target(), "id");
        else if (path.starts_with(prefix + "/")) id = path.substr(prefix.size() + 1);
        if (id && is_valid_song_id(*id)) {
            const std::string k{kind};
            return Link{k == "song" ? "track" : k == "djradio" ? "podcast" : k == "program" ? "episode" : k, std::string{*id}};
        }
    }
    return refuse();
}

domain::Result<std::string> parse_track_link(std::string_view link) {
    auto parsed = parse_link(link);
    if (!parsed) return parsed.error();
    if (parsed->kind != "track") return Error::validation("link", "is not a netease song link");
    return parsed->id;
}

} // namespace music_service::providers::netease::parsing
