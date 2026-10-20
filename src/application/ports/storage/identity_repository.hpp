#pragma once

#include "domain/result.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace music_service::application::ports {

enum class PrincipalRole { user, admin };

struct PrincipalRecord {
    std::string id;
    std::string display_name;
    PrincipalRole role{PrincipalRole::user};
    bool enabled{true};
    std::chrono::system_clock::time_point created_at;
};

struct AccessTokenRecord {
    std::string id;
    std::string principal_id;
    std::vector<std::string> scopes;
    std::chrono::system_clock::time_point created_at;
    std::optional<std::chrono::system_clock::time_point> revoked_at;
};

// A token that may authenticate a request right now: not revoked, principal enabled.
struct AuthenticatedToken {
    AccessTokenRecord token;
    PrincipalRecord principal;
};

// Service principals and their access tokens. SQLite is the only source of truth;
// tokens are stored as hashes only.
class IdentityRepository {
public:
    virtual ~IdentityRepository() = default;

    // fixed_id is for well-known principals such as init's prn_local; otherwise a new
    // prn_ id is generated. An id already in use is CONFLICT.
    virtual domain::Result<PrincipalRecord>
    create_principal(std::string display_name, PrincipalRole role,
                     std::optional<std::string> fixed_id) = 0;
    virtual domain::Result<PrincipalRecord> get_principal(const std::string& id) = 0;
    virtual domain::Result<std::vector<PrincipalRecord>> list_principals() = 0;
    // Setting the current value again succeeds; enabling does not revive revoked tokens.
    virtual domain::Result<PrincipalRecord> set_principal_enabled(const std::string& id,
                                                                  bool enabled) = 0;

    // Only for an existing, enabled principal.
    virtual domain::Result<AccessTokenRecord> insert_token(const std::string& principal_id,
                                                           const std::string& token_hash,
                                                           std::vector<std::string> scopes) = 0;
    virtual domain::Result<std::optional<AuthenticatedToken>>
    authenticate(const std::string& token_hash) = 0;
    // Revoking an already revoked token succeeds; an unknown id is NOT_FOUND.
    virtual domain::Result<AccessTokenRecord> revoke_token(const std::string& token_id) = 0;
    virtual domain::Result<std::vector<AccessTokenRecord>>
    list_tokens(const std::string& principal_id) = 0;
};

} // namespace music_service::application::ports
