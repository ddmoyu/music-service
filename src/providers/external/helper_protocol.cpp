#include "providers/external/helper_protocol.hpp"

#include "application/transport/http_rules.hpp"
#include "application/transport/url.hpp"
#include "infrastructure/observability/redaction.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <set>

namespace music_service::providers::external {

namespace {

using json = nlohmann::json;
using transport_url = application::transport::Url;

constexpr std::size_t max_headers = 16;
constexpr std::size_t max_header_bytes = 8192;
constexpr std::size_t max_field = 256;

domain::Error protocol_error(std::string message) {
    // A helper that breaks its own protocol is not fixed by retrying the same build.
    return domain::Error{domain::ErrorCode::upstream_failed, "helper protocol violation: " + std::move(message), false,
                         {{"helper", "protocol"}}};
}

domain::Error scope_error(std::string message) {
    return domain::Error{domain::ErrorCode::upstream_failed, "helper result outside its scope: " + std::move(message),
                         false, {{"helper", "scope"}}};
}

std::string lower(std::string_view text) {
    std::string out{text};
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool printable(std::string_view text) {
    return std::all_of(text.begin(), text.end(), [](unsigned char c) { return c >= 0x20 && c != 0x7f; });
}

// A string member or empty, whatever type the helper sent.
std::string string_or_empty(const json& object, const char* name) {
    const auto it = object.find(name);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

std::optional<json> parse_object(std::string_view line) {
    auto value = json::parse(line.begin(), line.end(), nullptr, false);
    if (value.is_discarded() || !value.is_object()) {
        return std::nullopt;
    }
    return value;
}

// A required string member, non-empty, printable and at most max bytes.
std::optional<std::string> text_member(const json& object, const char* name, std::size_t max = max_field) {
    const auto it = object.find(name);
    if (it == object.end() || !it->is_string()) {
        return std::nullopt;
    }
    const auto& value = it->get_ref<const std::string&>();
    if (value.empty() || value.size() > max || !printable(value)) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::uint64_t> count_member(const json& object, const char* name) {
    const auto it = object.find(name);
    if (it == object.end() || it->is_null()) {
        return std::nullopt;
    }
    if (!it->is_number_unsigned()) {
        return std::uint64_t{0} - 1; // marks a present but malformed value
    }
    return it->get<std::uint64_t>();
}

bool token_name(std::string_view name) {
    static constexpr std::string_view extra = "!#$%&'*+-.^_`|~";
    return !name.empty() && name.size() <= 64 && std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::isalnum(c) != 0 || extra.find(static_cast<char>(c)) != std::string_view::npos;
    });
}

// Headers the service's transport decides itself; a helper may not set them.
bool transport_controlled(std::string_view name) {
    static const std::set<std::string, std::less<>> names{
        "host",       "content-length", "transfer-encoding", "connection", "keep-alive", "upgrade",
        "te",         "trailer",        "range",             "if-range",   "expect",     "proxy-connection",
        "proxy-authenticate"};
    return names.contains(lower(name));
}

bool short_token(std::string_view text, std::size_t max) {
    return !text.empty() && text.size() <= max && std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '.' || c == '-' || c == '_';
    });
}

} // namespace

bool host_in_scope(std::string_view host, const std::vector<std::string>& domains) {
    const auto h = lower(host);
    return std::any_of(domains.begin(), domains.end(), [&](const std::string& domain) {
        const auto d = lower(domain);
        return h == d || (h.size() > d.size() && h.ends_with(d) && h[h.size() - d.size() - 1] == '.');
    });
}

domain::Result<void> check_link(std::string_view link, const HelperScope& scope) {
    auto url = transport_url::parse(link);
    if (!url) {
        return domain::Error::validation("link", "must be an absolute https URL");
    }
    if (!url.value().is_https()) {
        return domain::Error::validation("link", "must use https");
    }
    if (!host_in_scope(url.value().host(), scope.domains)) {
        return domain::Error{domain::ErrorCode::capability_unsupported, "link host is not handled by this adapter",
                             false, {{"link", "host outside the adapter's domains"}}};
    }
    return domain::ok();
}

domain::Result<ports::HelperIdentity> parse_hello(std::string_view line) {
    const auto message = parse_object(line);
    if (!message || string_or_empty(*message, "type") != "hello") {
        return protocol_error("expected a hello message");
    }
    const auto protocol = count_member(*message, "protocol");
    if (!protocol || *protocol > 1000) {
        return protocol_error("hello has no protocol version");
    }
    ports::HelperIdentity identity;
    identity.protocol = static_cast<std::uint32_t>(*protocol);
    if (identity.protocol != ports::helper_protocol_version) {
        return identity; // the caller refuses it with the version in hand
    }
    const auto implementation = text_member(*message, "implementation");
    const auto implementation_version = text_member(*message, "implementation_version");
    const auto tool = text_member(*message, "tool");
    const auto tool_version = text_member(*message, "tool_version");
    const auto operations = message->find("operations");
    if (!implementation || !implementation_version || !tool || !tool_version || operations == message->end() ||
        !operations->is_array() || operations->size() > 32) {
        return protocol_error("hello is missing identity fields");
    }
    identity.implementation = *implementation;
    identity.implementation_version = *implementation_version;
    identity.tool = *tool;
    identity.tool_version = *tool_version;
    for (const auto& operation : *operations) {
        if (!operation.is_string() || !short_token(operation.get_ref<const std::string&>(), 64)) {
            return protocol_error("hello lists an invalid operation");
        }
        identity.operations.push_back(operation.get<std::string>());
    }
    return identity;
}

std::string encode_request(std::string_view request_id, std::string_view operation, const HelperBudget& budget,
                           const HelperScope& scope, const ports::HelperExtractRequest& input) {
    json message{
        {"type", "request"},
        {"protocol", ports::helper_protocol_version},
        {"request_id", request_id},
        {"operation", operation},
        {"budget",
         {{"timeout_ms", budget.timeout.count()},
          {"max_connections", budget.max_connections},
          {"max_response_bytes", budget.max_response_bytes}}},
        {"scope", {{"extractors", scope.extractors}, {"domains", scope.domains}}},
        {"input", {{"link", input.link}}},
    };
    return message.dump(-1, ' ', true, json::error_handler_t::replace) + "\n";
}

std::string encode_cancel(std::string_view request_id) {
    return json{{"type", "cancel"}, {"request_id", request_id}}.dump(-1, ' ', true, json::error_handler_t::replace) +
           "\n";
}

domain::Result<HelperReply> parse_reply(std::string_view line, std::string_view request_id) {
    const auto message = parse_object(line);
    if (!message) {
        return protocol_error("reply is not a JSON object");
    }
    const auto type = string_or_empty(*message, "type");
    if (string_or_empty(*message, "request_id") != request_id) {
        return protocol_error("reply names another request");
    }
    HelperReply reply;
    if (type == "result") {
        const auto it = message->find("result");
        if (it == message->end() || !it->is_object()) {
            return protocol_error("result is not an object");
        }
        reply.result_json = it->dump(-1, ' ', false, json::error_handler_t::replace);
        return reply;
    }
    if (type != "error") {
        return protocol_error("unexpected message type");
    }
    const auto it = message->find("error");
    if (it == message->end() || !it->is_object()) {
        return protocol_error("error is not an object");
    }
    const auto code = string_or_empty(*it, "code");
    const auto text = sanitize_diagnostic(string_or_empty(*it, "message"), max_helper_message);
    const auto message_text = "helper reported " + (short_token(code, 64) ? code : std::string{"an error"}) +
                              (text.empty() ? std::string{} : ": " + text);
    domain::Error error{domain::ErrorCode::upstream_failed, message_text, false, {{"helper", "error"}}};
    if (code == "unsupported") {
        error.code = domain::ErrorCode::capability_unsupported;
    } else if (code == "not_found") {
        error.code = domain::ErrorCode::not_found;
    } else if (code == "credentials_required") {
        error.code = domain::ErrorCode::credentials_required;
    } else if (code == "rate_limited") {
        error.code = domain::ErrorCode::upstream_rate_limited;
        error.retryable = true;
        if (const auto after = count_member(*it, "retry_after_seconds"); after && *after <= 86400) {
            reply.retry_after = std::chrono::seconds{static_cast<std::int64_t>(*after)};
        }
    } else if (code == "network" || code == "upstream_failed") {
        error.retryable = true;
    }
    reply.error = std::move(error);
    return reply;
}

domain::Result<ports::HelperAudioLocation> parse_audio_location(std::string_view result_json, const HelperScope& scope,
                                                                std::chrono::system_clock::time_point now) {
    const auto result = parse_object(result_json);
    if (!result) {
        return protocol_error("result is not an object");
    }
    ports::HelperAudioLocation location;
    const auto url_text = text_member(*result, "url", transport_url::max_length);
    if (!url_text) {
        return protocol_error("result has no url");
    }
    auto url = transport_url::parse(*url_text);
    if (!url || !url.value().is_https()) {
        return scope_error("url must be an absolute https URL");
    }
    if (!host_in_scope(url.value().host(), scope.domains)) {
        return scope_error("url host is not among the adapter's domains");
    }
    location.url = url.value().str();

    const auto extractor = text_member(*result, "extractor", 64);
    if (!extractor) {
        return protocol_error("result has no extractor");
    }
    if (lower(*extractor) == "generic" ||
        std::find(scope.extractors.begin(), scope.extractors.end(), *extractor) == scope.extractors.end()) {
        return scope_error("extractor " + sanitize_diagnostic(*extractor, 64) + " is not allowed");
    }
    location.extractor = *extractor;
    const auto media_id = text_member(*result, "media_id");
    if (!media_id) {
        return protocol_error("result has no media_id");
    }
    location.media_id = *media_id;

    if (const auto it = result->find("container"); it != result->end() && !it->is_null()) {
        if (!it->is_string() || !short_token(it->get_ref<const std::string&>(), 16)) {
            return protocol_error("container is malformed");
        }
        location.container = lower(it->get_ref<const std::string&>());
    }
    if (const auto it = result->find("codec"); it != result->end() && !it->is_null()) {
        if (!it->is_string() || !short_token(it->get_ref<const std::string&>(), 32)) {
            return protocol_error("codec is malformed");
        }
        location.codec = lower(it->get_ref<const std::string&>());
    }
    const auto malformed = std::uint64_t{0} - 1;
    const auto bitrate = count_member(*result, "bitrate_kbps");
    const auto size = count_member(*result, "size_bytes");
    const auto duration = count_member(*result, "duration_ms");
    const auto expires = count_member(*result, "expires_at");
    if (bitrate == malformed || size == malformed || duration == malformed || expires == malformed) {
        return protocol_error("a numeric field is malformed");
    }
    if (bitrate) {
        if (*bitrate == 0 || *bitrate > 100000) {
            return protocol_error("bitrate_kbps is out of range");
        }
        location.bitrate_kbps = static_cast<std::uint32_t>(*bitrate);
    }
    location.size_bytes = size;
    location.duration_ms = duration;
    if (expires) {
        // Unix seconds; a URL already expired is useless, a far-future one is not trusted.
        const auto at = std::chrono::system_clock::time_point{std::chrono::seconds{
            static_cast<std::int64_t>(std::min<std::uint64_t>(*expires, 253402300799ULL))}};
        if (at <= now) {
            return scope_error("url has already expired");
        }
        location.expires_at = std::min(at, now + max_helper_url_lifetime);
    }

    if (const auto it = result->find("headers"); it != result->end() && !it->is_null()) {
        if (!it->is_object() || it->size() > max_headers) {
            return protocol_error("headers must be an object of at most 16 entries");
        }
        std::size_t total = 0;
        for (const auto& [name, value] : it->items()) {
            if (!token_name(name) || !value.is_string()) {
                return protocol_error("a header is malformed");
            }
            const auto& text = value.get_ref<const std::string&>();
            total += name.size() + text.size();
            if (!printable(text) || total > max_header_bytes) {
                return protocol_error("a header value is malformed or too long");
            }
            if (transport_controlled(name)) {
                return scope_error("header " + name + " is set by the service");
            }
            ports::HttpHeader header{name, text};
            if (application::transport::is_sensitive_header(name)) {
                location.secret_headers.push_back(std::move(header));
            } else {
                location.headers.push_back(std::move(header));
            }
        }
    }
    return location;
}

std::string sanitize_diagnostic(std::string_view text, std::size_t max_bytes) {
    std::string clean;
    clean.reserve(std::min(text.size(), max_bytes));
    for (const unsigned char c : text) {
        if (clean.size() >= max_bytes) {
            // Never end inside a UTF-8 sequence.
            while (!clean.empty() && (static_cast<unsigned char>(clean.back()) & 0xC0) == 0x80) {
                clean.pop_back();
            }
            if (!clean.empty() && static_cast<unsigned char>(clean.back()) >= 0xC0) {
                clean.pop_back();
            }
            break;
        }
        // Control characters (log injection) become spaces; UTF-8 bytes pass.
        clean += (c < 0x20 || c == 0x7f) ? ' ' : static_cast<char>(c);
    }
    return infrastructure::observability::redact_text(clean);
}

} // namespace music_service::providers::external
