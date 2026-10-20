#pragma once

#include "application/ports/providers/auth.hpp"
#include "application/ports/providers/bundle.hpp"
#include "domain/result.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
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

// Read-only view of the registered platforms that application services need for
// account handling, without depending on the registry implementation.
class ProviderDirectory {
public:
    virtual ~ProviderDirectory() = default;

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

    virtual domain::Result<std::shared_ptr<ICredentialVerifier>>
    require_verifier(std::string_view source, std::string_view auth_profile,
                     CredentialKind kind) const = 0;
};

} // namespace music_service::application::ports
