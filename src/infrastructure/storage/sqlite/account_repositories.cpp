#include "infrastructure/storage/sqlite/account_repositories.hpp"

#include "infrastructure/storage/sqlite/sql_support.hpp"

#include <sstream>

namespace music_service::infrastructure::storage::sqlite {

namespace ports = application::ports;
using detail::collect;
using detail::first;
using detail::from_millis;
using detail::not_found;
using detail::to_millis;

namespace {

// ---------------------------------------------------------------- principals, tokens

std::string_view role_text(ports::PrincipalRole role) {
    return role == ports::PrincipalRole::admin ? "admin" : "user";
}

constexpr std::string_view principal_columns = "id, display_name, role, enabled, created_at";

ports::PrincipalRecord read_principal(const Statement& s, int offset = 0) {
    return ports::PrincipalRecord{
        s.text(offset), s.text(offset + 1),
        s.text(offset + 2) == "admin" ? ports::PrincipalRole::admin : ports::PrincipalRole::user,
        s.integer(offset + 3) != 0, from_millis(s.integer(offset + 4))};
}

std::string join_scopes(const std::vector<std::string>& scopes) {
    std::string text;
    for (const auto& scope : scopes) {
        if (!text.empty()) {
            text.push_back(' ');
        }
        text.append(scope);
    }
    return text;
}

std::vector<std::string> split_scopes(const std::string& text) {
    std::vector<std::string> scopes;
    std::istringstream in{text};
    for (std::string scope; in >> scope;) {
        scopes.push_back(scope);
    }
    return scopes;
}

constexpr std::string_view token_columns =
    "id, principal_id, scopes, created_at, revoked_at";

ports::AccessTokenRecord read_token(const Statement& s, int offset = 0) {
    return ports::AccessTokenRecord{s.text(offset), s.text(offset + 1),
                                    split_scopes(s.text(offset + 2)),
                                    from_millis(s.integer(offset + 3)),
                                    from_millis(s.optional_integer(offset + 4))};
}

domain::Result<std::optional<ports::PrincipalRecord>> find_principal(Connection& db,
                                                                     const std::string& id) {
    auto s = db.prepare("SELECT " + std::string{principal_columns} +
                        " FROM principals WHERE id = ?");
    if (!s) {
        return std::move(s).error();
    }
    s->bind(1, id);
    return first<ports::PrincipalRecord>(*s, [](const Statement& st) { return read_principal(st); });
}

domain::Result<std::optional<ports::AccessTokenRecord>> find_token(Connection& db,
                                                                   const std::string& id) {
    auto s = db.prepare("SELECT " + std::string{token_columns} + " FROM access_tokens WHERE id = ?");
    if (!s) {
        return std::move(s).error();
    }
    s->bind(1, id);
    return first<ports::AccessTokenRecord>(*s, [](const Statement& st) { return read_token(st); });
}

// ------------------------------------------------------------------- credentials

ports::CredentialStatus parse_status(const std::string& text) {
    if (text == "revoked") {
        return ports::CredentialStatus::revoked;
    }
    return text == "invalid" ? ports::CredentialStatus::invalid : ports::CredentialStatus::active;
}

constexpr std::string_view credential_columns =
    "id, owner_id, source, label, kind, auth_profile, secret_ref, version, status, expires_at, "
    "created_at";

ports::CredentialRecord read_credential(const Statement& s) {
    return ports::CredentialRecord{s.text(0),
                                   s.text(1),
                                   s.text(2),
                                   s.text(3),
                                   s.text(4),
                                   s.text(5),
                                   s.optional_text(6),
                                   static_cast<std::uint64_t>(s.integer(7)),
                                   parse_status(s.text(8)),
                                   from_millis(s.optional_integer(9)),
                                   from_millis(s.integer(10))};
}

domain::Result<std::optional<ports::CredentialRecord>>
find_credential(Connection& db, const std::string& owner_id, const std::string& id) {
    auto s = db.prepare("SELECT " + std::string{credential_columns} +
                        " FROM credentials WHERE id = ? AND owner_id = ?");
    if (!s) {
        return std::move(s).error();
    }
    s->bind(1, id).bind(2, owner_id);
    return first<ports::CredentialRecord>(*s, read_credential);
}

domain::Result<std::optional<ports::DefaultSelection>>
find_default(Connection& db, const std::string& owner_id, const std::string& source) {
    auto s = db.prepare("SELECT source, credential_id, selection_version FROM credential_defaults"
                        " WHERE owner_id = ? AND source = ?");
    if (!s) {
        return std::move(s).error();
    }
    s->bind(1, owner_id).bind(2, source);
    return first<ports::DefaultSelection>(*s, [](const Statement& st) {
        return ports::DefaultSelection{st.text(0), st.optional_text(1),
                                       static_cast<std::uint64_t>(st.integer(2))};
    });
}

domain::Result<ports::DefaultSelection> write_default(Connection& db, const std::string& owner_id,
                                                      const std::string& source,
                                                      const std::optional<std::string>& credential_id,
                                                      std::uint64_t version) {
    auto s = db.prepare("INSERT INTO credential_defaults (owner_id, source, credential_id,"
                        " selection_version) VALUES (?, ?, ?, ?)"
                        " ON CONFLICT (owner_id, source) DO UPDATE SET"
                        " credential_id = excluded.credential_id,"
                        " selection_version = excluded.selection_version");
    if (!s) {
        return std::move(s).error();
    }
    s->bind(1, owner_id).bind(2, source).bind(3, credential_id).bind(
        4, static_cast<std::int64_t>(version));
    if (auto done = s->run(); !done) {
        return done.error();
    }
    return ports::DefaultSelection{source, credential_id, version};
}

// ------------------------------------------------------------------ destinations

ports::DestinationBinding read_binding(const Statement& s) {
    return ports::DestinationBinding{s.text(0), s.text(1), from_millis(s.integer(2))};
}

} // namespace

// ================================================================= identity

domain::Result<ports::PrincipalRecord>
SqliteIdentityRepository::create_principal(std::string display_name, ports::PrincipalRole role) {
    if (display_name.empty()) {
        return domain::Error::validation("display_name", "must not be empty");
    }
    ports::PrincipalRecord record{ctx_.ids->next("prn_"), std::move(display_name), role, true,
                                  ctx_.clock->now()};
    return ctx_.database->write([&](Connection& db) -> domain::Result<ports::PrincipalRecord> {
        auto s = db.prepare("INSERT INTO principals (id, display_name, role, enabled, created_at)"
                            " VALUES (?, ?, ?, 1, ?)");
        if (!s) {
            return std::move(s).error();
        }
        s->bind(1, record.id).bind(2, record.display_name).bind(3, role_text(role)).bind(
            4, to_millis(record.created_at));
        if (auto done = s->run(); !done) {
            return done.error();
        }
        // Read back so the returned time has the stored precision.
        auto stored = find_principal(db, record.id);
        if (!stored) {
            return std::move(stored).error();
        }
        return *stored.value();
    });
}

domain::Result<ports::PrincipalRecord> SqliteIdentityRepository::get_principal(const std::string& id) {
    return ctx_.database->read([&](Connection& db) -> domain::Result<ports::PrincipalRecord> {
        auto found = find_principal(db, id);
        if (!found) {
            return std::move(found).error();
        }
        if (!found.value()) {
            return not_found("principal");
        }
        return *found.value();
    });
}

domain::Result<std::vector<ports::PrincipalRecord>> SqliteIdentityRepository::list_principals() {
    return ctx_.database->read([&](Connection& db) -> domain::Result<std::vector<ports::PrincipalRecord>> {
        auto s = db.prepare("SELECT " + std::string{principal_columns} +
                            " FROM principals ORDER BY created_at, id");
        if (!s) {
            return std::move(s).error();
        }
        return collect<ports::PrincipalRecord>(*s, [](const Statement& st) { return read_principal(st); });
    });
}

domain::Result<ports::PrincipalRecord>
SqliteIdentityRepository::set_principal_enabled(const std::string& id, bool enabled) {
    return ctx_.database->write([&](Connection& db) -> domain::Result<ports::PrincipalRecord> {
        auto s = db.prepare("UPDATE principals SET enabled = ? WHERE id = ?");
        if (!s) {
            return std::move(s).error();
        }
        s->bind(1, std::int64_t{enabled ? 1 : 0}).bind(2, id);
        if (auto done = s->run(); !done) {
            return done.error();
        }
        auto found = find_principal(db, id);
        if (!found) {
            return std::move(found).error();
        }
        if (!found.value()) {
            return not_found("principal");
        }
        return *found.value();
    });
}

domain::Result<ports::AccessTokenRecord>
SqliteIdentityRepository::insert_token(const std::string& principal_id,
                                       const std::string& token_hash,
                                       std::vector<std::string> scopes) {
    if (token_hash.empty() || scopes.empty()) {
        return domain::Error::validation("token", "hash and scopes are required");
    }
    const auto id = ctx_.ids->next("tok_");
    const auto now = ctx_.clock->now();
    return ctx_.database->write([&](Connection& db) -> domain::Result<ports::AccessTokenRecord> {
        auto principal = find_principal(db, principal_id);
        if (!principal) {
            return std::move(principal).error();
        }
        if (!principal.value()) {
            return not_found("principal");
        }
        if (!principal.value()->enabled) {
            return domain::Error::validation("principal", "is disabled");
        }
        auto s = db.prepare("INSERT INTO access_tokens (id, principal_id, token_hash, scopes,"
                            " created_at) VALUES (?, ?, ?, ?, ?)");
        if (!s) {
            return std::move(s).error();
        }
        s->bind(1, id).bind(2, principal_id).bind(3, token_hash).bind(4, join_scopes(scopes)).bind(
            5, to_millis(now));
        if (auto done = s->run(); !done) {
            return done.error();
        }
        auto stored = find_token(db, id);
        if (!stored) {
            return std::move(stored).error();
        }
        return *stored.value();
    });
}

domain::Result<std::optional<ports::AuthenticatedToken>>
SqliteIdentityRepository::authenticate(const std::string& token_hash) {
    return ctx_.database->read(
        [&](Connection& db) -> domain::Result<std::optional<ports::AuthenticatedToken>> {
            auto s = db.prepare(
                "SELECT t.id, t.principal_id, t.scopes, t.created_at, t.revoked_at,"
                " p.id, p.display_name, p.role, p.enabled, p.created_at"
                " FROM access_tokens t JOIN principals p ON p.id = t.principal_id"
                " WHERE t.token_hash = ? AND t.revoked_at IS NULL AND p.enabled = 1");
            if (!s) {
                return std::move(s).error();
            }
            s->bind(1, token_hash);
            return first<ports::AuthenticatedToken>(*s, [](const Statement& st) {
                return ports::AuthenticatedToken{read_token(st, 0), read_principal(st, 5)};
            });
        });
}

domain::Result<ports::AccessTokenRecord>
SqliteIdentityRepository::revoke_token(const std::string& token_id) {
    const auto now = ctx_.clock->now();
    return ctx_.database->write([&](Connection& db) -> domain::Result<ports::AccessTokenRecord> {
        auto s = db.prepare("UPDATE access_tokens SET revoked_at = ? WHERE id = ? AND revoked_at"
                            " IS NULL");
        if (!s) {
            return std::move(s).error();
        }
        s->bind(1, to_millis(now)).bind(2, token_id);
        if (auto done = s->run(); !done) {
            return done.error();
        }
        auto found = find_token(db, token_id);
        if (!found) {
            return std::move(found).error();
        }
        if (!found.value()) {
            return not_found("access token");
        }
        return *found.value();
    });
}

domain::Result<std::vector<ports::AccessTokenRecord>>
SqliteIdentityRepository::list_tokens(const std::string& principal_id) {
    return ctx_.database->read([&](Connection& db) -> domain::Result<std::vector<ports::AccessTokenRecord>> {
        auto s = db.prepare("SELECT " + std::string{token_columns} +
                            " FROM access_tokens WHERE principal_id = ? ORDER BY created_at, id");
        if (!s) {
            return std::move(s).error();
        }
        s->bind(1, principal_id);
        return collect<ports::AccessTokenRecord>(*s, [](const Statement& st) { return read_token(st); });
    });
}

// ================================================================ credentials

domain::Result<ports::CredentialRecord>
SqliteCredentialRepository::insert(const ports::NewCredential& c) {
    if (c.source.empty() || c.kind.empty() || c.auth_profile.empty() || c.secret_ref.empty()) {
        return domain::Error::validation("credential", "source, kind, profile and secret are required");
    }
    const auto id = ctx_.ids->next("cre_");
    const auto now = ctx_.clock->now();
    return ctx_.database->write([&](Connection& db) -> domain::Result<ports::CredentialRecord> {
        auto s = db.prepare("INSERT INTO credentials (id, owner_id, source, label, kind,"
                            " auth_profile, secret_ref, version, status, expires_at, created_at)"
                            " VALUES (?, ?, ?, ?, ?, ?, ?, 1, 'active', ?, ?)");
        if (!s) {
            return std::move(s).error();
        }
        s->bind(1, id).bind(2, c.owner_id).bind(3, c.source).bind(4, c.label).bind(5, c.kind).bind(
            6, c.auth_profile).bind(7, c.secret_ref).bind(8, to_millis(c.expires_at)).bind(
            9, to_millis(now));
        if (auto done = s->run(); !done) {
            return done.error();
        }
        auto stored = find_credential(db, c.owner_id, id);
        if (!stored) {
            return std::move(stored).error();
        }
        return *stored.value();
    });
}

domain::Result<ports::CredentialRecord> SqliteCredentialRepository::get(const std::string& owner_id,
                                                                        const std::string& id) {
    return ctx_.database->read([&](Connection& db) -> domain::Result<ports::CredentialRecord> {
        auto found = find_credential(db, owner_id, id);
        if (!found) {
            return std::move(found).error();
        }
        if (!found.value()) {
            return not_found("credential");
        }
        return *found.value();
    });
}

domain::Result<std::vector<ports::CredentialRecord>>
SqliteCredentialRepository::list(const std::string& owner_id,
                                 const std::optional<std::string>& source) {
    return ctx_.database->read([&](Connection& db) -> domain::Result<std::vector<ports::CredentialRecord>> {
        auto s = db.prepare("SELECT " + std::string{credential_columns} +
                            " FROM credentials WHERE owner_id = ? AND (? IS NULL OR source = ?)"
                            " ORDER BY created_at, id");
        if (!s) {
            return std::move(s).error();
        }
        s->bind(1, owner_id).bind(2, source).bind(3, source);
        return collect<ports::CredentialRecord>(*s, read_credential);
    });
}

domain::Result<ports::SecretReplacement>
SqliteCredentialRepository::replace_secret(const std::string& owner_id, const std::string& id,
                                           std::uint64_t expected_version,
                                           const std::string& new_secret_ref,
                                           const std::optional<std::string>& new_label) {
    return ctx_.database->write([&](Connection& db) -> domain::Result<ports::SecretReplacement> {
        auto found = find_credential(db, owner_id, id);
        if (!found) {
            return std::move(found).error();
        }
        if (!found.value()) {
            return not_found("credential");
        }
        const auto current = *found.value();
        if (current.status == ports::CredentialStatus::revoked) {
            return domain::Error::validation("credential", "is revoked");
        }
        if (current.version != expected_version) {
            return domain::Error{domain::ErrorCode::version_conflict,
                                 "credential changed concurrently", true, {}};
        }
        auto s = db.prepare("UPDATE credentials SET secret_ref = ?, label = ?, version = ?,"
                            " status = 'active' WHERE id = ? AND owner_id = ?");
        if (!s) {
            return std::move(s).error();
        }
        s->bind(1, new_secret_ref).bind(2, new_label.value_or(current.label)).bind(
            3, static_cast<std::int64_t>(current.version + 1)).bind(4, id).bind(5, owner_id);
        if (auto done = s->run(); !done) {
            return done.error();
        }
        auto stored = find_credential(db, owner_id, id);
        if (!stored) {
            return std::move(stored).error();
        }
        return ports::SecretReplacement{*stored.value(), current.secret_ref};
    });
}

domain::Result<ports::SecretReplacement> SqliteCredentialRepository::revoke(const std::string& owner_id,
                                                                            const std::string& id) {
    const auto now = ctx_.clock->now();
    return ctx_.database->write([&](Connection& db) -> domain::Result<ports::SecretReplacement> {
        auto found = find_credential(db, owner_id, id);
        if (!found) {
            return std::move(found).error();
        }
        if (!found.value()) {
            return not_found("credential");
        }
        const auto current = *found.value();
        if (current.status == ports::CredentialStatus::revoked) {
            return ports::SecretReplacement{current, std::nullopt};
        }
        auto s = db.prepare("UPDATE credentials SET status = 'revoked', secret_ref = NULL,"
                            " version = version + 1, revoked_at = ? WHERE id = ?");
        if (!s) {
            return std::move(s).error();
        }
        s->bind(1, to_millis(now)).bind(2, id);
        if (auto done = s->run(); !done) {
            return done.error();
        }
        // The default that pointed here is cleared in the same transaction.
        auto clear = db.prepare("UPDATE credential_defaults SET credential_id = NULL,"
                                " selection_version = selection_version + 1"
                                " WHERE owner_id = ? AND credential_id = ?");
        if (!clear) {
            return std::move(clear).error();
        }
        clear->bind(1, owner_id).bind(2, id);
        if (auto done = clear->run(); !done) {
            return done.error();
        }
        auto stored = find_credential(db, owner_id, id);
        if (!stored) {
            return std::move(stored).error();
        }
        return ports::SecretReplacement{*stored.value(), current.secret_ref};
    });
}

domain::Result<ports::DefaultSelection>
SqliteCredentialRepository::get_default(const std::string& owner_id, const std::string& source) {
    return ctx_.database->read([&](Connection& db) -> domain::Result<ports::DefaultSelection> {
        auto found = find_default(db, owner_id, source);
        if (!found) {
            return std::move(found).error();
        }
        return found.value().value_or(ports::DefaultSelection{source, std::nullopt, 0});
    });
}

domain::Result<ports::DefaultSelection>
SqliteCredentialRepository::set_default(const std::string& owner_id, const std::string& source,
                                        const std::string& credential_id) {
    return ctx_.database->write([&](Connection& db) -> domain::Result<ports::DefaultSelection> {
        auto credential = find_credential(db, owner_id, credential_id);
        if (!credential) {
            return std::move(credential).error();
        }
        if (!credential.value()) {
            return not_found("credential");
        }
        if (credential.value()->source != source) {
            return domain::Error::validation("credential_id", "belongs to another source");
        }
        if (credential.value()->status == ports::CredentialStatus::revoked) {
            return domain::Error::validation("credential_id", "is revoked");
        }
        auto current = find_default(db, owner_id, source);
        if (!current) {
            return std::move(current).error();
        }
        const auto& existing = current.value();
        if (existing && existing->credential_id == credential_id) {
            return *existing;
        }
        const auto version = existing ? existing->selection_version + 1 : 1;
        return write_default(db, owner_id, source, credential_id, version);
    });
}

domain::Result<ports::DefaultSelection>
SqliteCredentialRepository::clear_default(const std::string& owner_id, const std::string& source) {
    return ctx_.database->write([&](Connection& db) -> domain::Result<ports::DefaultSelection> {
        auto current = find_default(db, owner_id, source);
        if (!current) {
            return std::move(current).error();
        }
        const auto& existing = current.value();
        if (!existing) {
            return ports::DefaultSelection{source, std::nullopt, 0};
        }
        if (!existing->credential_id) {
            return *existing;
        }
        return write_default(db, owner_id, source, std::nullopt, existing->selection_version + 1);
    });
}

// =============================================================== destinations

domain::Result<ports::DestinationBinding>
SqliteDestinationRepository::bind(const std::string& id, const std::string& canonical_root) {
    if (id.empty() || canonical_root.empty()) {
        return domain::Error::validation("destination", "id and root are required");
    }
    const auto now = ctx_.clock->now();
    return ctx_.database->write([&](Connection& db) -> domain::Result<ports::DestinationBinding> {
        auto s = db.prepare("SELECT id, canonical_root, registered_at FROM destinations WHERE id = ?");
        if (!s) {
            return std::move(s).error();
        }
        s->bind(1, id);
        auto existing = first<ports::DestinationBinding>(*s, read_binding);
        if (!existing) {
            return std::move(existing).error();
        }
        if (existing.value()) {
            if (existing.value()->canonical_root != canonical_root) {
                return domain::Error{domain::ErrorCode::destination_root_changed,
                                     "destination " + id + " is bound to '" +
                                         existing.value()->canonical_root + "', not '" +
                                         canonical_root + "'; use a new destination id",
                                     false,
                                     {}};
            }
            return *existing.value();
        }
        auto insert = db.prepare("INSERT INTO destinations (id, canonical_root, registered_at)"
                                 " VALUES (?, ?, ?)");
        if (!insert) {
            return std::move(insert).error();
        }
        insert->bind(1, id).bind(2, canonical_root).bind(3, to_millis(now));
        if (auto done = insert->run(); !done) {
            return done.error();
        }
        return ports::DestinationBinding{id, canonical_root, detail::from_millis(to_millis(now))};
    });
}

domain::Result<ports::DestinationBinding> SqliteDestinationRepository::get(const std::string& id) {
    return ctx_.database->read([&](Connection& db) -> domain::Result<ports::DestinationBinding> {
        auto s = db.prepare("SELECT id, canonical_root, registered_at FROM destinations WHERE id = ?");
        if (!s) {
            return std::move(s).error();
        }
        s->bind(1, id);
        auto found = first<ports::DestinationBinding>(*s, read_binding);
        if (!found) {
            return std::move(found).error();
        }
        if (!found.value()) {
            return not_found("destination");
        }
        return *found.value();
    });
}

domain::Result<std::vector<ports::DestinationBinding>> SqliteDestinationRepository::list() {
    return ctx_.database->read([&](Connection& db) -> domain::Result<std::vector<ports::DestinationBinding>> {
        auto s = db.prepare("SELECT id, canonical_root, registered_at FROM destinations ORDER BY id");
        if (!s) {
            return std::move(s).error();
        }
        return collect<ports::DestinationBinding>(*s, read_binding);
    });
}

domain::Result<bool> SqliteDestinationRepository::in_use(const std::string& id) {
    return ctx_.database->read([&](Connection& db) -> domain::Result<bool> {
        auto s = db.prepare("SELECT EXISTS (SELECT 1 FROM download_tasks WHERE destination_id = ?)"
                            " OR EXISTS (SELECT 1 FROM file_assets WHERE destination_id = ?)"
                            " OR EXISTS (SELECT 1 FROM finalization_intents"
                            " WHERE destination_id = ?)");
        if (!s) {
            return std::move(s).error();
        }
        s->bind(1, id).bind(2, id).bind(3, id);
        auto row = s->step();
        if (!row) {
            return std::move(row).error();
        }
        return s->integer(0) != 0;
    });
}

} // namespace music_service::infrastructure::storage::sqlite
