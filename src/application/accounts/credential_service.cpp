#include "application/accounts/credential_service.hpp"

#include "domain/catalog/refs.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace music_service::application::accounts {

namespace {

using domain::Error;
using domain::ErrorCode;

constexpr std::size_t max_label_bytes = 128;
// A lease retries when an update replaced the secret between reading the
// description and reading the secret.
constexpr int lease_attempts = 3;

std::optional<ports::CredentialKind> parse_kind(std::string_view text) {
    for (const auto kind : {ports::CredentialKind::cookie, ports::CredentialKind::oauth2,
                            ports::CredentialKind::api_key}) {
        if (ports::to_string(kind) == text) {
            return kind;
        }
    }
    return std::nullopt;
}

domain::Result<void> check_label(const std::string& label) {
    if (label.size() > max_label_bytes || !domain::is_valid_utf8(label)) {
        return Error::validation("label", "must be UTF-8 of at most 128 bytes");
    }
    return domain::ok();
}

// Cookie secrets become a request header: no control characters, so nothing can be
// smuggled into another header line.
domain::Result<void> check_secret(ports::CredentialKind kind, const ports::SecretBytes& secret) {
    if (kind != ports::CredentialKind::cookie) {
        return Error{ErrorCode::capability_unsupported,
                     "only cookie credentials are supported by this build", false, {}};
    }
    if (secret.empty()) {
        return Error::validation("secret", "must not be empty");
    }
    if (secret.size() > ports::SecretStore::max_secret_bytes) {
        return Error::validation("secret", "must be at most 64 KiB");
    }
    const auto bytes = secret.reveal();
    const bool control = std::any_of(bytes.begin(), bytes.end(), [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return u < 0x20 || u == 0x7F;
    });
    if (control) {
        return Error::validation("secret", "must not contain control characters");
    }
    return domain::ok();
}

Error credentials_required(std::string message) {
    return Error{ErrorCode::credentials_required, std::move(message), false, {}};
}

ports::VerifyOutcome to_outcome(ports::VerificationStatus status) {
    switch (status) {
    case ports::VerificationStatus::valid:
        return ports::VerifyOutcome::valid;
    case ports::VerificationStatus::invalid:
        return ports::VerifyOutcome::invalid;
    case ports::VerificationStatus::unknown:
        break;
    }
    return ports::VerifyOutcome::unknown;
}

} // namespace

CredentialService::CredentialService(std::shared_ptr<ports::CredentialRepository> credentials,
                                     std::shared_ptr<ports::SecretStore> secrets,
                                     std::shared_ptr<const ports::ProviderDirectory> providers,
                                     std::shared_ptr<const ports::Clock> clock)
    : credentials_(std::move(credentials)), secrets_(std::move(secrets)),
      providers_(std::move(providers)), clock_(std::move(clock)) {}

domain::Result<ports::CredentialRecord> CredentialService::create(NewCredentialRequest request) {
    if (auto profile = providers_->describe_auth_profile(request.source, request.auth_profile); !profile) {
        if (profile.error().code == ErrorCode::capability_unsupported) {
            return Error::validation("auth_profile", "is not registered for this source");
        }
        return std::move(profile).error();
    } else if (profile->kind != request.kind) {
        return Error::validation("kind", "does not match the auth profile");
    }
    if (auto valid = check_secret(request.kind, request.secret); !valid) {
        return valid.error();
    }
    if (auto valid = check_label(request.label); !valid) {
        return valid.error();
    }

    auto ref = secrets_->put(request.secret);
    if (!ref) {
        return std::move(ref).error();
    }
    auto stored = credentials_->insert(ports::NewCredential{
        request.owner_id, request.source, request.label, std::string{ports::to_string(request.kind)},
        request.auth_profile, ref.value(), request.expires_at});
    if (!stored) {
        // Best effort; a leftover is reclaimed as an orphan.
        (void)secrets_->remove(ref.value());
        return std::move(stored).error();
    }
    return stored;
}

domain::Result<ports::CredentialRecord> CredentialService::get(const std::string& owner_id,
                                                               const std::string& id) {
    return credentials_->get(owner_id, id);
}

domain::Result<std::vector<ports::CredentialRecord>>
CredentialService::list(const std::string& owner_id, const std::optional<std::string>& source) {
    return credentials_->list(owner_id, source);
}

domain::Result<ports::CredentialRecord> CredentialService::update(const std::string& owner_id,
                                                                  const std::string& id,
                                                                  CredentialUpdate update) {
    if (!update.secret && !update.label) {
        return Error::validation("credential", "nothing to update");
    }
    auto current = credentials_->get(owner_id, id);
    if (!current) {
        return std::move(current).error();
    }
    if (current->status == ports::CredentialStatus::revoked) {
        return Error::validation("credential", "is revoked");
    }
    if (update.label) {
        if (auto valid = check_label(*update.label); !valid) {
            return valid.error();
        }
    }
    std::optional<std::string> new_ref;
    if (update.secret) {
        const auto kind = parse_kind(current->kind);
        if (!kind) {
            return Error{ErrorCode::internal, "stored credential kind is unknown", false, {}};
        }
        if (auto valid = check_secret(*kind, *update.secret); !valid) {
            return valid.error();
        }
        auto ref = secrets_->put(*update.secret);
        if (!ref) {
            return std::move(ref).error();
        }
        new_ref = std::move(ref).value();
    }

    auto replaced = credentials_->replace_secret(owner_id, id,
                                                 update.expected_version.value_or(current->version),
                                                 new_ref, update.label);
    if (!replaced) {
        if (new_ref) {
            (void)secrets_->remove(*new_ref); // the old secret stays in use
        }
        return std::move(replaced).error();
    }
    if (replaced->previous_secret_ref) {
        (void)secrets_->remove(*replaced->previous_secret_ref);
    }
    return std::move(replaced->credential);
}

domain::Result<ports::CredentialRecord> CredentialService::revoke(const std::string& owner_id,
                                                                  const std::string& id) {
    auto revoked = credentials_->revoke(owner_id, id);
    if (!revoked) {
        return std::move(revoked).error();
    }
    // The database no longer leases it; a failed removal leaves an orphan to reclaim.
    if (revoked->previous_secret_ref) {
        (void)secrets_->remove(*revoked->previous_secret_ref);
    }
    return std::move(revoked->credential);
}

domain::Result<VerifyResult> CredentialService::verify(const std::string& owner_id, const std::string& id,
                                                       const ports::OperationContext& context) {
    auto record = credentials_->get(owner_id, id);
    if (!record) {
        return std::move(record).error();
    }
    if (record->status == ports::CredentialStatus::revoked || !record->secret_ref) {
        return Error::validation("credential", "is revoked");
    }
    const auto kind = parse_kind(record->kind);
    if (!kind) {
        return Error{ErrorCode::internal, "stored credential kind is unknown", false, {}};
    }
    auto verifier = providers_->require_verifier(record->source, record->auth_profile, *kind);
    if (!verifier) {
        return std::move(verifier).error();
    }
    auto source = domain::SourceId::parse(record->source);
    if (!source) {
        return std::move(source).error();
    }
    auto binding = domain::AccountBinding::make(owner_id, std::move(source).value(),
                                                domain::AccountMode::user, record->auth_profile, id,
                                                std::nullopt);
    if (!binding) {
        return std::move(binding).error();
    }
    auto identity = domain::AuthIdentityKey::make(std::move(binding).value(), {record->version, {}, {}, {}});
    if (!identity) {
        return std::move(identity).error();
    }
    auto secret = secrets_->get(*record->secret_ref);
    if (!secret) {
        return std::move(secret).error();
    }

    auto verification = verifier.value()->verify({identity.value(), secret.value()}, context);
    if (!verification) {
        return std::move(verification).error();
    }
    // Stored only for the version that was checked; a later secret is not judged by
    // an earlier result.
    auto stored = credentials_->record_verification(owner_id, id, record->version,
                                                    to_outcome(verification->status), clock_->now());
    if (!stored) {
        return std::move(stored).error();
    }
    return VerifyResult{std::move(stored).value(), std::move(verification).value()};
}

domain::Result<ports::DefaultSelection> CredentialService::get_default(const std::string& owner_id,
                                                                       const std::string& source) {
    if (auto valid = domain::SourceId::parse(source); !valid) {
        return std::move(valid).error();
    }
    return credentials_->get_default(owner_id, source);
}

domain::Result<ports::DefaultSelection> CredentialService::set_default(const std::string& owner_id,
                                                                       const std::string& source,
                                                                       const std::string& credential_id) {
    if (auto known = providers_->check_source(source); !known) {
        return known.error();
    }
    auto record = credentials_->get(owner_id, credential_id);
    if (!record) {
        return std::move(record).error();
    }
    if (record->source != source) {
        return Error::validation("credential_id", "belongs to another source");
    }
    if (record->status == ports::CredentialStatus::revoked) {
        return Error::validation("credential_id", "is revoked");
    }
    auto profile = providers_->describe_auth_profile(source, record->auth_profile);
    if (!profile || ports::to_string(profile->kind) != record->kind) {
        return Error::validation("credential_id", "its auth profile is no longer registered");
    }
    // The repository re-checks owner, source and revocation inside the transaction.
    return credentials_->set_default(owner_id, source, credential_id);
}

domain::Result<ports::DefaultSelection> CredentialService::clear_default(const std::string& owner_id,
                                                                         const std::string& source) {
    if (auto valid = domain::SourceId::parse(source); !valid) {
        return std::move(valid).error();
    }
    return credentials_->clear_default(owner_id, source);
}

domain::Result<ResolvedBinding> CredentialService::resolve_binding(const std::string& owner_id,
                                                                   std::string_view source_text,
                                                                   std::string_view capability,
                                                                   const std::optional<std::string>& credential_id) {
    auto spec = providers_->describe_capability(source_text, capability);
    if (!spec) {
        return std::move(spec).error();
    }
    auto source = domain::SourceId::parse(source_text);
    if (!source) {
        return std::move(source).error();
    }
    const auto allows = [&](const std::string& profile) {
        return std::find(spec->auth_profiles.begin(), spec->auth_profiles.end(), profile) !=
               spec->auth_profiles.end();
    };
    const auto user_binding = [&](const ports::CredentialRecord& record) {
        return domain::AccountBinding::make(owner_id, source.value(), domain::AccountMode::user,
                                            record.auth_profile, record.id, std::nullopt);
    };
    const auto anonymous_binding = [&] {
        return domain::AccountBinding::make(owner_id, source.value(), domain::AccountMode::anonymous,
                                            std::string{anonymous_profile}, std::nullopt, std::nullopt);
    };

    if (credential_id) {
        if (spec->credentials == ports::CredentialRequirement::none) {
            return Error::validation("credential_id", "this capability does not use credentials");
        }
        auto record = credentials_->get(owner_id, *credential_id);
        if (!record) {
            return std::move(record).error();
        }
        if (record->source != source_text) {
            return Error::validation("credential_id", "belongs to another source");
        }
        if (record->status == ports::CredentialStatus::revoked) {
            return Error::validation("credential_id", "is revoked");
        }
        if (!allows(record->auth_profile)) {
            return Error::validation("credential_id", "its auth profile cannot be used here");
        }
        auto binding = user_binding(record.value());
        if (!binding) {
            return std::move(binding).error();
        }
        return ResolvedBinding{std::move(binding).value(), std::nullopt};
    }

    if (spec->credentials == ports::CredentialRequirement::none) {
        auto binding = anonymous_binding();
        if (!binding) {
            return std::move(binding).error();
        }
        return ResolvedBinding{std::move(binding).value(), std::nullopt};
    }

    auto selection = credentials_->get_default(owner_id, std::string{source_text});
    if (!selection) {
        return std::move(selection).error();
    }
    if (selection->credential_id) {
        auto record = credentials_->get(owner_id, *selection->credential_id);
        if (!record) {
            return std::move(record).error();
        }
        if (record->status != ports::CredentialStatus::revoked && allows(record->auth_profile)) {
            auto binding = user_binding(record.value());
            if (!binding) {
                return std::move(binding).error();
            }
            return ResolvedBinding{std::move(binding).value(), selection->selection_version};
        }
    }
    if (spec->credentials == ports::CredentialRequirement::optional) {
        auto binding = anonymous_binding();
        if (!binding) {
            return std::move(binding).error();
        }
        return ResolvedBinding{std::move(binding).value(), selection->selection_version};
    }
    return credentials_required("no usable default credential for this source");
}

domain::Result<CredentialLease> CredentialService::lease(const domain::AccountBinding& binding) {
    switch (binding.mode()) {
    case domain::AccountMode::anonymous: {
        auto identity = domain::AuthIdentityKey::make(binding, {});
        if (!identity) {
            return std::move(identity).error();
        }
        return CredentialLease{std::move(identity).value(), std::nullopt};
    }
    case domain::AccountMode::application:
        return Error{ErrorCode::capability_unsupported, "application accounts are not supported yet",
                     false, {}};
    case domain::AccountMode::user:
        break;
    }

    const auto& credential_id = *binding.credential_id();
    for (int attempt = 0; attempt < lease_attempts; ++attempt) {
        auto record = credentials_->get(binding.principal_id(), credential_id);
        if (!record) {
            if (record.error().code == ErrorCode::not_found) {
                return credentials_required("the bound credential no longer exists");
            }
            return std::move(record).error();
        }
        if (record->status == ports::CredentialStatus::revoked || !record->secret_ref) {
            return credentials_required("the bound credential was revoked");
        }
        if (record->source != binding.source().value() || record->auth_profile != binding.auth_profile()) {
            return Error{ErrorCode::internal, "bound credential does not match its binding", false, {}};
        }
        auto secret = secrets_->get(*record->secret_ref);
        if (!secret) {
            // An update may have committed and removed this reference meanwhile.
            auto again = credentials_->get(binding.principal_id(), credential_id);
            if (again && again->version != record->version) {
                continue;
            }
            return std::move(secret).error();
        }
        auto identity = domain::AuthIdentityKey::make(binding, {record->version, {}, {}, {}});
        if (!identity) {
            return std::move(identity).error();
        }
        return CredentialLease{std::move(identity).value(), std::move(secret).value()};
    }
    return Error{ErrorCode::version_conflict, "credential kept changing while leasing", true, {}};
}

domain::Result<std::size_t> CredentialService::reclaim_orphan_secrets() {
    auto stored = secrets_->list();
    if (!stored) {
        return std::move(stored).error();
    }
    auto referenced = credentials_->referenced_secret_refs();
    if (!referenced) {
        return std::move(referenced).error();
    }
    const std::set<std::string> in_use(referenced->begin(), referenced->end());
    std::size_t removed = 0;
    for (const auto& ref : stored.value()) {
        if (in_use.count(ref) == 0) {
            if (auto done = secrets_->remove(ref); !done) {
                return done.error();
            }
            ++removed;
        }
    }
    return removed;
}

} // namespace music_service::application::accounts
