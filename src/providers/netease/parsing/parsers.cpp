#include "providers/netease/parsing/parsers.hpp"

#include "application/transport/url.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
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
    if (const auto artists = song.find("ar"); artists != song.end() && artists->is_array()) {
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
    if (const auto album = song.find("al"); album != song.end() && album->is_object()) {
        if (auto album_name = text(*album, "name"); album_name && !album_name->empty()) {
            domain::AlbumSummary summary{std::move(*album_name), std::nullopt};
            if (const auto album_id = id_of(*album)) {
                summary.ref = ref_of(*album_id);
            }
            track.album = std::move(summary);
        }
    }
    // A missing or zero duration stays unknown.
    if (const auto duration = integer(song, "dt"); duration && *duration > 0) {
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
    while (!query.empty()) {
        const auto end = query.find('&');
        const auto pair = query.substr(0, end);
        query = end == std::string_view::npos ? std::string_view{} : query.substr(end + 1);
        if (pair.size() > name.size() && pair.substr(0, name.size()) == name && pair[name.size()] == '=') {
            return pair.substr(name.size() + 1);
        }
    }
    return std::nullopt;
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
    if (!lyrics.lrc) {
        lyrics.translated_lrc.reset();
        lyrics.unavailable = true;
    }
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
    if (const auto profile = parsed->find("profile"); profile != parsed->end() && profile->is_object()) {
        account.nickname = text(*profile, "nickname");
        account.vip_type = integer(*profile, "vipType");
    }
    if (!account.vip_type) {
        account.vip_type = integer(*record, "vipType");
    }
    return account;
}

domain::Result<std::string> parse_track_link(std::string_view link) {
    const auto refuse = [] { return Error::validation("link", "is not a netease song link"); };
    // The web player keeps its route in the fragment: /#/song?id=N.
    std::string text{link};
    if (const auto hash = text.find("/#/"); hash != std::string::npos) {
        text.erase(hash + 1, 2);
    }
    auto url = application::transport::Url::parse(text);
    if (!url) {
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
    std::optional<std::string_view> id;
    if (path == "/song" || path == "/m/song") {
        id = query_value(url->target(), "id");
    } else if (path.substr(0, 6) == "/song/") {
        id = path.substr(6);
    } else if (path.substr(0, 8) == "/m/song/") {
        id = path.substr(8);
    }
    if (!id || !is_valid_song_id(*id)) {
        return Error::validation("link", "does not name a netease song id");
    }
    return std::string{*id};
}

} // namespace music_service::providers::netease::parsing
