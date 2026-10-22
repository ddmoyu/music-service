#include "providers/external/helper_protocol.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

using namespace music_service;
using namespace providers::external;
using namespace std::chrono_literals;

namespace {

const HelperScope scope{{"youtube"}, {"youtube.com", "googlevideo.com"}};
const auto now = std::chrono::system_clock::time_point{std::chrono::seconds{1'800'000'000}};

std::string result(const std::string& extra) {
    return R"({"url":"https://r1.googlevideo.com/videoplayback?id=1","extractor":"youtube","media_id":"dQw4w9WgXcQ")" +
           extra + "}";
}

} // namespace

TEST_CASE("hosts match a domain or its subdomains only", "[helper]") {
    CHECK(host_in_scope("youtube.com", scope.domains));
    CHECK(host_in_scope("www.youtube.com", scope.domains));
    CHECK(host_in_scope("WWW.YouTube.com", scope.domains));
    CHECK_FALSE(host_in_scope("notyoutube.com", scope.domains));
    CHECK_FALSE(host_in_scope("youtube.com.evil.net", scope.domains));
    CHECK_FALSE(host_in_scope("com", scope.domains));
}

TEST_CASE("only https links on declared hosts reach a helper", "[helper]") {
    CHECK(check_link("https://www.youtube.com/watch?v=1", scope));
    CHECK(check_link("http://www.youtube.com/watch?v=1", scope).error().code == domain::ErrorCode::validation_failed);
    CHECK(check_link("https://user@www.youtube.com/", scope).error().code == domain::ErrorCode::validation_failed);
    CHECK(check_link("https://example.org/", scope).error().code == domain::ErrorCode::capability_unsupported);
    CHECK(check_link("file:///etc/passwd", scope).error().code == domain::ErrorCode::validation_failed);
}

TEST_CASE("the hello carries the protocol and the tool identity", "[helper]") {
    auto hello = parse_hello(
        R"({"type":"hello","protocol":1,"implementation":"ytdlp-wrapper","implementation_version":"1.2.0","tool":"yt-dlp","tool_version":"2026.09.01","operations":["extract_audio"]})");
    REQUIRE(hello);
    CHECK(hello.value().protocol == 1);
    CHECK(hello.value().tool_version == "2026.09.01");
    CHECK(hello.value().operations == std::vector<std::string>{"extract_audio"});

    // Another protocol version parses far enough to be refused by version.
    auto future = parse_hello(R"({"type":"hello","protocol":2})");
    REQUIRE(future);
    CHECK(future.value().protocol == 2);

    CHECK_FALSE(parse_hello("not json"));
    CHECK_FALSE(parse_hello(R"({"type":"result"})"));
    CHECK_FALSE(parse_hello(R"({"type":"hello","protocol":"1"})"));
    CHECK_FALSE(parse_hello(R"({"type":7,"protocol":1})"));
    CHECK_FALSE(parse_hello(R"({"type":"hello","protocol":1,"implementation":"x"})"));
}

TEST_CASE("requests carry data only; the link never becomes an argument", "[helper]") {
    const auto line = encode_request("hlp_1", "extract_audio", {5000ms, 2, 1024}, scope,
                                     {"https://www.youtube.com/watch?v=\"; rm -rf /"});
    CHECK(line.back() == '\n');
    CHECK(line.find('\n') == line.size() - 1); // one line, whatever the input
    CHECK(line.find(R"("request_id":"hlp_1")") != std::string::npos);
    CHECK(line.find(R"("timeout_ms":5000)") != std::string::npos);
    CHECK(line.find(R"(\"; rm -rf /)") != std::string::npos);
    CHECK(encode_cancel("hlp_1") == "{\"request_id\":\"hlp_1\",\"type\":\"cancel\"}\n");
}

TEST_CASE("replies are matched to the request and helper errors are mapped", "[helper]") {
    auto ok = parse_reply(R"({"type":"result","request_id":"r1","result":{"url":"x"}})", "r1");
    REQUIRE(ok);
    REQUIRE(ok.value().result_json);

    CHECK_FALSE(parse_reply(R"({"type":"result","request_id":"r2","result":{}})", "r1"));
    CHECK_FALSE(parse_reply(R"({"type":"result","request_id":"r1","result":[]})", "r1"));
    CHECK_FALSE(parse_reply(R"({"type":"progress","request_id":"r1"})", "r1"));

    auto limited = parse_reply(
        R"({"type":"error","request_id":"r1","error":{"code":"rate_limited","message":"429 at https://x.example/?sig=SECRET","retry_after_seconds":30}})",
        "r1");
    REQUIRE(limited);
    REQUIRE(limited.value().error);
    CHECK(limited.value().error->code == domain::ErrorCode::upstream_rate_limited);
    CHECK(limited.value().error->retryable);
    CHECK(limited.value().retry_after == std::chrono::seconds{30});
    // The helper's message is scrubbed: no query strings reach the service's errors.
    CHECK(limited.value().error->message.find("SECRET") == std::string::npos);

    auto missing = parse_reply(R"({"type":"error","request_id":"r1","error":{"code":"not_found"}})", "r1");
    CHECK(missing.value().error->code == domain::ErrorCode::not_found);
    auto odd = parse_reply(R"({"type":"error","request_id":"r1","error":{"code":"\u0000weird","message":5}})", "r1");
    REQUIRE(odd);
    CHECK(odd.value().error->code == domain::ErrorCode::upstream_failed);
    CHECK_FALSE(odd.value().error->retryable);
}

TEST_CASE("extract results are re-checked against the scope", "[helper]") {
    auto good = parse_audio_location(
        result(R"(,"container":"M4A","bitrate_kbps":129,"expires_at":1800000600,"headers":{"User-Agent":"yt","Cookie":"a=b"})"),
        scope, now);
    REQUIRE(good);
    CHECK(good.value().container == "m4a");
    CHECK(good.value().expires_at == now + 600s);
    CHECK(good.value().headers.size() == 1);
    CHECK(good.value().secret_headers.size() == 1);

    // Far-future expiries are shortened, past ones refused.
    auto far = parse_audio_location(result(R"(,"expires_at":1900000000)"), scope, now);
    REQUIRE(far);
    CHECK(far.value().expires_at == now + max_helper_url_lifetime);
    CHECK_FALSE(parse_audio_location(result(R"(,"expires_at":1700000000)"), scope, now));

    const auto refused = [&](const std::string& json) {
        auto r = parse_audio_location(json, scope, now);
        return !r && r.error().code == domain::ErrorCode::upstream_failed && !r.error().retryable;
    };
    CHECK(refused(R"({"url":"https://evil.example/a","extractor":"youtube","media_id":"x"})"));
    CHECK(refused(R"({"url":"http://r1.googlevideo.com/a","extractor":"youtube","media_id":"x"})"));
    CHECK(refused(R"({"url":"https://r1.googlevideo.com/a","extractor":"generic","media_id":"x"})"));
    CHECK(refused(R"({"url":"https://r1.googlevideo.com/a","extractor":"vimeo","media_id":"x"})"));
    CHECK(refused(R"({"url":"https://r1.googlevideo.com/a","extractor":"youtube"})"));
    CHECK(refused(result(R"(,"headers":{"Host":"evil.example"})")));
    CHECK(refused(result(R"(,"headers":{"Range":"bytes=0-"})")));
    CHECK(refused(result(R"(,"headers":{"X-A":"line\r\nInjected: 1"})")));
    CHECK(refused(result(R"(,"headers":{"Bad Name":"v"})")));
    CHECK(refused(result(R"(,"bitrate_kbps":-1)")));
    CHECK(refused(result(R"(,"size_bytes":"12")")));
    CHECK(refused(result(R"(,"container":"m4a; rm")")));
}

TEST_CASE("diagnostics are bounded, single-line and scrubbed", "[helper]") {
    const auto clean = sanitize_diagnostic("line1\nline2 Bearer abc.def https://h.example/p?token=x", 200);
    CHECK(clean.find('\n') == std::string::npos);
    CHECK(clean.find("abc.def") == std::string::npos);
    CHECK(clean.find("token=x") == std::string::npos);
    // Cut on a character boundary: "é" is two bytes.
    CHECK(sanitize_diagnostic("aé", 2) == "a");
    CHECK(sanitize_diagnostic(std::string(1000, 'x'), 10).size() == 10);
}
