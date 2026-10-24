#include "application/accounts/credential_service.hpp"

#include "domain/catalog/refs.hpp"
#include "application/ports/runtime/scope_exit.hpp"

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
    if (control && kind == ports::CredentialKind::cookie) {
        return Error::validation("secret", "must not contain control characters");
    }
    return domain::ok();
}

Error credentials_required(std::string message) {
    return Error{ErrorCode::credentials_required, std::move(message), false, {}};
}

// Why a credential cannot authorize new work, if it cannot: revoked, failed its last
// verification, or past its known expiry. Verification itself may still run, so a
// renewed account can become usable again.
std::optional<std::string> unusable_reason(const ports::CredentialRecord& record,
                                           std::chrono::system_clock::time_point now) {
    if (!record.enabled) return "is disabled";
    if (record.refresh_state == "reauthorization_required") return "requires OAuth authorization again";
    switch (record.status) {
    case ports::CredentialStatus::revoked:
        return "is revoked";
    case ports::CredentialStatus::invalid:
        return "failed verification";
    case ports::CredentialStatus::active:
        break;
    }
    if (!record.secret_ref) {
        return "is revoked";
    }
    if (record.expires_at && *record.expires_at <= now) {
        return "has expired";
    }
    return std::nullopt;
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

domain::Result<ports::CredentialRecord> CredentialService::create(NewCredentialRequest request) {
    if (auto profile = providers_->describe_auth_profile(request.source, request.auth_profile); !profile) {
        if (profile.error().code == ErrorCode::capability_unsupported) {
            return Error::validation("auth_profile", "is not registered for this source");
        }
        return std::move(profile).error();
    } else if (profile->kind != request.kind) {
        return Error::validation("kind", "does not match the auth profile");
    }
    if (request.enabled) {
        if (auto valid = check_secret(request.kind, request.secret); !valid) return valid.error();
        if (auto valid = providers_->validate_secret(request.source, request.auth_profile, request.kind, request.secret); !valid)
            return valid.error();
    } else if (!request.secret.empty()) return Error::validation("secret", "must be absent for a disabled account");
    if (auto valid = check_label(request.label); !valid) {
        return valid.error();
    }

    if (auto cleaned = cleanup_secrets(request.owner_id, request.source); !cleaned) return cleaned.error();
    std::string ref;
    if (request.enabled) {
        auto saved = secrets_->put(request.secret);
        if (!saved) return saved.error();
        ref = *saved;
    }
    auto stored = credentials_->insert(ports::NewCredential{
        request.owner_id, request.source, request.label, std::string{ports::to_string(request.kind)},
        request.auth_profile, ref, request.expires_at, {}, {}, 0, {}, request.enabled});
    if (!stored) {
        // Best effort; a leftover is reclaimed as an orphan.
        if (!ref.empty()) (void)secrets_->remove(ref);
        return std::move(stored).error();
    }
    interrupt_inactive_verifications(request.owner_id, request.source);
    if (auto cleaned = cleanup_secrets(request.owner_id, request.source); !cleaned) return cleaned.error();
    if (stored->enabled && stored->kind == "cookie" && verification_) return schedule_verification(stored->owner_id, stored->id);
    return stored;
}

domain::Result<ports::CredentialRecord> CredentialService::get(const std::string& owner_id,
                                                               const std::string& id) {
    auto record = credentials_->get(owner_id, id);
    if (record) apply_verification_overlay(*record);
    return record;
}

domain::Result<std::vector<ports::CredentialRecord>>
CredentialService::list(const std::string& owner_id, const std::optional<std::string>& source) {
    auto records = credentials_->list(owner_id, source);
    if (records) for (auto& record : *records) apply_verification_overlay(record);
    return records;
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
        if (!current->enabled) return Error::validation("secret", "enable the account with a new secret through credential-defaults");
        const auto kind = parse_kind(current->kind);
        if (!kind) {
            return Error{ErrorCode::internal, "stored credential kind is unknown", false, {}};
        }
        if (auto valid = check_secret(*kind, *update.secret); !valid) {
            return valid.error();
        }
        if (auto valid = providers_->validate_secret(current->source, current->auth_profile, *kind, *update.secret); !valid)
            return valid.error();
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
    if (auto cleaned = cleanup_secrets(owner_id, current->source); !cleaned) return cleaned.error();
    if (replaced->credential.kind == "cookie" && verification_ && (new_ref || current->verification_state == "pending"))
        return schedule_verification(owner_id, id);
    return get(owner_id, id);
}

domain::Result<ports::CredentialRecord> CredentialService::revoke(const std::string& owner_id,
                                                                  const std::string& id) {
    auto revoked = credentials_->revoke(owner_id, id);
    if (!revoked) {
        return std::move(revoked).error();
    }
    interrupt_verifications(owner_id, id);
    if (auto cleaned = cleanup_secrets(owner_id, revoked->credential.source); !cleaned) return cleaned.error();
    return get(owner_id, id);
}

domain::Result<ports::CredentialRecord> CredentialService::disable(const std::string& owner, const std::string& id) {
    auto result = credentials_->disable(owner, id);
    if (!result) return result.error();
    interrupt_inactive_verifications(owner, result->source);
    if (auto cleaned = cleanup_secrets(owner, result->source); !cleaned) return cleaned.error();
    return get(owner, id);
}

domain::Result<void> CredentialService::cleanup_secrets(const std::optional<std::string>& owner,
    const std::optional<std::string>& source) {
    auto pending = credentials_->pending_secret_deletions(owner, source);
    if (!pending) return pending.error();
    for (const auto& ref : *pending) {
        if (auto removed = secrets_->remove(ref); !removed)
            return Error{ErrorCode::storage_unavailable, "account state saved; secret cleanup is pending, retry this operation", true, {}};
        if (auto done = credentials_->finish_secret_deletion(ref); !done) return done.error();
    }
    auto remaining = credentials_->pending_secret_deletions(owner, source);
    if (!remaining) return remaining.error();
    if (!remaining->empty()) return Error{ErrorCode::service_busy, "secret cleanup is pending, retry this operation", true, {}};
    return domain::ok();
}

domain::Result<VerifyResult> CredentialService::verify(const std::string& owner_id, const std::string& id,
    const ports::OperationContext& context, std::optional<std::uint64_t> expected_version,
    std::optional<std::string> verification_id) {
    auto record = credentials_->get(owner_id, id);
    if (!record) {
        return std::move(record).error();
    }
    if (record->status == ports::CredentialStatus::revoked || !record->enabled || !record->secret_ref) {
        return Error::validation("credential", "is revoked or disabled");
    }
    if ((expected_version && record->version != *expected_version) ||
        (verification_id && (record->verification_id != verification_id || record->verification_state != "pending")))
        return Error{ErrorCode::version_conflict, "credential changed before verification", true, {}};
    auto perform = [&]() -> domain::Result<ports::CredentialVerification> {
        if (context.stop.stop_requested()) return Error{ErrorCode::canceled, "verification canceled", false, {}};
        if (context.expired(std::chrono::steady_clock::now()))
            return Error{ErrorCode::deadline_exceeded, "verification deadline exceeded", true, {}};
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

        return verifier.value()->verify({identity.value(), secret.value()}, context);
    };
    domain::Result<ports::CredentialVerification> verification = Error{ErrorCode::internal, "verification failed", false, {}};
    try { verification = perform(); } catch (...) { /* 边界异常只保存脱敏错误类别。 */ }
    if (context.stop.stop_requested()) verification = Error{ErrorCode::canceled, "verification canceled", false, {}};
    else if (context.expired(std::chrono::steady_clock::now()) &&
             (!verification || verification->status != ports::VerificationStatus::valid))
        verification = Error{ErrorCode::deadline_exceeded, "verification deadline exceeded", true, {}};
    std::optional<std::string> error;
    if (!verification) {
        error = std::string{domain::to_string(verification.error().code)};
        verification = ports::CredentialVerification{};
    }
    if (verification->status != ports::VerificationStatus::valid) verification->membership = {};
    verification->membership.checked_at = clock_->now();
    if (error) verification->membership.error_code = error;
    // Stored only for the version that was checked; a later secret is not judged by
    // an earlier result.
    auto stored = credentials_->record_verification(
        owner_id, id, record->version,
        {to_outcome(verification->status), clock_->now(), verification->expires_at, verification->scopes,
         verification->membership, verification_id, error});
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
                                                                       const std::string& credential_id,
                                                                       std::optional<ports::SecretBytes> secret) {
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
    if (record->status == ports::CredentialStatus::revoked) return Error::validation("credential_id", "is revoked");
    if (!secret && !record->enabled) return Error::validation("secret", "a new secret is required to enable a disabled account");
    if (!secret) if (auto reason = unusable_reason(record.value(), clock_->now())) return Error::validation("credential_id", *reason);
    auto profile = providers_->describe_auth_profile(source, record->auth_profile);
    if (!profile || ports::to_string(profile->kind) != record->kind) {
        return Error::validation("credential_id", "its auth profile is no longer registered");
    }
    if (auto cleaned = cleanup_secrets(owner_id, source); !cleaned) return cleaned.error();
    std::optional<std::string> new_ref;
    if (secret) {
        if (auto valid = check_secret(profile->kind, *secret); !valid) return valid.error();
        if (auto valid = providers_->validate_secret(source, record->auth_profile, profile->kind, *secret); !valid) return valid.error();
        auto saved = secrets_->put(*secret);
        if (!saved) return saved.error();
        new_ref = *saved;
    }
    auto selected = credentials_->set_default(owner_id, source, credential_id, record->version, new_ref);
    if (!selected) {
        if (new_ref) (void)secrets_->remove(*new_ref);
        return selected.error();
    }
    interrupt_inactive_verifications(owner_id, source);
    if (auto cleaned = cleanup_secrets(owner_id, source); !cleaned) return cleaned.error();
    if (new_ref && record->kind == "cookie" && verification_) {
        if (auto scheduled = schedule_verification(owner_id, credential_id); !scheduled) return scheduled.error();
    }
    return selected;
}

domain::Result<ports::DefaultSelection> CredentialService::clear_default(const std::string& owner_id,
                                                                         const std::string& source) {
    if (auto valid = domain::SourceId::parse(source); !valid) {
        return std::move(valid).error();
    }
    auto result = credentials_->clear_default(owner_id, source);
    if (!result) return result.error();
    interrupt_inactive_verifications(owner_id, source);
    if (auto cleaned = cleanup_secrets(owner_id, source); !cleaned) return cleaned.error();
    return result;
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
        if (auto reason = unusable_reason(record.value(), clock_->now())) {
            return Error::validation("credential_id", *reason);
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
        if (!unusable_reason(record.value(), clock_->now()) && allows(record->auth_profile)) {
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
        if (auto reason = unusable_reason(record.value(), clock_->now())) {
            return credentials_required("the bound credential " + *reason);
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

namespace {
domain::Result<void> check_grant(const ports::OAuthGrant& grant) {
    if (grant.expires_in <= std::chrono::seconds::zero() || grant.expires_in > std::chrono::hours{24 * 366})
        return Error{ErrorCode::upstream_failed, "invalid OAuth token lifetime", false, {}};
    if (auto valid = check_secret(ports::CredentialKind::oauth2, grant.secret); !valid) return valid;
    if (grant.scopes.size() > 64) return Error::validation("scopes", "too many scopes");
    for (const auto& scope : grant.scopes) {
        if (scope.empty() || scope.size() > 128 || std::any_of(scope.begin(), scope.end(), [](unsigned char c) {
            return c < 0x21 || c > 0x7e || c == '"' || c == '\\';
        })) return Error::validation("scopes", "invalid OAuth scope");
    }
    return domain::ok();
}
std::chrono::system_clock::time_point refresh_after(std::chrono::system_clock::time_point issued_at,
                                                   std::chrono::seconds lifetime) {
    // 提前最多一分钟且不超过有效期的十分之一，避免短期 token 不断刷新。
    return issued_at + lifetime - std::min(std::chrono::seconds{60}, lifetime / 10);
}
}

domain::Result<ports::CredentialRecord> CredentialService::create_oauth(const std::string& owner,
    const std::string& label, const ports::OAuthApplication& application, ports::OAuthGrant grant,
    std::chrono::system_clock::time_point issued_at) {
    if (auto valid = check_label(label); !valid) return valid.error();
    if (auto valid = check_grant(grant); !valid) return valid.error();
    if (issued_at + grant.expires_in <= clock_->now())
        return Error{ErrorCode::upstream_failed, "OAuth token expired during exchange", false, {}};
    if (auto valid = providers_->validate_secret(application.source, application.auth_profile,
        ports::CredentialKind::oauth2, grant.secret); !valid) return valid.error();
    auto ref = secrets_->put(grant.secret);
    if (!ref) return ref.error();
    auto stored = credentials_->insert({owner, application.source, label, "oauth2", application.auth_profile,
        *ref, issued_at + grant.expires_in, grant.scopes, application.id, application.version,
        refresh_after(issued_at, grant.expires_in)});
    if (!stored) { (void)secrets_->remove(*ref); return stored.error(); }
    interrupt_inactive_verifications(owner, application.source);
    if (auto cleaned = cleanup_secrets(owner, application.source); !cleaned) return cleaned.error();
    return stored;
}

domain::Result<ports::CredentialRecord> CredentialService::refresh_oauth(const ports::CredentialRecord& candidate,
    const ports::OAuthApplication& application, const ports::OperationContext& context) {
    if (context.stop.stop_requested() || context.expired(clock_->steady_now()))
        return Error{ErrorCode::canceled, "OAuth refresh canceled before dispatch", false, {}};
    if (candidate.kind != "oauth2" || candidate.oauth_application_id != application.id ||
        candidate.oauth_application_version != application.version || candidate.source != application.source ||
        candidate.auth_profile != application.auth_profile || !candidate.secret_ref)
        return credentials_required("OAuth application changed; authorize again");
    auto oauth = providers_->require_oauth(candidate.source, candidate.auth_profile);
    if (!oauth) return oauth.error();
    auto secret = secrets_->get(*candidate.secret_ref);
    if (!secret) return secret.error();
    auto source = domain::SourceId::parse(candidate.source);
    if (!source) return source.error();
    auto binding = domain::AccountBinding::make(candidate.owner_id, *source, domain::AccountMode::user,
        candidate.auth_profile, candidate.id, std::nullopt);
    if (!binding) return binding.error();
    auto identity = domain::AuthIdentityKey::make(*binding, {candidate.version, {}, {}, {}});
    if (!identity) return identity.error();
    auto claimed = credentials_->begin_refresh(candidate.owner_id, candidate.id, candidate.version);
    if (!claimed) return claimed.error();
    // 任何异常、超时或存储失败都不能使旧 refresh token 再次进入自动刷新。
    bool committed = false;
    ports::ScopeExit failure{[&] {
        try {
            if (!committed) (void)credentials_->finish_refresh(candidate.owner_id, candidate.id,
                candidate.version, std::nullopt, {}, {});
        } catch (...) { /* pending 保留在数据库中，重启恢复也不重放 */ }
    }};
    try {
        const auto issued_at = clock_->now();
        auto grant = (*oauth)->refresh({*identity, *secret}, context);
        if (!grant) return Error{grant.error().code, "OAuth refresh failed; authorize again", false, {}};
        if (context.stop.stop_requested() || context.expired(clock_->steady_now()))
            return Error{ErrorCode::canceled, "OAuth refresh did not finish within its budget", false, {}};
        if (auto valid = check_grant(*grant); !valid) return valid.error();
        if (issued_at + grant->expires_in <= clock_->now())
            return Error{ErrorCode::upstream_failed, "OAuth token expired during refresh", false, {}};
        if (auto valid = providers_->validate_secret(candidate.source, candidate.auth_profile,
            ports::CredentialKind::oauth2, grant->secret); !valid) return valid.error();
        auto ref = secrets_->put(grant->secret);
        if (!ref) return ref.error();
        auto result = credentials_->finish_refresh(candidate.owner_id, candidate.id, candidate.version,
            *ref, issued_at + grant->expires_in, grant->scopes, refresh_after(issued_at, grant->expires_in));
        if (!result) { (void)secrets_->remove(*ref); return result.error(); }
        committed = true;
        if (auto cleaned = cleanup_secrets(candidate.owner_id, candidate.source); !cleaned) return cleaned.error();
        return get(candidate.owner_id, candidate.id);
    } catch (...) {
        return Error{ErrorCode::upstream_failed, "OAuth refresh failed; authorize again", false, {}};
    }
}

domain::Result<std::size_t> CredentialService::reclaim_orphan_secrets(const std::vector<std::string>& reserved) {
    auto stored = secrets_->list();
    if (!stored) {
        return std::move(stored).error();
    }
    auto referenced = credentials_->referenced_secret_refs();
    if (!referenced) {
        return std::move(referenced).error();
    }
    std::set<std::string> in_use(referenced->begin(), referenced->end());
    in_use.insert(reserved.begin(), reserved.end());
    std::size_t removed = 0;
    for (const auto& ref : stored.value()) {
        if (in_use.count(ref) == 0) {
            if (auto done = secrets_->remove(ref); !done) {
                return done.error();
            }
            ++removed;
        }
    }
    // 已移除孤儿文件后确认持久清理队列；只有启动阶段执行，尚无并发写入。
    for (;;) {
        auto pending = credentials_->pending_secret_deletions({}, {});
        if (!pending) return pending.error();
        if (pending->empty()) break;
        for (const auto& ref : *pending) {
            if (in_use.count(ref)) return Error{ErrorCode::storage_unavailable, "cleanup references an active secret", false, {}};
            if (auto done = credentials_->finish_secret_deletion(ref); !done) return done.error();
        }
    }
    return removed;
}

} // namespace music_service::application::accounts
