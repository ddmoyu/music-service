#pragma once

#include "domain/result.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace music_service::application::transport {

// An absolute http(s) URL as the upstream transport accepts it. Deliberately strict:
// no user info, ASCII host names only (IDNs must arrive already in punycode), and a
// target that is already percent-encoded printable ASCII. Fragments are dropped.
class Url {
public:
    static constexpr std::size_t max_length = 8192;

    static domain::Result<Url> parse(std::string_view text);

    [[nodiscard]] const std::string& scheme() const noexcept { return scheme_; }
    [[nodiscard]] const std::string& host() const noexcept { return host_; } // lowercase, IPv6 unbracketed
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] const std::string& target() const noexcept { return target_; } // path and query
    [[nodiscard]] bool is_https() const noexcept { return scheme_ == "https"; }

    // The path without the query.
    [[nodiscard]] std::string_view path() const noexcept;
    // scheme://host[:port], the port only when not the scheme's default.
    [[nodiscard]] std::string origin() const;
    [[nodiscard]] std::string str() const { return origin() + target_; }

    // Resolves a Location value against this URL: absolute, scheme-relative,
    // absolute-path, query-only or relative-path references; dot segments removed.
    [[nodiscard]] domain::Result<Url> resolve(std::string_view reference) const;

    friend bool operator==(const Url&, const Url&) = default;

private:
    Url() = default;

    std::string scheme_;
    std::string host_;
    std::uint16_t port_{0};
    std::string target_;
};

// Percent-encodes a query component as UTF-8 bytes; only RFC 3986 unreserved
// characters (A-Z a-z 0-9 - . _ ~) stay literal.
[[nodiscard]] std::string encode_query_component(std::string_view text);

} // namespace music_service::application::transport
