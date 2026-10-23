#pragma once

#include "application/ports/providers/auth.hpp"
#include "application/ports/providers/bundle.hpp"
#include "domain/assets/quality.hpp"
#include "domain/catalog/refs.hpp"
#include "domain/result.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <typeindex>
#include <vector>

namespace music_service::application::ports {

// A registered auth profile without its factories.
struct AuthProfileInfo {
    std::string auth_profile;
    CredentialKind kind{CredentialKind::cookie};
    std::uint32_t version{1};
    std::vector<AuthField> input_fields;
    bool supports_verify{false};
};

// Secret-free description of one registered capability.
struct CapabilityView {
    CapabilitySpec spec;
    bool enabled{false}; // the source is enabled; supported is implied by being listed
};

// One implemented auth profile. supports_* come from actual bindings, never from kind.
struct AuthMethodView {
    std::string auth_profile;
    CredentialKind kind{CredentialKind::cookie};
    std::uint32_t version{1};
    std::vector<AuthField> input_fields;
    std::vector<std::string> scopes;
    bool supports_verify{false};
    bool supports_refresh{false};           // refresh ports arrive with T24
    bool supports_application_token{false}; // application token ports arrive with T24
};

struct ProviderView {
    domain::SourceId source;
    std::string name;
    bool enabled{false};
    std::string implementation_id;
    std::string implementation_version;
    ExecutionMode execution_mode{ExecutionMode::native};
    std::vector<CapabilityView> capabilities; // sorted by key
    std::vector<AuthMethodView> auth_methods; // sorted by auth_profile
    std::vector<domain::Quality> quality_hints;

    [[nodiscard]] bool supports(std::string_view key) const {
        for (const auto& capability : capabilities) {
            if (capability.spec.key == key) {
                return true;
            }
        }
        return false;
    }
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

// Read-only view of the registered platforms that application services need for
// account handling and the HTTP entry, without depending on the registry implementation.
class ProviderDirectory {
public:
    virtual ~ProviderDirectory() = default;

    // Secret-free description; UNKNOWN_PROVIDER for sources that are not registered.
    virtual domain::Result<ProviderView> describe(std::string_view source) const = 0;
    // Registered sources in source order, paged; never touches the network.
    virtual domain::Result<ProviderListPage> list(const ProviderListQuery& query) const = 0;

    // UNKNOWN_PROVIDER when the source is not registered. Disabled sources exist.
    virtual domain::Result<void> check_source(std::string_view source) const = 0;

    // The registered auth profile: UNKNOWN_PROVIDER for the source,
    // CAPABILITY_UNSUPPORTED when the profile is not registered.
    virtual domain::Result<AuthProfileInfo> describe_auth_profile(std::string_view source,
                                                                  std::string_view auth_profile) const = 0;

    // The declared capability of an enabled source: UNKNOWN_PROVIDER,
    // PROVIDER_DISABLED or CAPABILITY_UNSUPPORTED otherwise.
    virtual domain::Result<CapabilitySpec> describe_capability(std::string_view source,
                                                               std::string_view key) const = 0;

    // Capability objects of an enabled source, created on first use:
    // UNKNOWN_PROVIDER, PROVIDER_DISABLED or CAPABILITY_UNSUPPORTED otherwise.
    virtual domain::Result<std::shared_ptr<ITrackSearch>> track_search(std::string_view source) const = 0;
    virtual domain::Result<std::shared_ptr<ITrackDetail>> track_detail(std::string_view source) const = 0;
    virtual domain::Result<std::shared_ptr<ILinkParser>> link_parser(std::string_view source) const = 0;
    virtual domain::Result<std::shared_ptr<IAssetResolver>> asset_resolver(std::string_view source) const = 0;

    // Any capability port by its interface, with the same errors; the key comes from
    // capability_traits, so a key is only ever served by the port bound to it.
    template <class Interface>
    [[nodiscard]] domain::Result<std::shared_ptr<Interface>> capability(std::string_view source) const {
        auto erased = capability_object(source, capability_traits<Interface>::key, std::type_index{typeid(Interface)});
        if (!erased) {
            return std::move(erased).error();
        }
        return std::static_pointer_cast<Interface>(std::move(erased).value());
    }
    // Type-checked: INTERNAL if the key is bound to another port.
    virtual domain::Result<std::shared_ptr<void>> capability_object(std::string_view source, std::string_view key,
                                                                    std::type_index port) const = 0;

    virtual domain::Result<std::shared_ptr<ICredentialVerifier>>
    require_verifier(std::string_view source, std::string_view auth_profile,
                     CredentialKind kind) const = 0;

    virtual domain::Result<void> validate_secret(std::string_view, std::string_view, CredentialKind kind,
                                                 const SecretBytes&) const {
        if (kind == CredentialKind::cookie) return domain::ok();
        return domain::Error{domain::ErrorCode::capability_unsupported,
                             "this auth profile does not accept structured credentials", false, {}};
    }
};

} // namespace music_service::application::ports
