#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace music_dl::infrastructure::config::detail {

struct HostPort {
    std::string host;
    std::uint16_t port{0};
};

// "host:port" or "[ipv6]:port"; the port may be 0 (system-chosen).
std::optional<HostPort> parse_listen_address(std::string_view text);

// 127.0.0.0/8, ::1 and localhost.
bool is_loopback_host(std::string_view host);

struct BaseUrl {
    std::string scheme; // http | https
    std::string host;   // without brackets
};

// Absolute http(s) base URL without user info, query or fragment.
std::optional<BaseUrl> parse_base_url(std::string_view text);

} // namespace music_dl::infrastructure::config::detail
