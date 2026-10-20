#pragma once

#include "music_dl/domain/catalog/refs.hpp"
#include "music_dl/domain/result.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace music_dl::domain {

enum class AccountMode { anonymous, application, user };

std::string_view to_string(AccountMode mode) noexcept;

// Stable account selection for one principal and source. Holds no secrets and no
// short-lived versions, so it is safe to persist in a download intent.
// - anonymous: no credential_id, no app_profile_id
// - application: app_profile_id required, no credential_id
// - user: credential_id required; app_profile_id is kept when the user flow depends on it
class AccountBinding {
public:
    static Result<AccountBinding> make(std::string principal_id, SourceId source, AccountMode mode,
                                       std::string auth_profile,
                                       std::optional<std::string> credential_id,
                                       std::optional<std::string> app_profile_id);

    [[nodiscard]] const std::string& principal_id() const noexcept { return principal_id_; }
    [[nodiscard]] const SourceId& source() const noexcept { return source_; }
    [[nodiscard]] AccountMode mode() const noexcept { return mode_; }
    [[nodiscard]] const std::string& auth_profile() const noexcept { return auth_profile_; }
    [[nodiscard]] const std::optional<std::string>& credential_id() const noexcept {
        return credential_id_;
    }
    [[nodiscard]] const std::optional<std::string>& app_profile_id() const noexcept {
        return app_profile_id_;
    }

    friend bool operator==(const AccountBinding&, const AccountBinding&) = default;

private:
    AccountBinding(std::string principal_id, SourceId source, AccountMode mode,
                   std::string auth_profile, std::optional<std::string> credential_id,
                   std::optional<std::string> app_profile_id);

    std::string principal_id_;
    SourceId source_;
    AccountMode mode_;
    std::string auth_profile_;
    std::optional<std::string> credential_id_;
    std::optional<std::string> app_profile_id_;
};

// Versioned authorization snapshot used to key caches, cursors, contexts and
// connection reuse. Two bindings with equal versions are still different keys.
class AuthIdentityKey {
public:
    struct Versions {
        std::optional<std::uint64_t> credential_version;
        std::optional<std::uint64_t> app_profile_version;
        std::optional<std::string> app_token_id;
        std::optional<std::uint64_t> app_token_version;
    };

    // Versions must be present exactly for the references the binding uses.
    static Result<AuthIdentityKey> make(AccountBinding binding, Versions versions);

    [[nodiscard]] const AccountBinding& binding() const noexcept { return binding_; }
    [[nodiscard]] const Versions& versions() const noexcept { return versions_; }

    friend bool operator==(const AuthIdentityKey& a, const AuthIdentityKey& b) {
        return a.binding_ == b.binding_ &&
               a.versions_.credential_version == b.versions_.credential_version &&
               a.versions_.app_profile_version == b.versions_.app_profile_version &&
               a.versions_.app_token_id == b.versions_.app_token_id &&
               a.versions_.app_token_version == b.versions_.app_token_version;
    }

    // Unambiguous text form for use as a map or cache key.
    [[nodiscard]] std::string canonical() const;

private:
    AuthIdentityKey(AccountBinding binding, Versions versions)
        : binding_(std::move(binding)), versions_(std::move(versions)) {}

    AccountBinding binding_;
    Versions versions_;
};

} // namespace music_dl::domain
