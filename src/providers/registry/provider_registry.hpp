#pragma once

#include "application/config/service_config.hpp"
#include "application/ports/providers/bundle.hpp"
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

// Secret-free description of one registered capability.
struct CapabilityView {
    ports::CapabilitySpec spec;
    bool enabled{false}; // the source is enabled; supported is implied by being listed
};

// One implemented auth profile. supports_* come from actual bindings, never from kind.
struct AuthMethodView {
    std::string auth_profile;
    ports::CredentialKind kind{ports::CredentialKind::cookie};
    std::uint32_t version{1};
    std::vector<ports::AuthField> input_fields;
    std::vector<std::string> scopes;
    bool supports_verify{false};
    bool supports_refresh{false};           // refresh ports arrive with T24
    bool supports_application_token{false}; // application tokens arrive with T24
};

struct ProviderView {
    domain::SourceId source;
    std::string name;
    bool enabled{false};
    std::string implementation_id;
    std::string implementation_version;
    ports::ExecutionMode execution_mode{ports::ExecutionMode::native};
    std::vector<CapabilityView> capabilities; // sorted by key
    std::vector<AuthMethodView> auth_methods; // sorted by auth_profile
    std::vector<domain::Quality> quality_hints;

    [[nodiscard]] bool supports(std::string_view key) const;
};

struct ProviderListQuery {
    std::string principal_id;              // the cursor is bound to it
    std::optional<std::string> capability; // only sources that support this key
    std::optional<bool> enabled;
    std::uint32_t limit{50}; // 1..100
    std::optional<std::string> cursor;
};

struct ProviderListPage {
    std::vector<ProviderView> items;
    std::optional<std::string> next_cursor;
};

// Immutable after build(); safe to share across threads. Capability objects are created
// on first require() and then reused; listing or describing never creates them, starts
// threads or touches the network.
class ProviderRegistry {
public:
    static constexpr std::uint32_t max_list_limit = 100;

    // Validates every bundle against the contract and the providers configuration and
    // reports all problems at once (VALIDATION_FAILED, details keyed by
    // "providers/<source>/..."). Sources missing from the configuration use
    // ProviderConfig defaults; configured sources without a bundle are rejected.
    static domain::Result<std::shared_ptr<const ProviderRegistry>>
    build(std::vector<ports::ProviderBundle> bundles,
          const std::map<std::string, application::config::ProviderConfig>& config);

    ~ProviderRegistry();
    ProviderRegistry(const ProviderRegistry&) = delete;
    ProviderRegistry& operator=(const ProviderRegistry&) = delete;

    // UNKNOWN_PROVIDER for sources that are not registered.
    [[nodiscard]] domain::Result<ProviderView> describe(std::string_view source) const;

    // Pages through registered sources in source order. A cursor from another principal
    // or filter is CURSOR_MISMATCH; one from an earlier registry (a restart) is
    // CURSOR_EXPIRED.
    [[nodiscard]] domain::Result<ProviderListPage> list(const ProviderListQuery& query) const;

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

    // The verifier bound to source + auth_profile + kind.
    [[nodiscard]] domain::Result<std::shared_ptr<ports::ICredentialVerifier>>
    require_verifier(std::string_view source, std::string_view auth_profile,
                     ports::CredentialKind kind) const;

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
