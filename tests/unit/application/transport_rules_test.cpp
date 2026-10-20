// URL handling and the per-hop request and redirect rules, without any network.

#include "../domain/test_support.hpp"

#include "application/transport/http_rules.hpp"
#include "application/transport/url.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace music_service;
using namespace music_service::application::transport;
namespace ports = application::ports;

namespace {

Url url(std::string_view text) { return test::unwrap(Url::parse(text)); }

std::string category(const domain::Error& error) {
    for (const auto& detail : error.details) {
        if (detail.field == "transport") {
            return detail.reason;
        }
    }
    return {};
}

ports::HttpRequest get(std::string target) {
    ports::HttpRequest request;
    request.url = std::move(target);
    request.operation = "search";
    return request;
}

} // namespace

TEST_CASE("URLs parse strictly and give normalized origins", "[transport]") {
    const auto plain = url("HTTPS://Music.Example.COM/a/b?q=1#frag");
    CHECK(plain.scheme() == "https");
    CHECK(plain.host() == "music.example.com");
    CHECK(plain.port() == 443);
    CHECK(plain.target() == "/a/b?q=1");
    CHECK(plain.path() == "/a/b");
    CHECK(plain.origin() == "https://music.example.com");

    CHECK(url("http://h:8080").target() == "/");
    CHECK(url("http://h:8080").origin() == "http://h:8080");
    CHECK(url("http://h:80/x").origin() == "http://h");
    CHECK(url("https://h?x=1").target() == "/?x=1");
    CHECK(url("https://[::1]:8443/x").origin() == "https://[::1]:8443");
    CHECK(url("https://[::1]:8443/x").host() == "::1");

    for (const auto* bad : {"", "ftp://h/x", "//h/x", "https://user:pw@h/x", "https:///x", "https://h:0/",
                            "https://h:65536/", "https://h:x/", "https://h_/x", "https://例子.cn/x",
                            "https://h/a b", "https://[::1/x", "https://[zz]/x", "https://h/\x01"}) {
        INFO(bad);
        CHECK(Url::parse(bad).error().code == domain::ErrorCode::validation_failed);
    }
}

TEST_CASE("Location references resolve against the current URL", "[transport]") {
    const auto base = url("https://a.example/dir/page?x=1");
    CHECK(test::unwrap(base.resolve("https://b.example/y")).str() == "https://b.example/y");
    CHECK(test::unwrap(base.resolve("//c.example/z")).str() == "https://c.example/z");
    CHECK(test::unwrap(base.resolve("/root?k=v")).str() == "https://a.example/root?k=v");
    CHECK(test::unwrap(base.resolve("?y=2")).str() == "https://a.example/dir/page?y=2");
    CHECK(test::unwrap(base.resolve("next")).str() == "https://a.example/dir/next");
    CHECK(test::unwrap(base.resolve("../up/./there")).str() == "https://a.example/up/there");
    CHECK(test::unwrap(base.resolve("../../../../top")).str() == "https://a.example/top");
    CHECK(test::unwrap(base.resolve("sub/")).str() == "https://a.example/dir/sub/");
    CHECK(test::unwrap(base.resolve("#only")).str() == base.str());
    CHECK_FALSE(base.resolve("javascript://x y").has_value());
}

TEST_CASE("requests keep secrets out of plain headers and plaintext HTTP", "[transport]") {
    CHECK(check_request(get("https://a.example/x")).has_value());

    auto cookie_in_headers = get("https://a.example/x");
    cookie_in_headers.headers.push_back({"cookie", "a=1"});
    CHECK(check_request(cookie_in_headers).error().code == domain::ErrorCode::validation_failed);

    auto split = get("https://a.example/x");
    split.headers.push_back({"X-Test", "a\r\nInjected: 1"});
    CHECK_FALSE(check_request(split).has_value());
    auto bad_name = get("https://a.example/x");
    bad_name.headers.push_back({"Bad Name", "v"});
    CHECK_FALSE(check_request(bad_name).has_value());

    auto get_with_body = get("https://a.example/x");
    get_with_body.body = "x";
    CHECK_FALSE(check_request(get_with_body).has_value());

    // Plaintext only for a registered host, port, path prefix and operation.
    auto plain = get("http://search.example/r.s?all=1");
    CHECK_FALSE(check_request(plain).has_value());
    plain.plaintext_endpoints.push_back({"search.example", 80, "/r.s", "search", false});
    CHECK(check_request(plain).has_value());
    plain.operation = "detail";
    CHECK_FALSE(check_request(plain).has_value());
    plain.operation = "search";
    plain.url = "http://search.example:8080/r.s";
    CHECK_FALSE(check_request(plain).has_value());
    plain.url = "http://search.example/other";
    CHECK_FALSE(check_request(plain).has_value());
    plain.url = "http://search.example.evil/r.s";
    CHECK_FALSE(check_request(plain).has_value());
    plain.url = "http://search.example/r.s";
    plain.secret_headers.push_back({"Cookie", "a=1"});
    CHECK(check_request(plain).error().details.at(0).field == "secret_headers");
}

TEST_CASE("redirects rewrite methods, drop secrets across origins and stay bounded", "[transport]") {
    auto post = get("https://a.example/form");
    post.method = ports::HttpMethod::post;
    post.body = "q=1";
    post.headers.push_back({"Content-Type", "application/x-www-form-urlencoded"});
    post.secret_headers.push_back({"Cookie", "s=1"});
    const auto from = url(post.url);

    const auto see_other = test::unwrap(next_redirect(post, from, 303, "/done", 5));
    CHECK(see_other.method == ports::HttpMethod::get);
    CHECK(see_other.body.empty());
    CHECK(see_other.headers.empty());
    CHECK(see_other.secret_headers.size() == 1); // same origin keeps the cookie
    CHECK(see_other.redirects_followed == 1);
    CHECK(see_other.url == "https://a.example/done");

    const auto temporary = test::unwrap(next_redirect(post, from, 307, "https://b.example/again", 5));
    CHECK(temporary.method == ports::HttpMethod::post);
    CHECK(temporary.body == "q=1");
    CHECK(temporary.secret_headers.empty()); // another origin never gets it

    const auto moved = test::unwrap(next_redirect(post, from, 302, "https://a.example:8443/x", 5));
    CHECK(moved.method == ports::HttpMethod::get);
    CHECK(moved.secret_headers.empty()); // another port is another origin

    auto counted = post;
    counted.redirects_followed = 5;
    CHECK(category(next_redirect(counted, from, 302, "/x", 5).error()) == "too_many_redirects");
    CHECK(category(next_redirect(post, from, 302, "ftp://x/y", 5).error()) == "redirect_rejected");

    // HTTPS to HTTP only for a registered download endpoint, never with secrets.
    auto download = get("https://cdn.example/a.mp3");
    download.operation = "download";
    download.secret_headers.push_back({"Cookie", "s=1"});
    const auto https = url(download.url);
    CHECK(category(next_redirect(download, https, 302, "http://plain.example/a.mp3", 5).error()) ==
          "redirect_rejected");
    download.plaintext_endpoints.push_back({"plain.example", 80, "/", "download", false});
    CHECK(category(next_redirect(download, https, 302, "http://plain.example/a.mp3", 5).error()) ==
          "redirect_rejected");
    download.plaintext_endpoints.back().downgrade_redirect = true;
    const auto downgraded = test::unwrap(next_redirect(download, https, 302, "http://plain.example/a.mp3", 5));
    CHECK(downgraded.url == "http://plain.example/a.mp3");
    CHECK(downgraded.secret_headers.empty());
}
