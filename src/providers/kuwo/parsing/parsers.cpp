#include "providers/kuwo/parsing/parsers.hpp"

#include "application/transport/url.hpp"
#include "domain/catalog/refs.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
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

// A string or integer member as an integer.
std::optional<std::int64_t> as_int(const json& value) {
    if (value.is_number_integer()) {
        return value.get<std::int64_t>();
    }
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        std::int64_t parsed = 0;
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (ec == std::errc{} && end == text.data() + text.size()) {
            return parsed;
        }
    }
    return std::nullopt;
}

std::string as_string(const json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

std::optional<domain::PlatformRef> ref_for(std::string_view id) {
    if (id.empty() || id == "0") {
        return std::nullopt;
    }
    auto platform_id = domain::PlatformId::parse(id);
    auto source = domain::SourceId::parse(source_name);
    if (!platform_id || !source) {
        return std::nullopt;
    }
    return domain::PlatformRef{std::move(source).value(), std::move(platform_id).value()};
}

std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> parts;
    while (true) {
        const auto at = text.find(separator);
        const auto part = trim(text.substr(0, at));
        if (!part.empty()) {
            parts.emplace_back(part);
        }
        if (at == std::string_view::npos) {
            return parts;
        }
        text.remove_prefix(at + 1);
    }
}

// Kuwo joins several artists with '&'; ids line up when the counts match.
std::vector<domain::ArtistSummary> artists_from(std::string_view names, std::string_view ids) {
    const auto name_list = split(decode_text(names), '&');
    const auto id_list = split(ids, '&');
    std::vector<domain::ArtistSummary> artists;
    for (std::size_t i = 0; i < name_list.size(); ++i) {
        std::optional<domain::ArtistRef> ref;
        if (id_list.size() == name_list.size()) {
            ref = ref_for(id_list[i]);
        }
        artists.push_back({name_list[i], std::move(ref)});
    }
    return artists;
}

// The text after `marker`, up to `end`, within [from, html.size()).
std::optional<std::string_view> between(std::string_view html, std::string_view marker, std::string_view end,
                                        std::size_t from = 0) {
    const auto start = html.find(marker, from);
    if (start == std::string_view::npos) {
        return std::nullopt;
    }
    const auto value = start + marker.size();
    const auto stop = html.find(end, value);
    if (stop == std::string_view::npos) {
        return std::nullopt;
    }
    return html.substr(value, stop - value);
}

std::string strip_tags(std::string_view html) {
    std::string out;
    bool in_tag = false;
    for (const char c : html) {
        if (c == '<') {
            in_tag = true;
        } else if (c == '>') {
            in_tag = false;
        } else if (!in_tag) {
            out.push_back(c);
        }
    }
    return out;
}

std::optional<std::int64_t> leading_digits(std::string_view text) {
    std::int64_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end == text.data()) {
        return std::nullopt;
    }
    return value;
}

// Seconds from the page state's songinfo: duration:<n>, or songTimeMinutes:"mm:ss".
// A duration that the minified state refers to by variable stays unknown.
std::optional<std::int64_t> page_duration(std::string_view html) {
    const auto info = html.find("songinfo:{");
    if (info == std::string_view::npos) {
        return std::nullopt;
    }
    if (auto value = between(html, "duration:", ",", info)) {
        if (auto seconds = leading_digits(*value); seconds && *seconds > 0) {
            return seconds;
        }
    }
    if (auto value = between(html, "songTimeMinutes:\"", "\"", info)) {
        const auto colon = value->find(':');
        if (colon != std::string_view::npos) {
            const auto minutes = leading_digits(value->substr(0, colon));
            const auto seconds = leading_digits(value->substr(colon + 1));
            if (minutes && seconds && (*minutes > 0 || *seconds > 0)) {
                return *minutes * 60 + *seconds;
            }
        }
    }
    return std::nullopt;
}

} // namespace

bool is_valid_rid(std::string_view rid) noexcept {
    return !rid.empty() && rid.size() <= 18 && rid.front() != '0' &&
           std::all_of(rid.begin(), rid.end(), [](char c) { return c >= '0' && c <= '9'; });
}

std::string decode_text(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '&') {
            out.push_back(text[i]);
            continue;
        }
        const auto semicolon = text.find(';', i);
        if (semicolon == std::string_view::npos || semicolon - i > 10) {
            out.push_back('&');
            continue;
        }
        const auto entity = text.substr(i + 1, semicolon - i - 1);
        std::optional<std::uint32_t> code;
        if (entity == "amp") {
            code = '&';
        } else if (entity == "lt") {
            code = '<';
        } else if (entity == "gt") {
            code = '>';
        } else if (entity == "quot") {
            code = '"';
        } else if (entity == "apos" || entity == "#39") {
            code = '\'';
        } else if (entity == "nbsp") {
            code = ' ';
        } else if (entity.size() > 1 && entity.front() == '#') {
            const bool hex = entity[1] == 'x' || entity[1] == 'X';
            const auto digits = entity.substr(hex ? 2 : 1);
            std::uint32_t value = 0;
            const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value, hex ? 16 : 10);
            if (ec == std::errc{} && end == digits.data() + digits.size() && value > 0 && value <= 0x10FFFF &&
                (value < 0xD800 || value > 0xDFFF)) {
                code = value;
            }
        }
        if (!code) {
            out.push_back('&');
            continue;
        }
        append_utf8(out, *code);
        i = semicolon;
    }
    return std::string{trim(out)};
}

domain::Result<SearchPage> parse_search(std::string_view body) {
    auto document = json::parse(body, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return protocol_error("search response is not a JSON object");
    }
    const auto list = document.find("abslist");
    // Plain values with a sentinel: GCC -O2 reports reads of checked optionals here as
    // maybe-uninitialized.
    const std::int64_t total = document.contains("TOTAL") ? as_int(document["TOTAL"]).value_or(-1) : -1;
    const std::int64_t page = document.contains("PN") ? as_int(document["PN"]).value_or(-1) : -1;
    if (list == document.end() || !list->is_array() || total < 0 || page < 0) {
        return protocol_error("search response lacks abslist, TOTAL or PN");
    }
    SearchPage result;
    result.total = static_cast<std::uint64_t>(total);
    result.page = static_cast<std::uint32_t>(page);
    for (const auto& item : *list) {
        if (!item.is_object()) {
            return protocol_error("search item is not an object");
        }
        auto rid = as_string(item, "MUSICRID");
        if (rid.rfind("MUSIC_", 0) == 0) {
            rid.erase(0, 6);
        }
        auto ref = is_valid_rid(rid) ? ref_for(rid) : std::nullopt;
        const auto title = decode_text(as_string(item, "SONGNAME"));
        if (!ref || title.empty()) {
            return protocol_error("search item lacks MUSICRID or SONGNAME");
        }
        domain::Track track{std::move(*ref), title, artists_from(as_string(item, "ARTIST"), as_string(item, "ARTISTID")),
                            std::nullopt, std::nullopt, {}};
        if (auto album = decode_text(as_string(item, "ALBUM")); !album.empty()) {
            track.album = domain::AlbumSummary{std::move(album), ref_for(as_string(item, "ALBUMID"))};
        }
        if (item.contains("DURATION")) {
            if (const auto seconds = as_int(item["DURATION"]); seconds && *seconds > 0) {
                track.duration_ms = *seconds * 1000;
            }
        }
        result.tracks.push_back(std::move(track));
    }
    return result;
}

domain::Result<domain::Track> parse_track_page(std::string_view html, std::string_view rid) {
    const auto name = between(html, "id=\"songinfo-name\" type=\"hidden\" value=\"", "\"");
    if (!name) {
        return domain::Error{domain::ErrorCode::not_found, "kuwo has no such song", false, {}};
    }
    // The page must be about the requested song.
    const auto canonical = between(html, "rel=\"canonical\" href=\"", "\"");
    const auto expected = "/play_detail/" + std::string{rid};
    if (!canonical || canonical->size() < expected.size() ||
        canonical->substr(canonical->size() - expected.size()) != expected) {
        return protocol_error("song page does not belong to the requested rid");
    }
    auto title = decode_text(*name);
    if (title.empty()) {
        return protocol_error("song page has an empty title");
    }
    auto ref = ref_for(rid);
    if (!ref) {
        return domain::Error::validation("rid", "is not a kuwo song id");
    }
    domain::Track track{std::move(*ref), std::move(title), {}, std::nullopt, std::nullopt, {}};

    if (const auto artist_block = html.find("class=\"artist_name"); artist_block != std::string_view::npos) {
        if (auto artist = between(html, "<span class=\"name\"", "</span>", artist_block)) {
            const auto text = artist->substr(std::min(artist->find('>') + 1, artist->size()));
            track.artists = artists_from(strip_tags(text), "");
        }
    }
    if (const auto album_block = html.find("class=\"tip album_name\""); album_block != std::string_view::npos) {
        if (auto album = between(html, ">", "</span>", album_block)) {
            if (auto text = decode_text(strip_tags(*album)); !text.empty()) {
                track.album = domain::AlbumSummary{std::move(text), std::nullopt};
            }
        }
    }
    if (const auto seconds = page_duration(html)) {
        track.duration_ms = *seconds * 1000;
    }
    return track;
}

domain::Result<AudioEvidence> parse_audio(std::string_view body, std::string_view requested_rid) {
    auto document = json::parse(body, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return protocol_error("audio response is not a JSON object");
    }
    const auto code = document.contains("code") ? as_int(document["code"]) : std::nullopt;
    if (!code) {
        return protocol_error("audio response lacks code");
    }
    const auto data = document.find("data");
    const auto url_text = data != document.end() && data->is_object() ? as_string(*data, "url") : std::string{};
    if (*code != 200 || url_text.empty() || url_text == "None") {
        domain::Error error{domain::ErrorCode::upstream_failed, "kuwo returned no audio address", false, {}};
        error.details.push_back({"kuwo_code", std::to_string(*code)});
        return error;
    }

    auto url = application::transport::Url::parse(url_text);
    if (!url) {
        return protocol_error("audio address is not a valid URL");
    }
    const auto& host = url->host();
    const bool kuwo_host = host == "kuwo.cn" || (host.size() > 8 && host.substr(host.size() - 8) == ".kuwo.cn");
    const bool default_port = url->port() == (url->is_https() ? 443 : 80);
    if (!kuwo_host || !default_port) {
        return protocol_error("audio address is not on a kuwo.cn host");
    }

    AudioEvidence evidence;
    // The CDN serves the same paths over HTTPS with a valid certificate.
    evidence.url = "https://" + host + url->target();
    evidence.format = as_string(*data, "format");
    if (data->contains("bitrate")) {
        evidence.bitrate_kbps = as_int((*data)["bitrate"]);
    }
    if (data->contains("duration")) {
        evidence.duration_seconds = as_int((*data)["duration"]);
    }
    const auto returned_rid = data->contains("rid") ? as_int((*data)["rid"]) : std::nullopt;
    const auto type = data->contains("type") ? as_int((*data)["type"]) : std::nullopt;
    evidence.substitute = !returned_rid || std::to_string(*returned_rid) != requested_rid || (type && *type != 0);
    return evidence;
}

domain::Result<std::string> parse_track_link(std::string_view link) {
    auto url = application::transport::Url::parse(link);
    if (!url) {
        return domain::Error::validation("link", "is not a kuwo song link");
    }
    const auto path = url->path();
    std::string_view prefix;
    if (url->host() == "www.kuwo.cn" || url->host() == "kuwo.cn") {
        prefix = "/play_detail/";
    } else if (url->host() == "m.kuwo.cn") {
        prefix = "/newh5app/play_detail/";
    } else {
        return domain::Error::validation("link", "is not a kuwo song link");
    }
    if (path.substr(0, prefix.size()) != prefix) {
        return domain::Error::validation("link", "is not a kuwo song link");
    }
    auto rid = path.substr(prefix.size());
    if (!rid.empty() && rid.back() == '/') {
        rid.remove_suffix(1);
    }
    if (!is_valid_rid(rid)) {
        return domain::Error::validation("link", "does not name a kuwo song id");
    }
    return std::string{rid};
}

} // namespace music_service::providers::kuwo::parsing
