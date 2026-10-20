#include "application/transport/http_rules.hpp"

#include <algorithm>
#include <memory>
#include <utility>

namespace music_service::application::transport {

namespace {

bool iequals(std::string_view a, std::string_view b) noexcept {
    const auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [&](char x, char y) { return lower(x) == lower(y); });
}

// RFC 9110 token characters.
bool is_token_char(char c) noexcept {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        return true;
    }
    return std::string_view{"!#$%&'*+-.^_`|~"}.find(c) != std::string_view::npos;
}

bool valid_header(const ports::HttpHeader& header) noexcept {
    if (header.name.empty() || !std::all_of(header.name.begin(), header.name.end(), is_token_char)) {
        return false;
    }
    return std::none_of(header.value.begin(), header.value.end(), [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return (u < 0x20 && c != '\t') || u == 0x7F;
    });
}

bool is_body_header(std::string_view name) noexcept {
    return iequals(name, "Content-Type") || iequals(name, "Content-Length") ||
           iequals(name, "Content-Encoding");
}

} // namespace

bool is_sensitive_header(std::string_view name) noexcept {
    return iequals(name, "Cookie") || iequals(name, "Authorization") || iequals(name, "Proxy-Authorization");
}

bool is_redirect_status(int status) noexcept {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

const ports::PlaintextEndpoint* find_plaintext_endpoint(const ports::HttpRequest& request,
                                                        const Url& url) noexcept {
    if (url.is_https()) {
        return nullptr;
    }
    const auto path = url.path();
    const auto it = std::find_if(request.plaintext_endpoints.begin(), request.plaintext_endpoints.end(),
                                 [&](const ports::PlaintextEndpoint& endpoint) {
                                     return endpoint.host == url.host() && endpoint.port == url.port() &&
                                            endpoint.operation == request.operation &&
                                            !endpoint.path_prefix.empty() &&
                                            path.substr(0, endpoint.path_prefix.size()) == endpoint.path_prefix;
                                 });
    return it == request.plaintext_endpoints.end() ? nullptr : &*it;
}

domain::Result<Url> check_request(const ports::HttpRequest& request) {
    auto url = Url::parse(request.url);
    if (!url) {
        return std::move(url).error();
    }
    for (const auto& header : request.headers) {
        if (!valid_header(header)) {
            return domain::Error::validation("headers", "invalid header " + header.name);
        }
        if (is_sensitive_header(header.name)) {
            return domain::Error::validation("headers", header.name + " must be passed as a secret header");
        }
    }
    for (const auto& header : request.secret_headers) {
        if (!valid_header(header)) {
            return domain::Error::validation("secret_headers", "invalid header");
        }
    }
    if (request.method != ports::HttpMethod::post && !request.body.empty()) {
        return domain::Error::validation("body", "only POST requests carry a body");
    }
    if (!url->is_https()) {
        if (find_plaintext_endpoint(request, url.value()) == nullptr) {
            return domain::Error::validation("url", "plaintext HTTP endpoint is not registered");
        }
        if (!request.secret_headers.empty()) {
            return domain::Error::validation("secret_headers", "secrets are never sent over plaintext HTTP");
        }
    }
    return url;
}

domain::Error transport_error(std::string category, std::string message, bool retryable) {
    domain::Error error{domain::ErrorCode::upstream_failed, std::move(message), retryable, {}};
    error.details.push_back({"transport", std::move(category)});
    return error;
}

domain::Result<ports::HttpRequest> next_redirect(const ports::HttpRequest& current, const Url& current_url,
                                                 int status, std::string_view location,
                                                 std::uint32_t redirect_max) {
    if (current.redirects_followed + 1 > redirect_max) {
        return transport_error("too_many_redirects", "redirect limit reached", false);
    }
    auto target = current_url.resolve(location);
    if (!target) {
        return transport_error("redirect_rejected", "redirect target is not a valid URL", false);
    }

    ports::HttpRequest next = current;
    next.url = target->str();
    next.redirects_followed = current.redirects_followed + 1;
    const bool to_get = status == 303 || ((status == 301 || status == 302) && current.method == ports::HttpMethod::post);
    if (to_get) {
        if (next.method != ports::HttpMethod::head) {
            next.method = ports::HttpMethod::get;
        }
        next.body.clear();
        std::erase_if(next.headers, [](const ports::HttpHeader& h) { return is_body_header(h.name); });
    }
    if (target->origin() != current_url.origin()) {
        next.secret_headers.clear();
    }
    if (current_url.is_https() && !target->is_https()) {
        const auto* endpoint = find_plaintext_endpoint(next, target.value());
        if (endpoint == nullptr || !endpoint->downgrade_redirect) {
            return transport_error("redirect_rejected", "redirect from HTTPS to HTTP is not allowed", false);
        }
    }
    if (auto valid = check_request(next); !valid) {
        return transport_error("redirect_rejected", "redirect target is not allowed", false);
    }
    return next;
}

} // namespace music_service::application::transport
