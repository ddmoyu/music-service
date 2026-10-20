#pragma once

#include "application/ports/identity/access_tokens.hpp"
#include "application/ports/storage/identity_repository.hpp"
#include "domain/result.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace music_service::application::accounts {

// The principal init creates.
inline constexpr std::string_view local_principal_id = "prn_local";

struct IssuedToken {
    ports::AccessTokenRecord token;
    std::filesystem::path delivered_to; // the only place the plaintext exists
};

// Offline principal and access token management (configuration-and-deployment.md,
// 本机身份管理命令). The CLI runs these only while holding the instance lock; the
// HTTP API never exposes them. SQLite is the only source of truth: delivery files,
// old outputs and configuration are never read back.
class IdentityAdminService {
public:
    IdentityAdminService(std::shared_ptr<ports::IdentityRepository> identities,
                         std::shared_ptr<ports::AccessTokenCodec> codec,
                         std::shared_ptr<ports::TokenDelivery> delivery);

    // Creates prn_local (admin) and a token with every API scope, delivered to
    // bootstrap_file. An instance that already has principals is CONFLICT, except an
    // interrupted init: prn_local alone without any token gets its token now.
    domain::Result<IssuedToken> initialize(const std::filesystem::path& bootstrap_file);

    domain::Result<ports::PrincipalRecord> create_principal(std::string display_name);
    domain::Result<std::vector<ports::PrincipalRecord>> list_principals();
    domain::Result<ports::PrincipalRecord> set_principal_enabled(const std::string& principal_id,
                                                                 bool enabled);

    // Delivers the plaintext to a new file first, then stores the hash. If storing
    // fails the file is removed again and no token is valid. An existing output file
    // fails the call before anything is stored.
    domain::Result<IssuedToken> issue_token(const std::string& principal_id,
                                            std::vector<std::string> scopes,
                                            const std::filesystem::path& output);
    domain::Result<std::vector<ports::AccessTokenRecord>> list_tokens(const std::string& principal_id);
    // Already revoked succeeds; other tokens are unaffected.
    domain::Result<ports::AccessTokenRecord> revoke_token(const std::string& token_id);

private:
    std::shared_ptr<ports::IdentityRepository> identities_;
    std::shared_ptr<ports::AccessTokenCodec> codec_;
    std::shared_ptr<ports::TokenDelivery> delivery_;
};

// Resolves a presented bearer token to its principal. Revoked tokens and disabled
// principals do not authenticate.
class AccessTokenAuthenticator {
public:
    static constexpr std::size_t max_token_bytes = 256;

    AccessTokenAuthenticator(std::shared_ptr<ports::IdentityRepository> identities,
                             std::shared_ptr<const ports::AccessTokenCodec> codec);

    // nullopt for an unknown, revoked or malformed token; errors are storage failures.
    domain::Result<std::optional<ports::AuthenticatedToken>> authenticate(std::string_view token) const;

private:
    std::shared_ptr<ports::IdentityRepository> identities_;
    std::shared_ptr<const ports::AccessTokenCodec> codec_;
};

} // namespace music_service::application::accounts
