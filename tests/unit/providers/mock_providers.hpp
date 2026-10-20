#pragma once

// Simulated platform bundles covering the capability combinations the registry must
// carry: catalog only, audio only with credentials, links only, and catalog plus audio.
// They answer from memory and never touch the network.

#include "../domain/test_support.hpp"

#include "application/ports/providers/bundle.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <utility>

namespace music_service::test {

namespace ports = application::ports;

// How many objects each mock factory has created, to prove lazy instantiation.
struct FactoryCounters {
    std::atomic<int> created{0};
};

class MockSearch final : public ports::ITrackSearch {
public:
    explicit MockSearch(domain::SourceId source) : source_(std::move(source)) {}
    domain::Result<ports::TrackSearchPage> search_tracks(const ports::ProviderContext&,
                                                         const ports::TrackSearchQuery& query) override {
        ports::TrackSearchPage result;
        domain::Track item{{source_, unwrap(domain::PlatformId::parse("t1"))}, query.text, {}, {}, {}, {}};
        result.page.items.push_back(std::move(item));
        result.page.page = {query.limit, 1, 1, false};
        return result;
    }

private:
    domain::SourceId source_;
};

class MockDetail final : public ports::ITrackDetail {
public:
    domain::Result<domain::Track> track_detail(const ports::ProviderContext&,
                                               const domain::TrackRef& track) override {
        return domain::Track{track, "detail", {}, {}, 1000, {}};
    }
};

class MockLinks final : public ports::ILinkParser {
public:
    explicit MockLinks(domain::SourceId source) : source_(std::move(source)) {}
    domain::Result<ports::ParsedLink> parse_link(std::string_view link) override {
        constexpr std::string_view prefix = "mock://track/";
        if (link.substr(0, prefix.size()) != prefix) {
            return domain::Error::validation("link", "not a recognized link");
        }
        auto id = domain::PlatformId::parse(link.substr(prefix.size()));
        if (!id) {
            return std::move(id).error();
        }
        return ports::ParsedLink{"track", {source_, std::move(id).value()}};
    }

private:
    domain::SourceId source_;
};

class MockResolver final : public ports::IAssetResolver {
public:
    domain::Result<ports::ResolvedAudio> resolve_audio(const ports::ProviderContext&,
                                                       const domain::TrackRef& track,
                                                       const domain::ResourceRequirement&) override {
        ports::ResolvedAudio audio{track, domain::Quality::standard, domain::Completeness::full,
                                   "mp3", 128000, "https://audio.invalid/a.mp3", {}, {}};
        return audio;
    }
};

class MockVerifier final : public ports::ICredentialVerifier {
public:
    domain::Result<ports::CredentialVerification> verify(const ports::CredentialSnapshot& credential,
                                                         const ports::OperationContext&) override {
        ports::CredentialVerification result;
        result.status = credential.secret.reveal() == "good" ? ports::VerificationStatus::valid
                                                           : ports::VerificationStatus::invalid;
        return result;
    }
};

// Creates T as a factory result for Interface.
template <class Interface, class T, class... Args>
domain::Result<std::shared_ptr<Interface>> make(Args&&... args) {
    return std::shared_ptr<Interface>{std::make_shared<T>(std::forward<Args>(args)...)};
}

template <class Interface, class Make>
ports::ProviderFactory<Interface> counted(std::shared_ptr<FactoryCounters> counters, Make make) {
    return [counters = std::move(counters), make = std::move(make)](const ports::ProviderSetup& setup)
               -> domain::Result<std::shared_ptr<Interface>> {
        ++counters->created;
        return std::shared_ptr<Interface>{make(setup)};
    };
}

inline ports::ProviderBundle base_bundle(std::string_view name) {
    return ports::ProviderBundle{
        .source = source(name),
        .name = std::string{name},
        .implementation_id = std::string{name} + "-mock",
        .implementation_version = "1.0.0",
    };
}

// Catalog-only source: search and detail, anonymous, no audio.
inline ports::ProviderBundle mock_catalog(std::string_view name,
                                          std::shared_ptr<FactoryCounters> counters =
                                              std::make_shared<FactoryCounters>()) {
    auto bundle = base_bundle(name);
    bundle.capabilities = {
        {.key = "search_tracks", .pagination = ports::Pagination::cursor,
         .catalog_kind = ports::CatalogKind::live},
        {.key = "track_detail"},
    };
    bundle.bindings = {
        ports::CapabilityBinding::bind<ports::ITrackSearch>(counted<ports::ITrackSearch>(
            counters, [](const ports::ProviderSetup& s) { return new MockSearch(s.source); })),
        ports::CapabilityBinding::bind<ports::ITrackDetail>(counted<ports::ITrackDetail>(
            counters, [](const ports::ProviderSetup&) { return new MockDetail(); })),
    };
    return bundle;
}

// Audio-only source: resolve with a user cookie, verifiable credentials.
inline ports::ProviderBundle mock_audio(std::string_view name,
                                        std::shared_ptr<FactoryCounters> counters =
                                            std::make_shared<FactoryCounters>()) {
    auto bundle = base_bundle(name);
    bundle.auth_profiles = {{
        .auth_profile = "web_cookie",
        .kind = ports::CredentialKind::cookie,
        .input_fields = {{"cookie", true, true}},
        .verifier = counted<ports::ICredentialVerifier>(
            counters, [](const ports::ProviderSetup&) { return new MockVerifier(); }),
    }};
    bundle.capabilities = {
        {.key = "resolve_audio", .credentials = ports::CredentialRequirement::required,
         .auth_profiles = {"web_cookie"}},
        {.key = "verify_credentials", .credentials = ports::CredentialRequirement::required,
         .auth_profiles = {"web_cookie"}},
    };
    bundle.bindings = {
        ports::CapabilityBinding::bind<ports::IAssetResolver>(counted<ports::IAssetResolver>(
            counters, [](const ports::ProviderSetup&) { return new MockResolver(); })),
    };
    bundle.quality_hints = {domain::Quality::standard, domain::Quality::high};
    return bundle;
}

// Link-parsing-only source.
inline ports::ProviderBundle mock_links(std::string_view name) {
    auto bundle = base_bundle(name);
    bundle.capabilities = {{.key = "parse_links"}};
    bundle.bindings = {ports::CapabilityBinding::bind<ports::ILinkParser>(
        [](const ports::ProviderSetup& s) -> domain::Result<std::shared_ptr<ports::ILinkParser>> {
            return make<ports::ILinkParser, MockLinks>(s.source);
        })};
    return bundle;
}

// Catalog and audio from one source, anonymous search and optional cookie for audio.
inline ports::ProviderBundle mock_combo(std::string_view name,
                                        std::shared_ptr<FactoryCounters> counters =
                                            std::make_shared<FactoryCounters>()) {
    auto bundle = mock_catalog(name, counters);
    bundle.auth_profiles = {{.auth_profile = "web_cookie", .kind = ports::CredentialKind::cookie}};
    bundle.capabilities.push_back({.key = "resolve_audio",
                                   .credentials = ports::CredentialRequirement::optional,
                                   .auth_profiles = {"web_cookie"}});
    bundle.bindings.push_back(ports::CapabilityBinding::bind<ports::IAssetResolver>(
        counted<ports::IAssetResolver>(counters,
                                       [](const ports::ProviderSetup&) { return new MockResolver(); })));
    return bundle;
}

} // namespace music_service::test
