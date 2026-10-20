#include "application/transport/url.hpp"

#include <charconv>
#include <vector>

namespace music_service::application::transport {

namespace {

char lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string to_lower(std::string_view text) {
    std::string out(text);
    for (auto& c : out) {
        c = lower(c);
    }
    return out;
}

bool is_alpha(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

bool is_host_char(char c) noexcept { return is_alpha(c) || is_digit(c) || c == '-' || c == '.'; }

bool is_ipv6_char(char c) noexcept {
    return is_digit(c) || (lower(c) >= 'a' && lower(c) <= 'f') || c == ':' || c == '.';
}

// Printable ASCII except space; '#' never reaches here.
bool is_target_char(char c) noexcept { return c > 0x20 && c < 0x7F; }

domain::Error invalid(std::string reason) { return domain::Error::validation("url", std::move(reason)); }

// RFC 3986 section 5.2.4, applied to the path only.
std::string remove_dot_segments(std::string_view path) {
    std::vector<std::string_view> out;
    std::size_t i = 1; // path starts with '/'
    bool trailing_slash = false;
    while (i <= path.size()) {
        const auto end = path.find('/', i);
        const auto segment = path.substr(i, end == std::string_view::npos ? std::string_view::npos : end - i);
        const bool last = end == std::string_view::npos;
        if (segment == ".") {
            trailing_slash = last;
        } else if (segment == "..") {
            if (!out.empty()) {
                out.pop_back();
            }
            trailing_slash = last;
        } else {
            out.push_back(segment);
            trailing_slash = false;
        }
        if (last) {
            break;
        }
        i = end + 1;
    }
    std::string result;
    for (const auto segment : out) {
        result.push_back('/');
        result.append(segment);
    }
    if (trailing_slash || result.empty()) {
        result.push_back('/');
    }
    return result;
}

} // namespace

domain::Result<Url> Url::parse(std::string_view text) {
    if (text.empty() || text.size() > max_length) {
        return invalid("must be 1 to 8192 characters");
    }
    const auto colon = text.find("://");
    if (colon == std::string_view::npos) {
        return invalid("must be absolute");
    }
    Url url;
    url.scheme_ = to_lower(text.substr(0, colon));
    if (url.scheme_ != "http" && url.scheme_ != "https") {
        return invalid("scheme must be http or https");
    }
    auto rest = text.substr(colon + 3);
    if (const auto hash = rest.find('#'); hash != std::string_view::npos) {
        rest = rest.substr(0, hash);
    }
    const auto authority_end = rest.find_first_of("/?");
    const auto authority = rest.substr(0, authority_end);
    auto target = authority_end == std::string_view::npos ? std::string_view{} : rest.substr(authority_end);

    if (authority.find('@') != std::string_view::npos) {
        return invalid("must not contain user info");
    }
    std::string_view host;
    std::string_view port_text;
    if (!authority.empty() && authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) {
            return invalid("has an unterminated IPv6 address");
        }
        host = authority.substr(1, close - 1);
        const auto after = authority.substr(close + 1);
        if (!after.empty()) {
            if (after.front() != ':') {
                return invalid("has junk after the IPv6 address");
            }
            port_text = after.substr(1);
        }
        if (host.empty() || host.find(':') == std::string_view::npos) {
            return invalid("has an invalid IPv6 address");
        }
        for (const char c : host) {
            if (!is_ipv6_char(c)) {
                return invalid("has an invalid IPv6 address");
            }
        }
    } else {
        const auto port_colon = authority.rfind(':');
        host = authority.substr(0, port_colon);
        if (port_colon != std::string_view::npos) {
            port_text = authority.substr(port_colon + 1);
        }
        if (host.empty()) {
            return invalid("must have a host");
        }
        for (const char c : host) {
            if (!is_host_char(c)) {
                return invalid("host must be ASCII letters, digits, '-' and '.'");
            }
        }
    }
    url.host_ = to_lower(host);

    const std::uint16_t default_port = url.scheme_ == "https" ? 443 : 80;
    if (port_text.empty()) {
        url.port_ = default_port;
    } else {
        unsigned value = 0;
        const auto [end, ec] = std::from_chars(port_text.data(), port_text.data() + port_text.size(), value);
        if (ec != std::errc{} || end != port_text.data() + port_text.size() || value == 0 || value > 65535) {
            return invalid("has an invalid port");
        }
        url.port_ = static_cast<std::uint16_t>(value);
    }

    for (const char c : target) {
        if (!is_target_char(c)) {
            return invalid("target must be percent-encoded printable ASCII");
        }
    }
    url.target_ = std::string{target};
    if (url.target_.empty() || url.target_.front() == '?') {
        url.target_.insert(url.target_.begin(), '/');
    }
    return url;
}

std::string_view Url::path() const noexcept {
    const std::string_view target{target_};
    return target.substr(0, target.find('?'));
}

std::string Url::origin() const {
    std::string out = scheme_ + "://";
    if (host_.find(':') != std::string::npos) {
        out.append("[").append(host_).append("]");
    } else {
        out.append(host_);
    }
    const std::uint16_t default_port = scheme_ == "https" ? 443 : 80;
    if (port_ != default_port) {
        out.append(":").append(std::to_string(port_));
    }
    return out;
}

domain::Result<Url> Url::resolve(std::string_view reference) const {
    if (const auto hash = reference.find('#'); hash != std::string_view::npos) {
        reference = reference.substr(0, hash);
    }
    if (reference.empty()) {
        return *this;
    }
    // Absolute: a scheme before the first '/', '?' or ':' delimiter.
    if (const auto colon = reference.find("://"); colon != std::string_view::npos &&
                                                  reference.find_first_of("/?") > colon) {
        return parse(reference);
    }
    if (reference.substr(0, 2) == "//") {
        return parse(scheme_ + ":" + std::string{reference});
    }
    std::string target;
    if (reference.front() == '/') {
        target = std::string{reference};
    } else if (reference.front() == '?') {
        target = std::string{path()} + std::string{reference};
    } else {
        const auto base = path();
        target = std::string{base.substr(0, base.rfind('/') + 1)} + std::string{reference};
    }
    const auto query = target.find('?');
    auto normalized = remove_dot_segments(std::string_view{target}.substr(0, query));
    if (query != std::string::npos) {
        normalized.append(target.substr(query));
    }
    return parse(origin() + normalized);
}

std::string encode_query_component(std::string_view text) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3);
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if (is_alpha(c) || is_digit(c) || c == '-' || c == '.' || c == '_' || c == '~') {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(digits[u >> 4]);
            out.push_back(digits[u & 0x0F]);
        }
    }
    return out;
}

} // namespace music_service::application::transport
