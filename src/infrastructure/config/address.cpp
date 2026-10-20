#include "infrastructure/config/address.hpp"

#include <charconv>

namespace music_service::infrastructure::config::detail {

namespace {

std::optional<std::uint16_t> parse_port(std::string_view text) {
    if (text.empty() || text.size() > 5) {
        return std::nullopt;
    }
    unsigned value = 0;
    const auto* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    if (ec != std::errc{} || ptr != end || value > 65535) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(value);
}

bool valid_host_text(std::string_view host) {
    if (host.empty() || host.size() > 253) {
        return false;
    }
    for (const char c : host) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '-' || c == ':' || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

} // namespace

std::optional<HostPort> parse_listen_address(std::string_view text) {
    std::string_view host;
    std::string_view port;
    if (!text.empty() && text.front() == '[') {
        const auto close = text.find(']');
        if (close == std::string_view::npos || close + 1 >= text.size() || text[close + 1] != ':') {
            return std::nullopt;
        }
        host = text.substr(1, close - 1);
        port = text.substr(close + 2);
    } else {
        const auto colon = text.rfind(':');
        if (colon == std::string_view::npos || text.find(':') != colon) {
            return std::nullopt; // bare IPv6 must use brackets
        }
        host = text.substr(0, colon);
        port = text.substr(colon + 1);
    }
    const auto parsed_port = parse_port(port);
    if (!valid_host_text(host) || !parsed_port) {
        return std::nullopt;
    }
    return HostPort{std::string{host}, *parsed_port};
}

bool is_loopback_host(std::string_view host) {
    if (!host.empty() && host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);
    }
    return host == "localhost" || host == "::1" || host.rfind("127.", 0) == 0;
}

std::optional<BaseUrl> parse_base_url(std::string_view text) {
    BaseUrl url;
    std::string_view rest;
    if (text.rfind("http://", 0) == 0) {
        url.scheme = "http";
        rest = text.substr(7);
    } else if (text.rfind("https://", 0) == 0) {
        url.scheme = "https";
        rest = text.substr(8);
    } else {
        return std::nullopt;
    }
    if (rest.find_first_of("?#@") != std::string_view::npos) {
        return std::nullopt;
    }
    const auto authority = rest.substr(0, rest.find('/'));
    std::string_view host = authority;
    if (!authority.empty() && authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) {
            return std::nullopt;
        }
        host = authority.substr(1, close - 1);
        const auto after = authority.substr(close + 1);
        if (!after.empty() && (after.front() != ':' || !parse_port(after.substr(1)))) {
            return std::nullopt;
        }
    } else if (const auto colon = authority.find(':'); colon != std::string_view::npos) {
        host = authority.substr(0, colon);
        if (!parse_port(authority.substr(colon + 1))) {
            return std::nullopt;
        }
    }
    if (!valid_host_text(host)) {
        return std::nullopt;
    }
    url.host = std::string{host};
    return url;
}

} // namespace music_service::infrastructure::config::detail
