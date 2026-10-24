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

} // namespace

domain::Result<domain::TrackPay> parse_pay(const json& item) {
    const auto flag = [](const json& object, const char* key) -> domain::Result<std::optional<std::uint32_t>> {
        const auto it = object.find(key);
        if (it == object.end() || it->is_null()) return std::optional<std::uint32_t>{};
        if (it->is_boolean()) return std::optional<std::uint32_t>{it->get<bool>() ? 1U : 0U};
        if (it->is_number_unsigned() && it->get<std::uint64_t>() > UINT32_MAX)
            return protocol_error("pay flag out of range");
        const auto value = as_int(*it);
        if (!value || *value < 0 || *value > UINT32_MAX) return protocol_error("invalid pay flag");
        return *value <= 1 ? std::optional<std::uint32_t>{static_cast<std::uint32_t>(*value)} : std::nullopt;
    };
    domain::TrackPay out;
    auto play = flag(item, item.contains("tpay") ? "tpay" : "isListenFee");
    auto down = flag(item, "fpay");
    if (!play) return play.error();
    if (!down) return down.error();
    out.pay_play = *play; out.pay_down = *down;
    const auto info = item.find("payInfo");
    if (info != item.end() && !info->is_null()) {
        if (!info->is_object()) return protocol_error("invalid payInfo");
        const auto fee = info->find("feeType");
        if (fee != info->end() && !fee->is_null()) {
            if (!fee->is_object()) return protocol_error("invalid feeType");
            auto month = flag(*fee, "vip");
            if (!month) return month.error();
            out.pay_month = *month;
        }
    }
    return out;
}

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

std::vector<domain::ArtistSummary> parse_artists(std::string_view names, std::string_view ids) {
    return artists_from(names, ids);
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
        auto pay = parse_pay(item);
        if (!pay) return pay.error();
        track.pay = *pay;
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
    // Read through value_or: GCC's release build misreads the optional's comparisons.
    const auto type = data->contains("type") ? as_int((*data)["type"]) : std::nullopt;
    const bool typed = type.has_value();
    const auto type_value = type.value_or(0);
    evidence.substitute = !returned_rid || std::to_string(*returned_rid) != requested_rid || (typed && type_value != 0);
    evidence.regular = !evidence.substitute && typed && type_value == 0;
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

domain::Result<Link> parse_link(std::string_view link) {
    if (auto song = parse_track_link(link)) return Link{"track", *song};
    auto url = application::transport::Url::parse(link);
    if (!url || (url->host() != "www.kuwo.cn" && url->host() != "kuwo.cn") ||
        url->port() != (url->is_https() ? 443 : 80))
        return domain::Error::validation("link", "is not a supported kuwo link");
    auto path = url->path();
    if (path.ends_with('/')) path.remove_suffix(1);
    for (const auto* kind : {"playlist", "album"}) {
        const auto prefix = "/" + std::string{kind} + "_detail/";
        if (path.starts_with(prefix) && is_valid_rid(path.substr(prefix.size())))
            return Link{kind, std::string{path.substr(prefix.size())}};
    }
    constexpr std::string_view artist_prefix = "/singer_detail/";
    if (path.starts_with(artist_prefix)) {
        auto suffix = path.substr(artist_prefix.size());
        const auto slash = suffix.find('/');
        const auto id = suffix.substr(0, slash);
        const auto tab = slash == suffix.npos ? std::string_view{} : suffix.substr(slash);
        if (is_valid_rid(id) && (tab.empty() || tab == "/info" || tab == "/album" || tab == "/mv"))
            return Link{"artist", std::string{id}};
    }
    return domain::Error::validation("link", "is not a supported kuwo collection link");
}

} // namespace music_service::providers::kuwo::parsing
