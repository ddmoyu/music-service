#pragma once

#include "application/config/service_config.hpp"
#include "application/ports/providers/bundle.hpp"
#include "application/ports/providers/directory.hpp"
#include "domain/catalog/refs.hpp"
#include "domain/result.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <typeindex>
#include <vector>

namespace music_service::providers {

namespace ports = application::ports;

// Shared services handed to every factory.
struct ProviderServices {
    std::shared_ptr<ports::UpstreamHttp> upstream;
};

// The listing types live with the ProviderDirectory port.
using ports::AuthMethodView;
using ports::CapabilityView;
using ports::ProviderListPage;
using ports::ProviderListQuery;
using ports::ProviderView;

// Immutable after build(); safe to share across threads. Capability objects are created
// on first require() and then reused; listing or describing never creates them, starts
// threads or touches the network.
class ProviderRegistry final : public ports::ProviderDirectory {
public:
    static constexpr std::uint32_t max_list_limit = 100;

    // Validates every bundle against the contract and the providers configuration and
    // reports all problems at once (VALIDATION_FAILED, details keyed by
    // "providers/<source>/..."). Sources missing from the configuration use
    // ProviderConfig defaults; configured sources without a bundle are rejected.
    static domain::Result<std::shared_ptr<const ProviderRegistry>>
    build(std::vector<ports::ProviderBundle> bundles,
          const std::map<std::string, application::config::ProviderConfig>& config,
          ProviderServices services = {});

    ~ProviderRegistry() override;
    ProviderRegistry(const ProviderRegistry&) = delete;
    ProviderRegistry& operator=(const ProviderRegistry&) = delete;

    // UNKNOWN_PROVIDER for sources that are not registered.
    [[nodiscard]] domain::Result<ProviderView> describe(std::string_view source) const override;

    // Pages through registered sources in source order. A cursor from another principal
    // or filter is CURSOR_MISMATCH; one from an earlier registry (a restart) is
    // CURSOR_EXPIRED.
    [[nodiscard]] domain::Result<ProviderListPage> list(const ProviderListQuery& query) const override;

    [[nodiscard]] std::vector<domain::SourceId> sources() const;
    [[nodiscard]] std::size_t size() const noexcept;

    // The capability object for a source: UNKNOWN_PROVIDER, PROVIDER_DISABLED or
    // CAPABILITY_UNSUPPORTED when it is not available; a failing factory's error is
    // returned as is and retried on the next call.
    template <class Interface>
    [[nodiscard]] domain::Result<std::shared_ptr<Interface>> require(std::string_view source) const {
        auto erased = require_erased(source, ports::capability_traits<Interface>::key,
                                     std::type_index{typeid(Interface)});
        if (!erased) {
            return std::move(erased).error();
        }
        return std::static_pointer_cast<Interface>(std::move(erased).value());
    }

    // ProviderDirectory.
    [[nodiscard]] domain::Result<void> check_source(std::string_view source) const override;
    [[nodiscard]] domain::Result<ports::AuthProfileInfo>
    describe_auth_profile(std::string_view source, std::string_view auth_profile) const override;
    [[nodiscard]] domain::Result<ports::CapabilitySpec>
    describe_capability(std::string_view source, std::string_view key) const override;
    [[nodiscard]] domain::Result<std::shared_ptr<ports::ITrackSearch>>
    track_search(std::string_view source) const override {
        return require<ports::ITrackSearch>(source);
    }
    [[nodiscard]] domain::Result<std::shared_ptr<ports::ITrackDetail>>
    track_detail(std::string_view source) const override {
        return require<ports::ITrackDetail>(source);
    }
    [[nodiscard]] domain::Result<std::shared_ptr<ports::ILinkParser>>
    link_parser(std::string_view source) const override {
        return require<ports::ILinkParser>(source);
    }
    [[nodiscard]] domain::Result<std::shared_ptr<ports::IAssetResolver>>
    asset_resolver(std::string_view source) const override {
        return require<ports::IAssetResolver>(source);
    }
    [[nodiscard]] domain::Result<std::shared_ptr<void>> capability_object(std::string_view source,
                                                                          std::string_view key,
                                                                          std::type_index port) const override {
        return require_erased(source, key, port);
    }
    // The verifier bound to source + auth_profile + kind, created on first use.
    [[nodiscard]] domain::Result<std::shared_ptr<ports::ICredentialVerifier>>
    require_verifier(std::string_view source, std::string_view auth_profile,
                     ports::CredentialKind kind) const override;

    domain::Result<void> validate_secret(std::string_view source, std::string_view auth_profile,
                                         ports::CredentialKind kind, const ports::SecretBytes&) const override;

    // The plaintext HTTP endpoints the source registered, for its transport requests.
    // Empty for unknown sources.
    [[nodiscard]] std::vector<ports::PlaintextEndpoint> plaintext_endpoints(std::string_view source) const;

    // Capability and verifier objects created so far, for diagnostics and tests.
    [[nodiscard]] std::size_t instantiated() const noexcept;

    // Random per build; cursors carry it so they die with the process.
    [[nodiscard]] std::uint64_t generation() const noexcept;

private:
    struct Entry;

    ProviderRegistry();

    [[nodiscard]] domain::Result<std::shared_ptr<void>>
    require_erased(std::string_view source, std::string_view key, std::type_index type) const;
    [[nodiscard]] domain::Result<const Entry*> enabled_entry(std::string_view source) const;

    std::map<std::string, std::unique_ptr<Entry>, std::less<>> entries_;
    std::uint64_t generation_{0};
    mutable std::atomic<std::size_t> instantiated_{0};
};

} // namespace music_service::providers
