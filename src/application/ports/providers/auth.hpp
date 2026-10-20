#pragma once

#include "application/ports/providers/provider.hpp"
#include "application/ports/storage/secret_store.hpp"
#include "domain/result.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace music_service::application::ports {

// Authorization protocol ports, bound per source + auth_profile + kind by a provider
// bundle. They receive a read-only snapshot and return evidence or candidates; they
// never get a repository or a writable SecretStore. Refresh and application token
// ports are added with T24, so no bundle can claim them yet.

enum class CredentialKind { cookie, oauth2, api_key };

// Read-only authorization snapshot for one call. The secret stays owned by the caller.
struct CredentialSnapshot {
    domain::AuthIdentityKey identity;
    const SecretBytes& secret;
};

enum class VerificationStatus { valid, invalid, unknown };

struct CredentialVerification {
    VerificationStatus status{VerificationStatus::unknown};
    std::optional<std::string> account_hint; // non-secret display hint, e.g. a nickname
    std::vector<std::string> scopes;
    std::optional<std::chrono::system_clock::time_point> expires_at;
};

class ICredentialVerifier {
public:
    virtual ~ICredentialVerifier() = default;
    // Bounded check; updates neither secrets nor tasks.
    virtual domain::Result<CredentialVerification> verify(const CredentialSnapshot& credential,
                                                          const OperationContext& context) = 0;
};

inline std::string_view to_string(CredentialKind kind) noexcept {
    switch (kind) {
    case CredentialKind::cookie:
        return "cookie";
    case CredentialKind::oauth2:
        return "oauth2";
    case CredentialKind::api_key:
        return "api_key";
    }
    return "cookie";
}

} // namespace music_service::application::ports
