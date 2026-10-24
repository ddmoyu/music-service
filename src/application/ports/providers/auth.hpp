#pragma once

#include "application/ports/providers/provider.hpp"
#include "application/ports/providers/membership.hpp"
#include "application/ports/storage/secret_store.hpp"
#include "domain/result.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace music_service::application::ports {

// Authorization protocol ports, bound per source + auth_profile + kind by a provider
// bundle. They receive a read-only snapshot and return evidence or candidates; they
// never get a repository or a writable SecretStore.

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
    MembershipInfo membership{};
};

class ICredentialVerifier {
public:
    virtual ~ICredentialVerifier() = default;
    // Bounded check; updates neither secrets nor tasks.
    virtual domain::Result<CredentialVerification> verify(const CredentialSnapshot& credential,
                                                          const OperationContext& context) = 0;
};

// Administrator-managed confidential application. The user cannot choose endpoints,
// redirect destinations or replace the service's client secret through the HTTP API.
struct OAuthApplication {
    std::string id;
    std::string source;
    std::string auth_profile;
    std::uint64_t version{1};
    std::string client_id;
    std::string redirect_uri;
    std::shared_ptr<const SecretBytes> client_secret;
};

struct OAuthGrant {
    SecretBytes secret;
    std::chrono::seconds expires_in{};
    std::vector<std::string> scopes;
};

// Native authorization-code flow. A grant is a candidate; only the account service
// may persist it. Refresh tokens must never be replayed after an uncertain exchange.
// Implementations share one instance per profile: keep per-user secrets local to
// each call and set allow_redirects=false for token exchange and refresh requests.
// This protocol port alone does not provide HTTP authorization sessions or refresh coordination.
class IOAuth2 {
public:
    virtual ~IOAuth2() = default;
    virtual domain::Result<std::string> authorize_url(std::string_view state,
                                                     std::string_view code_challenge) const = 0;
    virtual domain::Result<OAuthGrant> exchange(std::string_view principal_id,
        const SecretBytes& code, const SecretBytes& code_verifier, const OperationContext& context) = 0;
    virtual domain::Result<OAuthGrant> refresh(const CredentialSnapshot& credential,
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
