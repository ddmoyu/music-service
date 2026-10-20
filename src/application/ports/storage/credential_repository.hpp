#pragma once

#include "domain/result.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace music_service::application::ports {

enum class CredentialStatus { active, invalid, revoked };

// Platform account description. The secret itself lives in the SecretStore under
// secret_ref; nothing here is secret.
struct CredentialRecord {
    std::string id;
    std::string owner_id;
    std::string source;
    std::string label;
    std::string kind;         // cookie | oauth2 | api_key
    std::string auth_profile;
    std::optional<std::string> secret_ref;
    std::uint64_t version{1};
    CredentialStatus status{CredentialStatus::active};
    std::optional<std::chrono::system_clock::time_point> expires_at;
    std::chrono::system_clock::time_point created_at;
};

struct NewCredential {
    std::string owner_id;
    std::string source;
    std::string label;
    std::string kind;
    std::string auth_profile;
    std::string secret_ref;
    std::optional<std::chrono::system_clock::time_point> expires_at;
};

struct SecretReplacement {
    CredentialRecord credential;
    std::optional<std::string> previous_secret_ref; // to delete after the commit
};

struct DefaultSelection {
    std::string source;
    std::optional<std::string> credential_id;
    std::uint64_t selection_version{0};
};

// Credentials and the per-owner default selection (data-models.md, 账号绑定与授权身份).
// Every operation is scoped by owner; another owner's credential is NOT_FOUND.
class CredentialRepository {
public:
    virtual ~CredentialRepository() = default;

    virtual domain::Result<CredentialRecord> insert(const NewCredential& credential) = 0;
    virtual domain::Result<CredentialRecord> get(const std::string& owner_id,
                                                 const std::string& id) = 0;
    virtual domain::Result<std::vector<CredentialRecord>>
    list(const std::string& owner_id, const std::optional<std::string>& source) = 0;

    // Conditional on expected_version and the credential not being revoked;
    // advances the version.
    virtual domain::Result<SecretReplacement>
    replace_secret(const std::string& owner_id, const std::string& id,
                   std::uint64_t expected_version, const std::string& new_secret_ref,
                   const std::optional<std::string>& new_label) = 0;

    // In one transaction: marks the credential revoked, advances its version, clears
    // any default pointing to it and advances that selection_version. Repeating the
    // call on a revoked credential succeeds without further changes.
    virtual domain::Result<SecretReplacement> revoke(const std::string& owner_id,
                                                     const std::string& id) = 0;

    virtual domain::Result<DefaultSelection> get_default(const std::string& owner_id,
                                                         const std::string& source) = 0;
    // The credential must belong to the owner (else NOT_FOUND), match the source and
    // not be revoked (else VALIDATION_FAILED). Setting the same value keeps the version.
    virtual domain::Result<DefaultSelection> set_default(const std::string& owner_id,
                                                         const std::string& source,
                                                         const std::string& credential_id) = 0;
    // Clearing an empty selection keeps its version.
    virtual domain::Result<DefaultSelection> clear_default(const std::string& owner_id,
                                                           const std::string& source) = 0;
};

} // namespace music_service::application::ports
