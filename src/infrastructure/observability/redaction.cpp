#include "infrastructure/observability/redaction.hpp"

#include <algorithm>
#include <array>

namespace music_dl::infrastructure::observability {

namespace {

char lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool contains_ci(std::string_view haystack, std::string_view needle) noexcept {
    if (needle.size() > haystack.size()) {
        return false;
    }
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (std::size_t k = 0; k < needle.size(); ++k) {
            if (lower(haystack[i + k]) != lower(needle[k])) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

bool starts_with_ci(std::string_view text, std::string_view prefix) noexcept {
    return text.size() >= prefix.size() && contains_ci(text.substr(0, prefix.size()), prefix);
}

bool is_url_key(std::string_view key) noexcept {
    return contains_ci(key, "url") || contains_ci(key, "uri") || contains_ci(key, "location") ||
           contains_ci(key, "referer");
}

bool is_space(char c) noexcept { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

} // namespace

bool is_sensitive_key(std::string_view key) noexcept {
    // Identifiers and versions (credential_id, token_id, credential_version) are meant
    // to be logged; they reference secrets without revealing them.
    const auto ends_with = [key](std::string_view suffix) {
        return key.size() >= suffix.size() &&
               contains_ci(key.substr(key.size() - suffix.size()), suffix);
    };
    if (ends_with("_id") || ends_with("_version")) {
        return false;
    }
    static constexpr std::array<std::string_view, 11> markers{
        "authorization", "cookie", "token",  "password",  "passwd", "secret",
        "api_key",       "apikey", "ticket", "signature", "session"};
    return std::any_of(markers.begin(), markers.end(),
                       [key](std::string_view marker) { return contains_ci(key, marker); });
}

std::string redact_url(std::string_view url) {
    auto end = url.find_first_of("?#");
    std::string_view base = url.substr(0, end);

    std::string result;
    const auto scheme_end = base.find("://");
    std::string_view rest = base;
    if (scheme_end != std::string_view::npos) {
        result.append(base.substr(0, scheme_end + 3));
        rest = base.substr(scheme_end + 3);
        const auto path_start = rest.find('/');
        auto authority = rest.substr(0, path_start);
        if (const auto at = authority.rfind('@'); at != std::string_view::npos) {
            authority = authority.substr(at + 1); // drop user info
        }
        result.append(authority);
        rest = path_start == std::string_view::npos ? std::string_view{} : rest.substr(path_start);
    }

    // /media/v1/tickets/<token> carries the ticket in the path.
    constexpr std::string_view ticket_marker = "/tickets/";
    if (const auto pos = rest.find(ticket_marker); pos != std::string_view::npos) {
        result.append(rest.substr(0, pos + ticket_marker.size()));
        result.append(redacted);
    } else {
        result.append(rest);
    }

    if (end != std::string_view::npos) {
        result.append(url[end] == '?' ? "?" : "#").append(redacted);
    }
    return result;
}

std::string redact_text(std::string_view text) {
    std::string result;
    std::size_t i = 0;
    while (i < text.size()) {
        const auto rest = text.substr(i);
        if (starts_with_ci(rest, "bearer ") || starts_with_ci(rest, "basic ")) {
            const auto scheme_len = rest.find(' ') + 1;
            result.append(rest.substr(0, scheme_len)).append(redacted);
            i += scheme_len;
            while (i < text.size() && !is_space(text[i])) {
                ++i;
            }
            continue;
        }
        if (starts_with_ci(rest, "http://") || starts_with_ci(rest, "https://")) {
            std::size_t len = 0;
            while (len < rest.size() && !is_space(rest[len]) && rest[len] != '"') {
                ++len;
            }
            result.append(redact_url(rest.substr(0, len)));
            i += len;
            continue;
        }
        result.push_back(text[i]);
        ++i;
    }
    return result;
}

std::string redact_field(std::string_view key, std::string_view value) {
    if (is_sensitive_key(key)) {
        return std::string{redacted};
    }
    if (is_url_key(key)) {
        return redact_url(value);
    }
    return redact_text(value);
}

} // namespace music_dl::infrastructure::observability
