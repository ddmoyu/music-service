#pragma once

#include "application/ports/providers/auth.hpp"
#include "application/ports/providers/provider.hpp"
#include "domain/assets/quality.hpp"
#include "domain/catalog/refs.hpp"
#include "domain/result.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <typeindex>
#include <utility>
#include <vector>

namespace music_service::application::ports {

// What a platform package hands to the registry: a descriptor, typed capability
// factories, auth profiles and an options validator. Bundles are built by plain
// functions referenced from the generated build manifest; nothing registers itself
// through static constructors. A bundle holds no live objects: factories run on first
// use, never at startup.

// Bumped when the bundle contract changes incompatibly; the registry refuses others.
inline constexpr std::uint32_t provider_contract_version = 1;

enum class ExecutionMode { native, external };
enum class CredentialRequirement { none, optional, required };
enum class Pagination { none, cursor, page };
enum class CatalogKind { live, built_in, search_derived };

inline std::string_view to_string(ExecutionMode value) noexcept {
    return value == ExecutionMode::external ? "external" : "native";
}
inline std::string_view to_string(CredentialRequirement value) noexcept {
    switch (value) {
    case CredentialRequirement::optional:
        return "optional";
    case CredentialRequirement::required:
        return "required";
    case CredentialRequirement::none:
        break;
    }
    return "none";
}
inline std::string_view to_string(Pagination value) noexcept {
    switch (value) {
    case Pagination::cursor:
        return "cursor";
    case Pagination::page:
        return "page";
    case Pagination::none:
        break;
    }
    return "none";
}
inline std::string_view to_string(CatalogKind value) noexcept {
    switch (value) {
    case CatalogKind::built_in:
        return "built_in";
    case CatalogKind::search_derived:
        return "search_derived";
    case CatalogKind::live:
        break;
    }
    return "live";
}

// Declared support for one capability. Must be backed by a factory (or, for
// verify_credentials, by verifiers on the listed auth profiles).
struct CapabilitySpec {
    std::string key{};
    CredentialRequirement credentials{CredentialRequirement::none};
    // Profiles the capability can run with; required unless credentials is none.
    std::vector<std::string> auth_profiles{};
    std::vector<std::string> scopes{};
    std::vector<std::string> regions{};      // empty: no regional restriction is known
    std::vector<std::string> dependencies{}; // external tools; empty for native code
    Pagination pagination{Pagination::none};
    std::optional<CatalogKind> catalog_kind{};
};

// One input field of a credential form. Never carries a value.
struct AuthField {
    std::string name{};
    bool required{true};
    bool secret{true};
};

// Immutable per-source settings handed to factories.
struct ProviderSetup {
    domain::SourceId source;
    std::string options_json; // already accepted by the bundle's options validator
};

template <class Interface>
using ProviderFactory = std::function<domain::Result<std::shared_ptr<Interface>>(const ProviderSetup&)>;

struct AuthProfileSpec {
    std::string auth_profile{};
    CredentialKind kind{CredentialKind::cookie};
    std::uint32_t version{1};
    std::vector<AuthField> input_fields{};
    std::vector<std::string> scopes{};
    ProviderFactory<ICredentialVerifier> verifier{}; // empty: this profile cannot verify
};

// Type-erased capability factory that remembers the interface it produces, so a factory
// bound under the wrong key is caught at registration instead of at a call site.
class CapabilityBinding {
public:
    template <class Interface>
    static CapabilityBinding bind(std::string key, ProviderFactory<Interface> factory) {
        CapabilityBinding binding{std::move(key), std::type_index{typeid(Interface)}};
        if (factory) {
            binding.create_ = [f = std::move(factory)](const ProviderSetup& setup)
                -> domain::Result<std::shared_ptr<void>> {
                auto made = f(setup);
                if (!made) {
                    return std::move(made).error();
                }
                return std::shared_ptr<void>{std::move(made).value()};
            };
        }
        return binding;
    }

    // Binds under the interface's own key.
    template <class Interface> static CapabilityBinding bind(ProviderFactory<Interface> factory) {
        return bind<Interface>(std::string{capability_traits<Interface>::key}, std::move(factory));
    }

    [[nodiscard]] const std::string& key() const noexcept { return key_; }
    [[nodiscard]] std::type_index interface_type() const noexcept { return type_; }
    [[nodiscard]] bool has_factory() const noexcept { return static_cast<bool>(create_); }

    // The result points to an object of interface_type().
    [[nodiscard]] domain::Result<std::shared_ptr<void>> create(const ProviderSetup& setup) const {
        return create_(setup);
    }

private:
    CapabilityBinding(std::string key, std::type_index type) : key_(std::move(key)), type_(type) {}

    std::string key_;
    std::type_index type_;
    std::function<domain::Result<std::shared_ptr<void>>(const ProviderSetup&)> create_;
};

struct ProviderBundle {
    std::uint32_t contract_version{provider_contract_version};
    domain::SourceId source; // required
    std::string name{};
    std::string implementation_id{};
    std::string implementation_version{};
    ExecutionMode execution_mode{ExecutionMode::native};
    std::vector<CapabilitySpec> capabilities{};
    std::vector<CapabilityBinding> bindings{};
    std::vector<AuthProfileSpec> auth_profiles{};
    // Tiers the implementation may return; no promise for any account or track.
    std::vector<domain::Quality> quality_hints{};
    // Checks providers.<source>.options (a JSON object). Without a validator only an
    // empty object is accepted.
    std::function<domain::Result<void>(std::string_view options_json)> validate_options{};
};

} // namespace music_service::application::ports
