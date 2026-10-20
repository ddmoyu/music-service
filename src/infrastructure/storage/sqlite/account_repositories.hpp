#pragma once

#include "application/ports/runtime/clock.hpp"
#include "application/ports/storage/credential_repository.hpp"
#include "application/ports/storage/destination_repository.hpp"
#include "application/ports/storage/identity_repository.hpp"
#include "infrastructure/observability/correlation.hpp"
#include "infrastructure/storage/sqlite/database.hpp"

#include <memory>

namespace music_service::infrastructure::storage::sqlite {

// Shared construction arguments for the SQLite repositories.
struct RepositoryContext {
    Database* database{nullptr};
    std::shared_ptr<const application::ports::Clock> clock;
    std::shared_ptr<observability::IdGenerator> ids;
};

class SqliteIdentityRepository final : public application::ports::IdentityRepository {
public:
    explicit SqliteIdentityRepository(RepositoryContext context) : ctx_(std::move(context)) {}

    domain::Result<application::ports::PrincipalRecord>
    create_principal(std::string display_name, application::ports::PrincipalRole role) override;
    domain::Result<application::ports::PrincipalRecord> get_principal(const std::string& id) override;
    domain::Result<std::vector<application::ports::PrincipalRecord>> list_principals() override;
    domain::Result<application::ports::PrincipalRecord> set_principal_enabled(const std::string& id,
                                                                              bool enabled) override;
    domain::Result<application::ports::AccessTokenRecord>
    insert_token(const std::string& principal_id, const std::string& token_hash,
                 std::vector<std::string> scopes) override;
    domain::Result<std::optional<application::ports::AuthenticatedToken>>
    authenticate(const std::string& token_hash) override;
    domain::Result<application::ports::AccessTokenRecord>
    revoke_token(const std::string& token_id) override;
    domain::Result<std::vector<application::ports::AccessTokenRecord>>
    list_tokens(const std::string& principal_id) override;

private:
    RepositoryContext ctx_;
};

class SqliteCredentialRepository final : public application::ports::CredentialRepository {
public:
    explicit SqliteCredentialRepository(RepositoryContext context) : ctx_(std::move(context)) {}

    domain::Result<application::ports::CredentialRecord>
    insert(const application::ports::NewCredential& credential) override;
    domain::Result<application::ports::CredentialRecord> get(const std::string& owner_id,
                                                              const std::string& id) override;
    domain::Result<std::vector<application::ports::CredentialRecord>>
    list(const std::string& owner_id, const std::optional<std::string>& source) override;
    domain::Result<application::ports::SecretReplacement>
    replace_secret(const std::string& owner_id, const std::string& id,
                   std::uint64_t expected_version, const std::string& new_secret_ref,
                   const std::optional<std::string>& new_label) override;
    domain::Result<application::ports::SecretReplacement> revoke(const std::string& owner_id,
                                                                 const std::string& id) override;
    domain::Result<application::ports::DefaultSelection>
    get_default(const std::string& owner_id, const std::string& source) override;
    domain::Result<application::ports::DefaultSelection>
    set_default(const std::string& owner_id, const std::string& source,
                const std::string& credential_id) override;
    domain::Result<application::ports::DefaultSelection>
    clear_default(const std::string& owner_id, const std::string& source) override;

private:
    RepositoryContext ctx_;
};

class SqliteDestinationRepository final : public application::ports::DestinationRepository {
public:
    explicit SqliteDestinationRepository(RepositoryContext context) : ctx_(std::move(context)) {}

    domain::Result<application::ports::DestinationBinding>
    bind(const std::string& id, const std::string& canonical_root) override;
    domain::Result<application::ports::DestinationBinding> get(const std::string& id) override;
    domain::Result<std::vector<application::ports::DestinationBinding>> list() override;
    domain::Result<bool> in_use(const std::string& id) override;

private:
    RepositoryContext ctx_;
};

} // namespace music_service::infrastructure::storage::sqlite
