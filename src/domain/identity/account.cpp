#include "domain/identity/account.hpp"

#include "domain/detail/canonical.hpp"

#include <utility>

namespace music_service::domain {

namespace {

bool present_and_nonempty(const std::optional<std::string>& value) {
    return value.has_value() && !value->empty();
}

} // namespace

std::string_view to_string(AccountMode mode) noexcept {
    switch (mode) {
    case AccountMode::anonymous:
        return "anonymous";
    case AccountMode::application:
        return "application";
    case AccountMode::user:
        return "user";
    }
    return "anonymous";
}

AccountBinding::AccountBinding(std::string principal_id, SourceId source, AccountMode mode,
                               std::string auth_profile, std::optional<std::string> credential_id,
                               std::optional<std::string> app_profile_id)
    : principal_id_(std::move(principal_id)), source_(std::move(source)), mode_(mode),
      auth_profile_(std::move(auth_profile)), credential_id_(std::move(credential_id)),
      app_profile_id_(std::move(app_profile_id)) {}

Result<AccountBinding> AccountBinding::make(std::string principal_id, SourceId source,
                                            AccountMode mode, std::string auth_profile,
                                            std::optional<std::string> credential_id,
                                            std::optional<std::string> app_profile_id) {
    if (!is_valid_service_id("prn_", principal_id)) {
        return Error::validation("principal_id", "must be a prn_ service id");
    }
    // Every mode, anonymous included, names an explicit profile; no empty-string key.
    if (auth_profile.empty()) {
        return Error::validation("auth_profile", "must not be empty");
    }
    if (credential_id && !is_valid_service_id("cre_", *credential_id)) {
        return Error::validation("credential_id", "must be a cre_ service id");
    }
    if (app_profile_id && app_profile_id->empty()) {
        return Error::validation("app_profile_id", "must not be empty");
    }

    switch (mode) {
    case AccountMode::anonymous:
        if (credential_id || app_profile_id) {
            return Error::validation("mode", "anonymous binding must not reference credentials");
        }
        break;
    case AccountMode::application:
        if (!present_and_nonempty(app_profile_id) || credential_id) {
            return Error::validation("mode",
                                     "application binding requires app_profile_id only");
        }
        break;
    case AccountMode::user:
        if (!credential_id) {
            return Error::validation("credential_id", "user binding requires a credential");
        }
        break;
    }

    return AccountBinding{std::move(principal_id), std::move(source),
                          mode,                    std::move(auth_profile),
                          std::move(credential_id), std::move(app_profile_id)};
}

Result<AuthIdentityKey> AuthIdentityKey::make(AccountBinding binding, Versions versions) {
    const bool uses_credential = binding.credential_id().has_value();
    const bool uses_app = binding.app_profile_id().has_value();

    if (uses_credential != versions.credential_version.has_value()) {
        return Error::validation("credential_version",
                                 "must be present exactly when a credential is bound");
    }
    if (uses_app != versions.app_profile_version.has_value()) {
        return Error::validation("app_profile_version",
                                 "must be present exactly when an app profile is bound");
    }
    if (versions.app_token_id.has_value() != versions.app_token_version.has_value()) {
        return Error::validation("app_token", "token id and version must be set together");
    }
    if (versions.app_token_id && !uses_app) {
        return Error::validation("app_token", "an app token requires an app profile");
    }
    return AuthIdentityKey{std::move(binding), std::move(versions)};
}

std::string AuthIdentityKey::canonical() const {
    detail::CanonicalWriter writer;
    writer.field("prn", std::string_view{binding_.principal_id()})
        .field("src", std::string_view{binding_.source().value()})
        .field("mode", to_string(binding_.mode()))
        .field("profile", std::string_view{binding_.auth_profile()})
        .optional("cred", binding_.credential_id())
        .optional("app", binding_.app_profile_id())
        .optional("cred_v", versions_.credential_version)
        .optional("app_v", versions_.app_profile_version)
        .optional("tok", versions_.app_token_id)
        .optional("tok_v", versions_.app_token_version);
    return writer.str();
}

} // namespace music_service::domain
