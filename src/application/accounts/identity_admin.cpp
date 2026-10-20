#include "application/accounts/identity_admin.hpp"

#include "application/accounts/scopes.hpp"

#include <utility>

namespace music_service::application::accounts {

IdentityAdminService::IdentityAdminService(std::shared_ptr<ports::IdentityRepository> identities,
                                           std::shared_ptr<ports::AccessTokenCodec> codec,
                                           std::shared_ptr<ports::TokenDelivery> delivery)
    : identities_(std::move(identities)), codec_(std::move(codec)), delivery_(std::move(delivery)) {}

domain::Result<IssuedToken> IdentityAdminService::initialize(const std::filesystem::path& bootstrap_file) {
    auto principals = identities_->list_principals();
    if (!principals) {
        return std::move(principals).error();
    }
    const auto& existing = principals.value();
    bool resume = false;
    if (existing.size() == 1 && existing.front().id == local_principal_id) {
        // An init that stopped between creating prn_local and storing its token.
        auto tokens = identities_->list_tokens(existing.front().id);
        if (!tokens) {
            return std::move(tokens).error();
        }
        resume = tokens.value().empty();
    }
    if (!existing.empty() && !resume) {
        return domain::Error{domain::ErrorCode::conflict, "instance is already initialized", false, {}};
    }
    if (!resume) {
        auto created = identities_->create_principal("local", ports::PrincipalRole::admin,
                                                     std::string{local_principal_id});
        if (!created) {
            return std::move(created).error();
        }
    }
    return issue_token(std::string{local_principal_id},
                       std::vector<std::string>(all_scopes.begin(), all_scopes.end()), bootstrap_file);
}

domain::Result<ports::PrincipalRecord> IdentityAdminService::create_principal(std::string display_name) {
    return identities_->create_principal(std::move(display_name), ports::PrincipalRole::user,
                                         std::nullopt);
}

domain::Result<std::vector<ports::PrincipalRecord>> IdentityAdminService::list_principals() {
    return identities_->list_principals();
}

domain::Result<ports::PrincipalRecord>
IdentityAdminService::set_principal_enabled(const std::string& principal_id, bool enabled) {
    return identities_->set_principal_enabled(principal_id, enabled);
}

domain::Result<IssuedToken> IdentityAdminService::issue_token(const std::string& principal_id,
                                                              std::vector<std::string> scopes,
                                                              const std::filesystem::path& output) {
    if (auto valid = validate_scopes(scopes); !valid) {
        return valid.error();
    }
    auto principal = identities_->get_principal(principal_id);
    if (!principal) {
        return std::move(principal).error();
    }
    if (!principal->enabled) {
        return domain::Error::validation("principal_id", "principal is disabled");
    }
    auto token = codec_->generate();
    if (!token) {
        return std::move(token).error();
    }
    const auto hash = codec_->hash(token->reveal());
    if (auto delivered = delivery_->deliver(output, token.value()); !delivered) {
        return delivered.error();
    }
    auto stored = identities_->insert_token(principal_id, hash, std::move(scopes));
    if (!stored) {
        // The file holds a token that never became valid.
        delivery_->discard(output);
        return std::move(stored).error();
    }
    return IssuedToken{std::move(stored).value(), output};
}

domain::Result<std::vector<ports::AccessTokenRecord>>
IdentityAdminService::list_tokens(const std::string& principal_id) {
    if (auto principal = identities_->get_principal(principal_id); !principal) {
        return std::move(principal).error();
    }
    return identities_->list_tokens(principal_id);
}

domain::Result<ports::AccessTokenRecord> IdentityAdminService::revoke_token(const std::string& token_id) {
    return identities_->revoke_token(token_id);
}

AccessTokenAuthenticator::AccessTokenAuthenticator(std::shared_ptr<ports::IdentityRepository> identities,
                                                   std::shared_ptr<const ports::AccessTokenCodec> codec)
    : identities_(std::move(identities)), codec_(std::move(codec)) {}

domain::Result<std::optional<ports::AuthenticatedToken>>
AccessTokenAuthenticator::authenticate(std::string_view token) const {
    if (token.empty() || token.size() > max_token_bytes) {
        return std::optional<ports::AuthenticatedToken>{};
    }
    return identities_->authenticate(codec_->hash(token));
}

} // namespace music_service::application::accounts
