#include "providers/jamendo/parsing.hpp"
#include "application/transport/url.hpp"
#include <charconv>
#include <chrono>
#include <cstdio>
#include <set>

namespace music_service::providers::jamendo::parsing {
std::string text(const Json& object, const char* key, bool required) {
    if (!object.is_object()) throw std::invalid_argument{"object"};
    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) {
        if (required) throw std::invalid_argument{"missing text"};
        return {};
    }
    auto value = it->get<std::string>();
    if (value.size() > 65536 || !domain::is_valid_utf8(value) || value.find('\0') != value.npos ||
        (required && value.find_first_not_of(" \r\n\t") == value.npos))
        throw std::invalid_argument{"text"};
    return value;
}
std::string clean_text(std::string_view value) {
    // 网站名称和简介含 HTML 实体；逐次解码，避免 &amp;lt; 被重复解码。
    std::string out;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '<') {
            auto end = value.find('>', i + 1);
            if (end != value.npos && end - i < 256) { out += ' '; i = end; continue; }
        }
        if (value[i] != '&') { out += value[i]; continue; }
        const auto end = value.find(';', i + 1);
        if (end == value.npos || end - i > 12) { out += '&'; continue; }
        const auto entity = value.substr(i + 1, end - i - 1);
        std::uint32_t point{};
        if (entity == "amp") point = '&';
        else if (entity == "quot") point = '"';
        else if (entity == "apos") point = '\'';
        else if (entity == "lt") point = '<';
        else if (entity == "gt") point = '>';
        else if (entity == "nbsp") point = ' ';
        else if (entity.starts_with('#')) {
            auto digits = entity.substr(1);
            const bool hex = digits.starts_with('x') || digits.starts_with('X');
            if (hex) digits.remove_prefix(1);
            const auto [last, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), point, hex ? 16 : 10);
            if (ec != std::errc{} || last != digits.data() + digits.size()) point = 0;
        }
        if (point < 32 || point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff)) { out += '&'; continue; }
        if (point <= 0x7f) out += static_cast<char>(point);
        else if (point <= 0x7ff) {
            out += static_cast<char>(0xc0 | (point >> 6)); out += static_cast<char>(0x80 | (point & 63));
        } else if (point <= 0xffff) {
            out += static_cast<char>(0xe0 | (point >> 12)); out += static_cast<char>(0x80 | ((point >> 6) & 63));
            out += static_cast<char>(0x80 | (point & 63));
        } else {
            out += static_cast<char>(0xf0 | (point >> 18)); out += static_cast<char>(0x80 | ((point >> 12) & 63));
            out += static_cast<char>(0x80 | ((point >> 6) & 63)); out += static_cast<char>(0x80 | (point & 63));
        }
        i = end;
    }
    const auto start = out.find_first_not_of(" \r\n\t");
    if (start == out.npos) return {};
    return out.substr(start, out.find_last_not_of(" \r\n\t") - start + 1);
}
std::uint64_t number(const Json& object, const char* key) {
    const auto& value = object.at(key);
    if (value.is_number_unsigned()) return value.get<std::uint64_t>();
    if (value.is_number_integer() && value.get<std::int64_t>() >= 0)
        return static_cast<std::uint64_t>(value.get<std::int64_t>());
    if (value.is_string()) {
        auto s = value.get<std::string>(); std::uint64_t result{};
        const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), result);
        if (!s.empty() && ec == std::errc{} && end == s.data() + s.size()) return result;
    }
    throw std::invalid_argument{"number"};
}
std::string id(const Json& object, const char* key) {
    const auto& raw = object.at(key);
    const auto value = raw.is_string() ? raw.get<std::string>() : std::to_string(number(object, key));
    if (!decimal(value)) throw std::invalid_argument{"identifier"};
    return value;
}
domain::PlatformRef ref(std::string value) {
    if (!decimal(value)) throw std::invalid_argument{"identifier"};
    return {take(domain::SourceId::parse("jamendo")), take(domain::PlatformId::parse(value))};
}
bool available(const Json& object) {
    const auto it = object.find("status");
    if (it == object.end()) return true;
    if (it->is_object()) return it->at("available").get<bool>();
    const auto status = it->get<std::string>();
    return status == "visible" || status == "public";
}
domain::Artwork artwork(const Json& object) {
    auto cover = object.find("cover");
    if (cover == object.end() || cover->is_null()) return {};
    const auto big = cover->find("big");
    if (big == cover->end() || !big->is_object()) return {};
    auto value = text(*big, "size300");
    // 拼图歌单不能把第一首的专辑封面冒充整个歌单封面。
    if (value.empty()) return {};
    auto url = application::transport::Url::parse(value);
    if (!url || !url->is_https() || url->port() != 443 ||
        (url->host() != "usercontent.jamendo.com" && url->host() != "images.jamendo.com")) return {};
    return url->str();
}
namespace {
std::string title(const Json& object) {
    auto result = clean_text(text(object, "name", true));
    if (result.empty()) throw std::invalid_argument{"empty name"};
    return result;
}
std::optional<std::string> related_id(const Json& object, const char* key) {
    if (!object.contains(key) || object.at(key).is_null() || number(object, key) == 0) return {};
    return id(object, key);
}
std::vector<domain::ArtistSummary> artists(const Json& object) {
    const auto it = object.find("artist");
    if (it == object.end() || it->is_null()) return {};
    auto value = id(*it);
    if (auto parent = related_id(object, "artistId"); parent && *parent != value)
        throw std::invalid_argument{"artist identity"};
    return {{title(*it), ref(value)}};
}
std::optional<std::string> description(const Json& object) {
    auto it = object.find("description");
    if (it == object.end() || it->is_null()) return {};
    std::string value;
    if (it->is_string()) value = text(object, "description");
    else {
        for (const auto key : {"en", "fr", "de", "es", "it", "pt", "ru", "pl"}) {
            value = text(*it, key); if (!value.empty()) break;
        }
    }
    value = clean_text(value);
    return value.empty() ? std::nullopt : std::optional<std::string>{std::move(value)};
}
std::optional<std::string> date(const Json& object) {
    if (!object.contains("dateReleased")) return {};
    const auto seconds = number(object, "dateReleased");
    if (seconds == 0) return {};
    if (seconds > 253402300799ULL) throw std::invalid_argument{"release date"};
    const std::chrono::year_month_day ymd{std::chrono::floor<std::chrono::days>(
        std::chrono::sys_seconds{std::chrono::seconds{static_cast<std::int64_t>(seconds)}})};
    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), "%04d-%02u-%02u", static_cast<int>(ymd.year()),
                  static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()));
    return std::string{buffer};
}
}
Json members(const Json& object, const char* field, bool positions) {
    const auto& rows = object.at(field);
    if (!rows.is_array() || rows.size() > max_collection) throw std::invalid_argument{"collection limit"};
    Json result = Json::array();
    std::optional<std::uint64_t> previous;
    for (const auto& entry : rows) {
        Json member{{"id", id(entry)}};
        if (positions) {
            const auto position = number(entry, "position");
            if (position > max_collection || (previous && position <= *previous))
                throw std::invalid_argument{"collection positions"};
            member["position"] = position; previous = position;
        }
        result.push_back(std::move(member));
    }
    return result;
}
domain::Track track(const Json& object) {
    domain::Track result{ref(id(object)), title(object), artists(object)};
    if (const auto it = object.find("album"); it != object.end() && !it->is_null()) {
        const auto value = id(*it);
        if (auto parent = related_id(object, "albumId"); parent && *parent != value)
            throw std::invalid_argument{"album identity"};
        result.album = domain::AlbumSummary{title(*it), ref(value)};
    }
    if (object.contains("duration")) {
        const auto seconds = number(object, "duration");
        if (seconds > 86400) throw std::invalid_argument{"duration"};
        if (seconds) result.duration_ms = static_cast<std::int64_t>(seconds) * 1000;
    }
    result.artwork = artwork(object);
    return result;
}
domain::Album album(const Json& object) {
    domain::Album result{ref(id(object)), title(object), artists(object)};
    result.release_date = date(object); result.artwork = artwork(object); result.description = description(object);
    if (object.contains("tracks")) result.track_count = members(object, "tracks", true).size();
    return result;
}
domain::Artist artist(const Json& object) {
    domain::Artist result{ref(id(object)), title(object), artwork(object), description(object)};
    if (object.contains("location")) {
        auto country = text(object.at("location"), "country");
        if (!country.empty()) result.country = std::move(country);
    }
    if (object.contains("albums")) result.album_count = members(object, "albums", false).size();
    // mostpopular 仅为热门子集，不作全量歌曲数或 artist_tracks。
    return result;
}
domain::Playlist playlist(const Json& object) {
    domain::Playlist result{ref(id(object)), title(object)};
    result.description = description(object); result.artwork = artwork(object);
    if (object.contains("tracks")) result.track_count = members(object, "tracks", true).size();
    return result;
}
domain::Album album_detail(const Client& client, const ports::ProviderContext& context,
    std::string_view operation, const Json& object) {
    auto result = album(object);
    if (result.artists.empty()) if (auto value = related_id(object, "artistId")) {
        auto values = client.batch(operation, "artists", {*value}, context);
        if (auto it = values.find(*value); it != values.end() && available(it->second))
            result.artists = {{title(it->second), ref(*value)}};
    }
    return result;
}
std::vector<domain::Track> tracks(const Client& client, const ports::ProviderContext& context,
    std::string_view operation, const std::vector<Json>& rows, const Json& known_album) {
    std::set<std::string> album_ids, artist_ids;
    std::map<std::string, Json> albums;
    if (!known_album.is_null()) albums.emplace(id(known_album), known_album);
    for (const auto& row : rows) {
        if (!row.contains("album") || row.at("album").is_null())
            if (auto value = related_id(row, "albumId"); value && !albums.contains(*value)) album_ids.insert(*value);
        if (!row.contains("artist") || row.at("artist").is_null())
            if (auto value = related_id(row, "artistId")) artist_ids.insert(*value);
    }
    auto fetched = client.batch(operation, "albums", {album_ids.begin(), album_ids.end()}, context);
    albums.merge(fetched);
    auto performers = client.batch(operation, "artists", {artist_ids.begin(), artist_ids.end()}, context);
    std::vector<domain::Track> result;
    for (const auto& row : rows) {
        auto value = track(row);
        if (auto artist_id = related_id(row, "artistId"); artist_id && value.artists.empty()) {
            if (auto it = performers.find(*artist_id); it != performers.end() && available(it->second))
                value.artists = {{title(it->second), ref(*artist_id)}};
        }
        if (auto album_id = related_id(row, "albumId")) {
            if (auto it = albums.find(*album_id); it != albums.end() && available(it->second)) {
                const auto& parent = it->second;
                value.album = domain::AlbumSummary{title(parent), ref(*album_id)};
                if (!value.artwork) value.artwork = artwork(parent);
                const auto list = members(parent, "tracks", true);
                std::optional<std::uint32_t> position; bool duplicate = false;
                for (const auto& member : list) if (id(member) == value.ref.id.value()) {
                    if (position) duplicate = true;
                    const auto n = number(member, "position");
                    if (n) position = static_cast<std::uint32_t>(n);
                }
                if (position && !duplicate) { value.track_number = position; value.track_total = static_cast<std::uint32_t>(list.size()); }
            }
        }
        result.push_back(std::move(value));
    }
    return result;
}
}
