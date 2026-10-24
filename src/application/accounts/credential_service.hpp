#pragma once

#include "application/ports/providers/directory.hpp"
#include "application/ports/runtime/clock.hpp"
#include "application/ports/runtime/operation_context.hpp"
#include "application/ports/storage/credential_repository.hpp"
#include "application/ports/storage/secret_store.hpp"
#include "domain/identity/account.hpp"
#include "domain/result.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace music_service::application::accounts {

// auth_profile of anonymous bindings; never shared with application or user keys.
inline constexpr std::string_view anonymous_profile = "anonymous";

struct NewCredentialRequest {
    std::string owner_id;
    std::string source;
    std::string label;
    ports::CredentialKind kind{ports::CredentialKind::cookie};
    std::string auth_profile;
    ports::SecretBytes secret;
    std::optional<std::chrono::system_clock::time_point> expires_at;
};

// Same kind and profile only; changing either needs a new credential.
struct CredentialUpdate {
    std::optional<ports::SecretBytes> secret;
    std::optional<std::string> label;
    // Defaults to the version read just before the update.
    std::optional<std::uint64_t> expected_version;
};

struct VerifyResult {
    ports::CredentialRecord credential; // with the stored verification
    ports::CredentialVerification verification;
};

// Account selection for one operation. selection_version is set when the owner's
// default was consulted, so cursors and prepared intents can be bound to it.
struct ResolvedBinding {
    domain::AccountBinding binding;
    std::optional<std::uint64_t> selection_version;
};

// Identity key plus a read-only secret snapshot for exactly one operation. Never
// stored in tasks or returned to clients.
struct CredentialLease {
    domain::AuthIdentityKey identity;
    std::optional<ports::SecretBytes> secret; // absent for anonymous bindings
};

// Coordinates platform credentials (application/accounts): validation against the
// registered auth profiles, secret commit order, default selection, account binding
// and leases. Platform packages only verify; they never see repositories or a
// writable SecretStore. Cookie credentials and profiles with a registered structured
// secret validator use owner-scoped credentials. Global application bindings and
// OAuth refresh remain separate extensions.
class CredentialService {
public:
    CredentialService(std::shared_ptr<ports::CredentialRepository> credentials,
                      std::shared_ptr<ports::SecretStore> secrets,
                      std::shared_ptr<const ports::ProviderDirectory> providers,
                      std::shared_ptr<const ports::Clock> clock);

    // Writes the secret under a new reference, then the description. A failed insert
    // removes the new secret again. Never becomes a default by itself.
    domain::Result<ports::CredentialRecord> create(NewCredentialRequest request);
    domain::Result<ports::CredentialRecord> get(const std::string& owner_id, const std::string& id);
    domain::Result<std::vector<ports::CredentialRecord>> list(const std::string& owner_id,
                                                              const std::optional<std::string>& source);
    // A new secret goes to a new reference before the conditional commit; the old
    // reference is removed only after the commit succeeds. The version advances.
    domain::Result<ports::CredentialRecord> update(const std::string& owner_id, const std::string& id,
                                                   CredentialUpdate update);
    // Revokes and clears a default pointing here in one transaction, then removes the
    // secret. Repeating it succeeds. Does not touch accepted tasks or reselect.
    domain::Result<ports::CredentialRecord> revoke(const std::string& owner_id, const std::string& id);
    // Runs the platform verifier on the current version and stores the result only if
    // that version is still current (VERSION_CONFLICT otherwise).
    domain::Result<VerifyResult> verify(const std::string& owner_id, const std::string& id,
                                        const ports::OperationContext& context);

    // Reading and clearing work for sources no longer registered; setting needs the
    // source registered, the credential owned, of that source, not revoked and its
    // profile still registered with the same kind.
    domain::Result<ports::DefaultSelection> get_default(const std::string& owner_id,
                                                        const std::string& source);
    domain::Result<ports::DefaultSelection> set_default(const std::string& owner_id,
                                                        const std::string& source,
                                                        const std::string& credential_id);
    domain::Result<ports::DefaultSelection> clear_default(const std::string& owner_id,
                                                          const std::string& source);

    // Picks the account for a capability call: the explicit credential, else the
    // owner's default user credential when the capability can use its profile, else
    // anonymous when the capability allows running without credentials, else
    // CREDENTIALS_REQUIRED. Never picks another user credential implicitly.
    domain::Result<ResolvedBinding> resolve_binding(const std::string& owner_id, std::string_view source,
                                                    std::string_view capability,
                                                    const std::optional<std::string>& credential_id);

    // The latest valid version of exactly this binding. A revoked or deleted
    // credential is CREDENTIALS_REQUIRED; it never falls back to another account.
    domain::Result<CredentialLease> lease(const domain::AccountBinding& binding);

    // Removes stored secrets no credential references (left by failed commits or
    // cleanups). Run at startup before requests are served: a create in flight
    // would otherwise lose its new secret.
    domain::Result<std::size_t> reclaim_orphan_secrets(const std::vector<std::string>& reserved = {});

    domain::Result<ports::CredentialRecord> create_oauth(const std::string& owner, const std::string& label,
        const ports::OAuthApplication& application, ports::OAuthGrant grant,
        std::chrono::system_clock::time_point issued_at);
    // 只能由独立的有界 OAuth 执行器调用；网络期间不持有数据库事务。
    domain::Result<ports::CredentialRecord> refresh_oauth(const ports::CredentialRecord& candidate,
        const ports::OAuthApplication& application, const ports::OperationContext& context);

private:
    std::shared_ptr<ports::CredentialRepository> credentials_;
    std::shared_ptr<ports::SecretStore> secrets_;
    std::shared_ptr<const ports::ProviderDirectory> providers_;
    std::shared_ptr<const ports::Clock> clock_;
};

} // namespace music_service::application::accounts
