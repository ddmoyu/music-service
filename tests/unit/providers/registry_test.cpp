// ProviderRegistry: bundle validation, typed on-demand capability access, auth profile
// bindings and the paged providers listing, with the Kuwo placeholder and simulated
// catalog / audio / link sources.

#include "mock_providers.hpp"

#include "infrastructure/config/config_loader.hpp"
#include "providers/kuwo/bundle.hpp"
#include "providers/registry/builtin_bundles.hpp"
#include "providers/registry/provider_registry.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace music_service;
using namespace music_service::test;
using application::config::ProviderConfig;
using providers::ProviderListQuery;
using providers::ProviderRegistry;

namespace {

using Config = std::map<std::string, ProviderConfig>;

std::shared_ptr<const ProviderRegistry> build(std::vector<ports::ProviderBundle> bundles,
                                              const Config& config = {}) {
    return unwrap(ProviderRegistry::build(std::move(bundles), config));
}

domain::Error build_error(std::vector<ports::ProviderBundle> bundles, const Config& config = {}) {
    auto result = ProviderRegistry::build(std::move(bundles), config);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == domain::ErrorCode::validation_failed);
    return std::move(result).error();
}

bool has_problem(const domain::Error& error, std::string_view field, std::string_view reason = {}) {
    return std::any_of(error.details.begin(), error.details.end(), [&](const auto& detail) {
        return detail.field == field && detail.reason.find(reason) != std::string::npos;
    });
}

ports::ProviderContext anonymous_context(std::string_view src) {
    return {{std::chrono::steady_clock::now() + std::chrono::seconds(5), {}},
            unwrap(domain::AuthIdentityKey::make(anonymous("prn_a", src), {})),
            std::nullopt};
}

// One code path for every source: what a search route does. Adding a source with an
// existing capability changes nothing here.
std::vector<std::string> search_titles(const ProviderRegistry& registry,
                                       const std::vector<std::string>& sources) {
    std::vector<std::string> out;
    for (const auto& src : sources) {
        auto search = registry.require<ports::ITrackSearch>(src);
        if (!search) {
            out.push_back(std::string{domain::to_string(search.error().code)});
            continue;
        }
        auto page = search.value()->search_tracks(anonymous_context(src), {"hello", 10, {}});
        REQUIRE(page.has_value());
        out.push_back(page->page.items.at(0).ref.source.value() + ":" + page->page.items.at(0).title);
    }
    return out;
}

std::vector<std::string> sources_of(const providers::ProviderListPage& page) {
    std::vector<std::string> out;
    for (const auto& item : page.items) {
        out.push_back(item.source.value());
    }
    return out;
}

} // namespace

TEST_CASE("the built-in manifest registers Kuwo", "[providers]") {
    const auto sources = providers::builtin_sources();
    CHECK(sources.count("kuwo") == 1);

    const auto registry = build(providers::builtin_bundles(), {{"kuwo", ProviderConfig{}}});
    const auto kuwo = unwrap(registry->describe("kuwo"));
    CHECK(kuwo.enabled);
    CHECK(kuwo.execution_mode == ports::ExecutionMode::native);
    CHECK(kuwo.supports("search_tracks"));
    CHECK(kuwo.supports("track_detail"));
    CHECK(kuwo.supports("parse_links"));
    CHECK(kuwo.supports("resolve_audio"));
    CHECK_FALSE(kuwo.supports("lyrics"));
    CHECK(kuwo.auth_methods.empty()); // standard quality needs no account
    // Networked capabilities need the upstream client; link parsing does not.
    CHECK(registry->require<ports::ITrackSearch>("kuwo").error().code == domain::ErrorCode::internal);
    CHECK(registry->require<ports::ILinkParser>("kuwo").has_value());

    // The same source list drives configuration validation.
    infrastructure::config::BuildCapabilities build_caps;
    build_caps.providers = sources;
    application::config::ServiceConfig config;
    config.data_dir = std::filesystem::temp_directory_path() / "music-service-registry-test";
    config.providers["kuwo"] = ProviderConfig{};
    config.catalog.default_sources = {"kuwo"};
    const auto valid = infrastructure::config::validate_service_config(config, build_caps);
    std::string problems;
    if (!valid) {
        for (const auto& detail : valid.error().details) {
            problems += detail.field + ": " + detail.reason + "; ";
        }
    }
    CAPTURE(problems);
    CHECK(problems.empty());
}

TEST_CASE("catalog, audio and link sources register side by side", "[providers]") {
    const auto registry = build({providers::kuwo::make_bundle(), mock_catalog("cat"), mock_audio("aud"),
                                 mock_links("lnk"), mock_combo("both")});
    CHECK(registry->size() == 5);

    const auto cat = unwrap(registry->describe("cat"));
    CHECK(cat.supports("search_tracks"));
    CHECK(cat.supports("track_detail"));
    CHECK_FALSE(cat.supports("resolve_audio"));
    CHECK(cat.capabilities.at(0).spec.pagination == ports::Pagination::cursor);
    CHECK(cat.capabilities.at(0).spec.catalog_kind == ports::CatalogKind::live);
    CHECK(cat.auth_methods.empty());

    const auto aud = unwrap(registry->describe("aud"));
    CHECK(aud.supports("resolve_audio"));
    CHECK(aud.supports("verify_credentials"));
    CHECK_FALSE(aud.supports("search_tracks"));
    REQUIRE(aud.auth_methods.size() == 1);
    CHECK(aud.auth_methods[0].supports_verify);
    CHECK_FALSE(aud.auth_methods[0].supports_refresh);
    CHECK_FALSE(aud.auth_methods[0].supports_application_token);
    CHECK(aud.quality_hints.size() == 2);

    const auto both = unwrap(registry->describe("both"));
    REQUIRE(both.auth_methods.size() == 1);
    CHECK_FALSE(both.auth_methods[0].supports_verify); // no verifier bound

    // Unsupported capabilities are reported, not guessed from the descriptor.
    CHECK(registry->require<ports::IAssetResolver>("cat").error().code ==
          domain::ErrorCode::capability_unsupported);
    CHECK(registry->require<ports::ITrackSearch>("aud").error().code ==
          domain::ErrorCode::capability_unsupported);
    CHECK(registry->require<ports::ILyrics>("both").error().code ==
          domain::ErrorCode::capability_unsupported);
    CHECK(registry->require<ports::ITrackSearch>("nope").error().code ==
          domain::ErrorCode::unknown_provider);
    CHECK(registry->describe("nope").error().code == domain::ErrorCode::unknown_provider);

    auto parser = unwrap(registry->require<ports::ILinkParser>("lnk"));
    const auto parsed = unwrap(parser->parse_link("mock://track/42"));
    CHECK(parsed.ref.source.value() == "lnk");
    CHECK(parsed.ref.id.value() == "42");
    CHECK_FALSE(parser->parse_link("https://elsewhere/42").has_value());

    auto resolver = unwrap(registry->require<ports::IAssetResolver>("both"));
    const auto audio = resolver->resolve_audio(anonymous_context("both"), track("both", "t1"), {});
    REQUIRE(audio.has_value());
    CHECK(audio->actual_track.source.value() == "both");

    // The same search code serves every source; sources without the capability fail alone.
    CHECK(search_titles(*registry, {"cat", "both", "aud", "lnk"}) ==
          std::vector<std::string>{"cat:hello", "both:hello", "CAPABILITY_UNSUPPORTED",
                                   "CAPABILITY_UNSUPPORTED"});
}

TEST_CASE("capability objects are created on first use and reused", "[providers]") {
    auto counters = std::make_shared<FactoryCounters>();
    const auto registry = build({mock_catalog("cat", counters), mock_audio("aud", counters)});

    // Building, describing and listing create nothing.
    (void)registry->describe("cat");
    (void)registry->list({"prn_a", {}, {}, 50, {}});
    CHECK(counters->created == 0);
    CHECK(registry->instantiated() == 0);

    auto first = unwrap(registry->require<ports::ITrackSearch>("cat"));
    auto second = unwrap(registry->require<ports::ITrackSearch>("cat"));
    CHECK(first == second);
    CHECK(counters->created == 1);

    // Only the requested capability of the requested source is created.
    (void)unwrap(registry->require<ports::ITrackDetail>("cat"));
    CHECK(counters->created == 2);
    CHECK(registry->instantiated() == 2);
}

TEST_CASE("concurrent first use creates one object", "[providers]") {
    auto counters = std::make_shared<FactoryCounters>();
    const auto registry = build({mock_catalog("cat", counters)});
    std::vector<std::shared_ptr<ports::ITrackSearch>> seen(8);
    {
        std::vector<std::jthread> threads;
        for (std::size_t i = 0; i < seen.size(); ++i) {
            threads.emplace_back([&, i] { seen[i] = registry->require<ports::ITrackSearch>("cat").value(); });
        }
    }
    CHECK(counters->created == 1);
    CHECK(std::all_of(seen.begin(), seen.end(), [&](const auto& p) { return p && p == seen[0]; }));
}

TEST_CASE("a failing factory is reported and retried", "[providers]") {
    int calls = 0;
    auto bundle = base_bundle("flaky");
    bundle.capabilities = {{.key = "search_tracks"}};
    bundle.bindings = {ports::CapabilityBinding::bind<ports::ITrackSearch>(
        [&calls](const ports::ProviderSetup& setup) -> domain::Result<std::shared_ptr<ports::ITrackSearch>> {
            if (++calls == 1) {
                return domain::Error{domain::ErrorCode::upstream_failed, "not ready", true, {}};
            }
            return make<ports::ITrackSearch, MockSearch>(setup.source);
        })};
    const auto registry = build({std::move(bundle)});
    CHECK(registry->require<ports::ITrackSearch>("flaky").error().code ==
          domain::ErrorCode::upstream_failed);
    CHECK(registry->require<ports::ITrackSearch>("flaky").has_value());
    CHECK(calls == 2);
}

TEST_CASE("verifiers are bound by source, auth profile and kind", "[providers]") {
    auto counters = std::make_shared<FactoryCounters>();
    const auto registry = build({mock_audio("aud", counters), mock_combo("both")});
    CHECK(counters->created == 0);

    auto verifier = unwrap(registry->require_verifier("aud", "web_cookie", ports::CredentialKind::cookie));
    const ports::SecretBytes good{"good"};
    const auto identity = unwrap(domain::AuthIdentityKey::make(user("prn_a", "aud", "cre_1"), {1, {}, {}, {}}));
    const auto verdict = verifier->verify({identity, good}, {std::chrono::steady_clock::now(), {}});
    REQUIRE(verdict.has_value());
    CHECK(verdict->status == ports::VerificationStatus::valid);
    CHECK(counters->created == 1);

    CHECK(registry->require_verifier("aud", "web_cookie", ports::CredentialKind::oauth2).error().code ==
          domain::ErrorCode::capability_unsupported);
    CHECK(registry->require_verifier("aud", "other", ports::CredentialKind::cookie).error().code ==
          domain::ErrorCode::capability_unsupported);
    CHECK(registry->require_verifier("both", "web_cookie", ports::CredentialKind::cookie).error().code ==
          domain::ErrorCode::capability_unsupported);
}

TEST_CASE("disabled sources are listed but not callable", "[providers]") {
    Config config;
    config["aud"].enabled = false;
    const auto registry = build({mock_catalog("cat"), mock_audio("aud")}, config);

    const auto aud = unwrap(registry->describe("aud"));
    CHECK_FALSE(aud.enabled);
    CHECK_FALSE(aud.capabilities.at(0).enabled);
    CHECK(registry->require<ports::IAssetResolver>("aud").error().code ==
          domain::ErrorCode::provider_disabled);
    CHECK(registry->require_verifier("aud", "web_cookie", ports::CredentialKind::cookie).error().code ==
          domain::ErrorCode::provider_disabled);

    const auto enabled_only = unwrap(registry->list({"prn_a", {}, true, 50, {}}));
    CHECK(sources_of(enabled_only) == std::vector<std::string>{"cat"});
    const auto disabled_only = unwrap(registry->list({"prn_a", {}, false, 50, {}}));
    CHECK(sources_of(disabled_only) == std::vector<std::string>{"aud"});
}

TEST_CASE("providers are listed in pages with bound cursors", "[providers]") {
    std::vector<ports::ProviderBundle> bundles;
    for (const auto* name : {"s01", "s02", "s03", "s04", "s05", "s06", "s07"}) {
        bundles.push_back(mock_catalog(name));
    }
    bundles.push_back(mock_audio("a01"));
    bundles.push_back(mock_audio("a02"));
    const auto registry = build(std::move(bundles));

    // All sources in source order, three per page.
    std::vector<std::string> all;
    ProviderListQuery query{"prn_a", {}, {}, 3, {}};
    int pages = 0;
    while (true) {
        const auto page = unwrap(registry->list(query));
        CHECK(page.items.size() <= 3);
        const auto names = sources_of(page);
        all.insert(all.end(), names.begin(), names.end());
        ++pages;
        if (!page.next_cursor) {
            break;
        }
        query.cursor = page.next_cursor;
    }
    CHECK(pages == 3);
    CHECK(all == std::vector<std::string>{"a01", "a02", "s01", "s02", "s03", "s04", "s05", "s06", "s07"});

    // Capability filter; an exactly full last page has no next cursor.
    const auto audio = unwrap(registry->list({"prn_a", "resolve_audio", {}, 2, {}}));
    CHECK(sources_of(audio) == std::vector<std::string>{"a01", "a02"});
    CHECK_FALSE(audio.next_cursor);
    CHECK(unwrap(registry->list({"prn_a", "lyrics", {}, 5, {}})).items.empty());

    // The cursor is bound to principal and filter but not to the page size.
    const auto first = unwrap(registry->list({"prn_a", "search_tracks", {}, 2, {}}));
    REQUIRE(first.next_cursor);
    const auto next = unwrap(registry->list({"prn_a", "search_tracks", {}, 4, first.next_cursor}));
    CHECK(sources_of(next) == std::vector<std::string>{"s03", "s04", "s05", "s06"});
    CHECK(registry->list({"prn_b", "search_tracks", {}, 2, first.next_cursor}).error().code ==
          domain::ErrorCode::cursor_mismatch);
    CHECK(registry->list({"prn_a", {}, {}, 2, first.next_cursor}).error().code ==
          domain::ErrorCode::cursor_mismatch);
    CHECK(registry->list({"prn_a", "search_tracks", true, 2, first.next_cursor}).error().code ==
          domain::ErrorCode::cursor_mismatch);
    CHECK(registry->list({"prn_a", "search_tracks", {}, 2, std::string{"garbage"}}).error().code ==
          domain::ErrorCode::cursor_mismatch);

    // A new registry (a restart) expires old cursors.
    const auto restarted = build({mock_catalog("s01"), mock_catalog("s02"), mock_catalog("s03")});
    CHECK(restarted->list({"prn_a", "search_tracks", {}, 2, first.next_cursor}).error().code ==
          domain::ErrorCode::cursor_expired);

    CHECK(registry->list({"prn_a", {}, {}, 0, {}}).error().code == domain::ErrorCode::validation_failed);
    CHECK(registry->list({"prn_a", {}, {}, 101, {}}).error().code == domain::ErrorCode::validation_failed);
    CHECK(registry->list({"prn_a", {}, {}, 100, {}}).has_value());
}

TEST_CASE("duplicate sources and unbuilt configured sources fail registration", "[providers]") {
    const auto duplicate = build_error({mock_catalog("cat"), mock_audio("cat")});
    CHECK(has_problem(duplicate, "providers/cat", "registered twice"));

    const auto unbuilt = build_error({mock_catalog("cat")}, {{"ghost", ProviderConfig{}}});
    CHECK(has_problem(unbuilt, "providers/ghost", "not built"));
}

TEST_CASE("declarations must match factory bindings", "[providers]") {
    auto undeclared = mock_catalog("a");
    undeclared.capabilities.pop_back(); // track_detail still bound
    auto unbound = mock_catalog("b");
    unbound.bindings.pop_back(); // track_detail still declared
    auto wrong_port = base_bundle("c");
    wrong_port.capabilities = {{.key = "search_tracks"}};
    wrong_port.bindings = {ports::CapabilityBinding::bind<ports::IAssetResolver>(
        "search_tracks", [](const ports::ProviderSetup&) -> domain::Result<std::shared_ptr<ports::IAssetResolver>> {
            return make<ports::IAssetResolver, MockResolver>();
        })};
    auto unknown_key = base_bundle("d");
    unknown_key.capabilities = {{.key = "teleport"}};
    auto empty_factory = base_bundle("e");
    empty_factory.capabilities = {{.key = "search_tracks"}};
    empty_factory.bindings = {ports::CapabilityBinding::bind<ports::ITrackSearch>({})};
    auto twice = mock_catalog("f");
    twice.capabilities.push_back({.key = "track_detail"});
    twice.bindings.push_back(ports::CapabilityBinding::bind<ports::ITrackDetail>(
        [](const ports::ProviderSetup&) -> domain::Result<std::shared_ptr<ports::ITrackDetail>> {
            return make<ports::ITrackDetail, MockDetail>();
        }));

    // Every problem is reported in one error.
    const auto error = build_error({std::move(undeclared), std::move(unbound), std::move(wrong_port),
                                    std::move(unknown_key), std::move(empty_factory), std::move(twice)});
    CHECK(has_problem(error, "providers/a/bindings/track_detail", "not declared"));
    CHECK(has_problem(error, "providers/b/capabilities/track_detail", "without a factory"));
    CHECK(has_problem(error, "providers/c/bindings/search_tracks", "expected the search_tracks port"));
    CHECK(has_problem(error, "providers/d/capabilities/teleport", "no port"));
    CHECK(has_problem(error, "providers/e/bindings/search_tracks", "empty factory"));
    CHECK(has_problem(error, "providers/f/capabilities/track_detail", "declared twice"));
    CHECK(has_problem(error, "providers/f/bindings/track_detail", "bound twice"));
}

TEST_CASE("auth profile bindings are validated", "[providers]") {
    auto duplicate = mock_audio("a");
    duplicate.auth_profiles.push_back({.auth_profile = "web_cookie", .kind = ports::CredentialKind::oauth2});
    auto missing_profile = mock_audio("b");
    missing_profile.capabilities.at(0).auth_profiles = {"app_token"};
    auto verify_without_verifier = mock_audio("c");
    verify_without_verifier.auth_profiles.at(0).verifier = {};
    auto verifier_undeclared = mock_audio("d");
    verifier_undeclared.capabilities.pop_back(); // drops verify_credentials
    auto credentials_without_profile = mock_audio("e");
    credentials_without_profile.capabilities.at(0).auth_profiles.clear();
    auto bad_name = base_bundle("f");
    bad_name.auth_profiles = {{.auth_profile = "Web Cookie"}};
    auto verify_bound = mock_audio("g");
    verify_bound.bindings.push_back(ports::CapabilityBinding::bind<ports::ITrackSearch>(
        "verify_credentials",
        [](const ports::ProviderSetup& s) -> domain::Result<std::shared_ptr<ports::ITrackSearch>> {
            return make<ports::ITrackSearch, MockSearch>(s.source);
        }));

    const auto error = build_error({std::move(duplicate), std::move(missing_profile),
                                    std::move(verify_without_verifier), std::move(verifier_undeclared),
                                    std::move(credentials_without_profile), std::move(bad_name),
                                    std::move(verify_bound)});
    CHECK(has_problem(error, "providers/a/auth_profiles/web_cookie", "registered twice"));
    CHECK(has_problem(error, "providers/b/capabilities/resolve_audio/auth_profiles", "unregistered profile app_token"));
    CHECK(has_problem(error, "providers/c/capabilities/verify_credentials/auth_profiles", "no verifier"));
    CHECK(has_problem(error, "providers/d/auth_profiles/web_cookie/verifier", "does not declare"));
    CHECK(has_problem(error, "providers/e/capabilities/resolve_audio/auth_profiles", "must name"));
    CHECK(has_problem(error, "providers/f/auth_profiles", "must match"));
    CHECK(has_problem(error, "providers/g/bindings/verify_credentials", "auth profile verifiers"));
}

TEST_CASE("contract version, implementation and options are checked", "[providers]") {
    auto old = mock_catalog("old");
    old.contract_version = ports::provider_contract_version + 1;
    auto unnamed = mock_catalog("unnamed");
    unnamed.implementation_id.clear();
    auto picky = mock_catalog("picky");
    picky.validate_options = [](std::string_view options) -> domain::Result<void> {
        if (options == R"({"region":"cn"})") {
            return domain::ok();
        }
        return domain::Error::validation("region", "must be cn");
    };

    Config config;
    config["old"] = ProviderConfig{};
    config["unnamed"].implementation = "external";
    config["picky"].options_json = R"({"region":"us"})";
    config["plain"].options_json = R"({"x":1})";
    const auto error = build_error(
        {std::move(old), std::move(unnamed), std::move(picky), mock_catalog("plain")}, config);
    CHECK(has_problem(error, "providers/old/contract_version"));
    CHECK(has_problem(error, "providers/unnamed/implementation_id"));
    CHECK(has_problem(error, "providers/unnamed/implementation", "this build provides native"));
    CHECK(has_problem(error, "providers/picky/options/region", "must be cn"));
    CHECK(has_problem(error, "providers/plain/options", "accepts no options"));

    // Valid options reach the factories.
    auto accepted = mock_catalog("picky");
    std::string seen;
    accepted.validate_options = [](std::string_view) { return domain::ok(); };
    accepted.bindings.at(0) = ports::CapabilityBinding::bind<ports::ITrackSearch>(
        [&seen](const ports::ProviderSetup& s) -> domain::Result<std::shared_ptr<ports::ITrackSearch>> {
            seen = s.options_json;
            return make<ports::ITrackSearch, MockSearch>(s.source);
        });
    Config ok_config;
    ok_config["picky"].options_json = R"({"region":"cn"})";
    const auto registry = build({std::move(accepted)}, ok_config);
    (void)unwrap(registry->require<ports::ITrackSearch>("picky"));
    CHECK(seen == R"({"region":"cn"})");
}

TEST_CASE("plaintext endpoints are validated and kept per source", "[providers]") {
    auto plain = mock_catalog("plain");
    plain.plaintext_endpoints = {{"search.example", 80, "/r.s", "search_tracks", false}};
    auto broken = mock_catalog("broken");
    broken.plaintext_endpoints = {{"Search.Example", 80, "r.s", "", false}};

    const auto error = build_error({mock_catalog("ok"), std::move(broken)});
    CHECK(has_problem(error, "providers/broken/plaintext_endpoints"));

    const auto registry = build({std::move(plain), mock_catalog("secure")});
    const auto endpoints = registry->plaintext_endpoints("plain");
    REQUIRE(endpoints.size() == 1);
    CHECK(endpoints[0].host == "search.example");
    CHECK(registry->plaintext_endpoints("secure").empty());
    CHECK(registry->plaintext_endpoints("ghost").empty());
}
