// Service identities, access tokens and platform credentials against real SQLite files,
// the file SecretStore and a provider registry with simulated sources.

#include "../unit/providers/mock_providers.hpp"

#include "application/accounts/credential_service.hpp"
#include "application/accounts/identity_admin.hpp"
#include "application/accounts/scopes.hpp"
#include "infrastructure/auth/access_tokens.hpp"
#include "infrastructure/clock.hpp"
#include "infrastructure/filesystem/files.hpp"
#include "infrastructure/filesystem/paths.hpp"
#include "infrastructure/secrets/file_secret_store.hpp"
#include "infrastructure/storage/sqlite/account_repositories.hpp"
#include "infrastructure/storage/sqlite/schema.hpp"
#include "providers/registry/provider_registry.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <barrier>
#include <functional>
#include <deque>
#include <thread>
#include <set>
#include <string>
#include <utility>

using namespace music_service;
using namespace music_service::test;
using namespace music_service::application::accounts;
using namespace music_service::infrastructure::storage::sqlite;
namespace fs = std::filesystem;
using domain::ErrorCode;

namespace {

// Identity repository whose token insert can be made to fail, to simulate a database
// failure after the delivery file was written.
class FlakyIdentityRepository final : public ports::IdentityRepository {
public:
    explicit FlakyIdentityRepository(std::shared_ptr<ports::IdentityRepository> inner)
        : inner_(std::move(inner)) {}

    bool fail_insert{false};

    domain::Result<ports::PrincipalRecord> create_principal(std::string name, ports::PrincipalRole role,
                                                            std::optional<std::string> id) override {
        return inner_->create_principal(std::move(name), role, std::move(id));
    }
    domain::Result<ports::PrincipalRecord> get_principal(const std::string& id) override {
        return inner_->get_principal(id);
    }
    domain::Result<std::vector<ports::PrincipalRecord>> list_principals() override {
        return inner_->list_principals();
    }
    domain::Result<ports::PrincipalRecord> set_principal_enabled(const std::string& id, bool enabled) override {
        return inner_->set_principal_enabled(id, enabled);
    }
    domain::Result<ports::AccessTokenRecord> insert_token(const std::string& principal_id,
                                                          const std::string& hash,
                                                          std::vector<std::string> scopes) override {
        if (fail_insert) {
            return domain::Error{ErrorCode::storage_unavailable, "simulated failure", true, {}};
        }
        return inner_->insert_token(principal_id, hash, std::move(scopes));
    }
    domain::Result<std::optional<ports::AuthenticatedToken>> authenticate(const std::string& hash) override {
        return inner_->authenticate(hash);
    }
    domain::Result<ports::AccessTokenRecord> revoke_token(const std::string& id) override {
        return inner_->revoke_token(id);
    }
    domain::Result<std::vector<ports::AccessTokenRecord>> list_tokens(const std::string& principal_id) override {
        return inner_->list_tokens(principal_id);
    }

private:
    std::shared_ptr<ports::IdentityRepository> inner_;
};

// Verifier that runs a hook before answering, to let a test change the credential
// while a verification is in flight, and returns the evidence the test chose.
class HookVerifier final : public ports::ICredentialVerifier {
public:
    HookVerifier(std::function<void()> hook, const ports::CredentialVerification& verdict)
        : hook_(std::move(hook)), verdict_(verdict) {}
    domain::Result<ports::CredentialVerification> verify(const ports::CredentialSnapshot&,
                                                         const ports::OperationContext&) override {
        if (hook_) {
            hook_();
        }
        return verdict_;
    }

private:
    std::function<void()> hook_;
    const ports::CredentialVerification& verdict_;
};

class FailingSecretRemoval final : public ports::SecretStore {
public:
    explicit FailingSecretRemoval(std::shared_ptr<ports::SecretStore> inner) : inner_(std::move(inner)) {}
    bool fail{true};
    domain::Result<std::string> put(const ports::SecretBytes& bytes) override { return inner_->put(bytes); }
    domain::Result<ports::SecretBytes> get(std::string_view ref) override { return inner_->get(ref); }
    domain::Result<std::vector<std::string>> list() override { return inner_->list(); }
    domain::Result<void> remove(std::string_view ref) override {
        if (fail) return domain::Error{ErrorCode::storage_unavailable, "simulated removal failure", true, {}};
        return inner_->remove(ref);
    }
private:
    std::shared_ptr<ports::SecretStore> inner_;
};

struct Accounts {
    explicit Accounts(std::string_view name, application::config::ProviderConfig off_config = {})
        : dir(fs::temp_directory_path() /
              infrastructure::filesystem::path_from_utf8("music-service-账号-" + std::string{name})) {
        fs::remove_all(dir);
        fs::create_directories(dir / "secrets");
        fs::create_directories(dir / "out");

        auto oauth = base_bundle("oauthy");
        oauth.auth_profiles = {{.auth_profile = "web_oauth", .kind = ports::CredentialKind::oauth2}};
        auto hooked = base_bundle("hooked");
        hooked.auth_profiles = {{.auth_profile = "web_cookie",
                                 .kind = ports::CredentialKind::cookie,
                                 .verifier = [this](const ports::ProviderSetup&)
                                     -> domain::Result<std::shared_ptr<ports::ICredentialVerifier>> {
                                     return std::shared_ptr<ports::ICredentialVerifier>{
                                         std::make_shared<HookVerifier>(
                                             [this] {
                                                 if (verify_hook) {
                                                     verify_hook();
                                                 }
                                             },
                                             hooked_verdict)};
                                 }}};
        hooked.capabilities = {{.key = "verify_credentials",
                                .credentials = ports::CredentialRequirement::required,
                                .auth_profiles = {"web_cookie"}}};
        std::map<std::string, application::config::ProviderConfig> config{{"off", off_config}};
        config["off"].enabled = false;
        registry = unwrap(providers::ProviderRegistry::build(
            {mock_catalog("cat"), mock_audio("aud"), mock_combo("both"), mock_combo("off"), std::move(oauth),
             std::move(hooked)},
            config));
        reopen();
    }
    ~Accounts() {
        identity_admin.reset();
        credentials.reset();
        identities.reset();
        credential_repo.reset();
        database.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    // A process restart: new database connections and services over the same files.
    void reopen() {
        identity_admin.reset();
        credentials.reset();
        identities.reset();
        credential_repo.reset();
        database.reset();
        database = Database::open(dir / "service.sqlite3", {}).value();
        REQUIRE(initialize_schema(*database, clock->now()).has_value());
        const RepositoryContext ctx{database.get(), clock, ids};
        identities = std::make_shared<FlakyIdentityRepository>(std::make_shared<SqliteIdentityRepository>(ctx));
        credential_repo = std::make_shared<SqliteCredentialRepository>(ctx);
        identity_admin = std::make_unique<IdentityAdminService>(identities, codec, delivery);
        authenticator = std::make_unique<AccessTokenAuthenticator>(identities, codec);
        credentials = std::make_unique<CredentialService>(credential_repo, secrets, registry, clock);
    }

    std::string principal(std::string name = "user") {
        return unwrap(identity_admin->create_principal(std::move(name))).id;
    }

    ports::CredentialRecord cookie(const std::string& owner, std::string_view source, std::string_view value,
                                   std::string label = "账号") {
        return unwrap(credentials->create({owner, std::string{source}, std::move(label),
                                           ports::CredentialKind::cookie, "web_cookie",
                                           ports::SecretBytes{value}, std::nullopt}));
    }

    std::string read(const fs::path& path) { return unwrap(infrastructure::filesystem::read_file(path, 4096)); }

    std::size_t stored_secrets() { return unwrap(secrets->list()).size(); }

    fs::path dir;
    std::shared_ptr<infrastructure::ManualClock> clock = std::make_shared<infrastructure::ManualClock>();
    std::shared_ptr<infrastructure::observability::IdGenerator> ids =
        std::make_shared<infrastructure::observability::IdGenerator>(77);
    std::shared_ptr<infrastructure::auth::MbedTlsTokenCodec> codec =
        std::make_shared<infrastructure::auth::MbedTlsTokenCodec>();
    std::shared_ptr<infrastructure::auth::FileTokenDelivery> delivery =
        std::make_shared<infrastructure::auth::FileTokenDelivery>();
    std::shared_ptr<infrastructure::secrets::FileSecretStore> secrets =
        std::make_shared<infrastructure::secrets::FileSecretStore>(dir / "secrets");
    std::shared_ptr<const providers::ProviderRegistry> registry;
    std::function<void()> verify_hook;
    ports::CredentialVerification hooked_verdict{ports::VerificationStatus::valid, {}, {}, {}};

    std::unique_ptr<Database> database;
    std::shared_ptr<FlakyIdentityRepository> identities;
    std::shared_ptr<SqliteCredentialRepository> credential_repo;
    std::unique_ptr<IdentityAdminService> identity_admin;
    std::unique_ptr<AccessTokenAuthenticator> authenticator;
    std::unique_ptr<CredentialService> credentials;
};

std::string trimmed(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

ports::OperationContext context() { return {std::chrono::steady_clock::now() + std::chrono::seconds(5), {}}; }

} // namespace

TEST_CASE("access tokens are random, prefixed and hashed with SHA-256", "[accounts]") {
    infrastructure::auth::MbedTlsTokenCodec codec;
    std::set<std::string> seen;
    for (int i = 0; i < 50; ++i) {
        const auto token = unwrap(codec.generate());
        const auto text = std::string{token.reveal()};
        CHECK(text.rfind("mst_", 0) == 0);
        CHECK(text.size() == 4 + 43);
        CHECK(text.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_", 4) ==
              std::string::npos);
        seen.insert(text);
    }
    CHECK(seen.size() == 50);
    CHECK(codec.hash("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(codec.hash("abc") != codec.hash("abd"));
}

TEST_CASE("init delivers the bootstrap token once; SQLite stays the source of truth", "[accounts]") {
    Accounts a{"init"};
    const auto bootstrap = a.dir / "secrets" / "bootstrap-token.txt";
    const auto issued = unwrap(a.identity_admin->initialize(bootstrap));
    CHECK(issued.token.principal_id == local_principal_id);
    CHECK(issued.token.scopes.size() == all_scopes.size());
    const auto plaintext = trimmed(a.read(bootstrap));

    const auto session = unwrap(a.authenticator->authenticate(plaintext));
    REQUIRE(session);
    CHECK(session->principal.id == local_principal_id);
    CHECK(session->principal.role == ports::PrincipalRole::admin);
    CHECK(has_scope(session->token.scopes, scope::admin));

    // An initialized instance is never initialized again, and the file is untouched.
    CHECK(a.identity_admin->initialize(bootstrap).error().code == ErrorCode::conflict);
    CHECK(trimmed(a.read(bootstrap)) == plaintext);

    // A34: revoking the token is final even though the bootstrap file still exists.
    (void)unwrap(a.identity_admin->revoke_token(issued.token.id));
    a.reopen();
    CHECK_FALSE(unwrap(a.authenticator->authenticate(plaintext)));
    CHECK(fs::exists(bootstrap));
    CHECK(a.identity_admin->initialize(a.dir / "out" / "again.txt").error().code == ErrorCode::conflict);
    CHECK_FALSE(fs::exists(a.dir / "out" / "again.txt"));
}

TEST_CASE("an interrupted init resumes only while prn_local has no token", "[accounts]") {
    Accounts a{"init-resume"};
    (void)unwrap(a.identities->create_principal("local", ports::PrincipalRole::admin,
                                                std::string{local_principal_id}));
    const auto issued = unwrap(a.identity_admin->initialize(a.dir / "out" / "bootstrap.txt"));
    CHECK(issued.token.principal_id == local_principal_id);
    CHECK(a.identity_admin->initialize(a.dir / "out" / "second.txt").error().code == ErrorCode::conflict);
}

TEST_CASE("token issue never overwrites and an unstored token is not valid", "[accounts]") {
    Accounts a{"issue"};
    const auto prn = a.principal("客户端");
    const std::vector<std::string> read_only{std::string{scope::catalog_read}};

    // An existing output file is refused before anything is stored.
    const auto taken = a.dir / "out" / "taken.txt";
    REQUIRE(infrastructure::filesystem::write_new_file(taken, "keep", infrastructure::filesystem::Privacy::normal));
    CHECK_FALSE(a.identity_admin->issue_token(prn, read_only, taken).has_value());
    CHECK(a.read(taken) == "keep");
    CHECK(unwrap(a.identity_admin->list_tokens(prn)).empty());

    // A failed database write removes the delivered file; nothing authenticates.
    a.identities->fail_insert = true;
    const auto lost = a.dir / "out" / "lost.txt";
    CHECK(a.identity_admin->issue_token(prn, read_only, lost).error().code == ErrorCode::storage_unavailable);
    CHECK_FALSE(fs::exists(lost));
    CHECK(unwrap(a.identity_admin->list_tokens(prn)).empty());
    a.identities->fail_insert = false;

    // Revoking one token leaves the others working.
    const auto first = unwrap(a.identity_admin->issue_token(prn, read_only, a.dir / "out" / "one.txt"));
    const auto second = unwrap(a.identity_admin->issue_token(
        prn, {std::string{scope::downloads_read}, std::string{scope::downloads_write}}, a.dir / "out" / "two.txt"));
    const auto one = trimmed(a.read(first.delivered_to));
    const auto two = trimmed(a.read(second.delivered_to));
    CHECK(one != two);
    (void)unwrap(a.identity_admin->revoke_token(first.token.id));
    (void)unwrap(a.identity_admin->revoke_token(first.token.id)); // repeat succeeds
    CHECK_FALSE(unwrap(a.authenticator->authenticate(one)));
    const auto still = unwrap(a.authenticator->authenticate(two));
    REQUIRE(still);
    CHECK(still->token.scopes == std::vector<std::string>{"downloads:read", "downloads:write"});
    CHECK(a.identity_admin->revoke_token("tok_missing").error().code == ErrorCode::not_found);

    // Listing never exposes plaintext or hashes, only descriptions.
    const auto listed = unwrap(a.identity_admin->list_tokens(prn));
    REQUIRE(listed.size() == 2);
    CHECK(listed[0].revoked_at.has_value() != listed[1].revoked_at.has_value());

    // Disabling blocks access; enabling does not revive the revoked token.
    (void)unwrap(a.identity_admin->set_principal_enabled(prn, false));
    CHECK_FALSE(unwrap(a.authenticator->authenticate(two)));
    CHECK(a.identity_admin->issue_token(prn, read_only, a.dir / "out" / "three.txt").error().code ==
          ErrorCode::validation_failed);
    (void)unwrap(a.identity_admin->set_principal_enabled(prn, true));
    CHECK(unwrap(a.authenticator->authenticate(two)));
    CHECK_FALSE(unwrap(a.authenticator->authenticate(one)));

    // Scopes are explicit, known and unique; unknown principals fail.
    const auto out = a.dir / "out" / "bad.txt";
    CHECK(a.identity_admin->issue_token(prn, {}, out).error().code == ErrorCode::validation_failed);
    CHECK(a.identity_admin->issue_token(prn, {"catalog:write"}, out).error().code == ErrorCode::validation_failed);
    CHECK(a.identity_admin->issue_token(prn, {"admin", "admin"}, out).error().code == ErrorCode::validation_failed);
    CHECK(a.identity_admin->issue_token("prn_nobody", read_only, out).error().code == ErrorCode::not_found);
    CHECK_FALSE(fs::exists(out));
    CHECK_FALSE(unwrap(a.authenticator->authenticate("")));
    CHECK_FALSE(unwrap(a.authenticator->authenticate(std::string(300, 'x'))));
}

TEST_CASE("cookie credentials are checked against registered auth profiles", "[accounts]") {
    Accounts a{"create"};
    const auto prn = a.principal();
    const auto make = [&](std::string source, std::string profile, ports::CredentialKind kind, std::string value) {
        return a.credentials->create(
            {prn, std::move(source), "", kind, std::move(profile), ports::SecretBytes{value}, std::nullopt});
    };
    CHECK(make("ghost", "web_cookie", ports::CredentialKind::cookie, "k=v").error().code ==
          ErrorCode::unknown_provider);
    CHECK(make("aud", "app_token", ports::CredentialKind::cookie, "k=v").error().code ==
          ErrorCode::validation_failed);
    CHECK(make("aud", "web_cookie", ports::CredentialKind::api_key, "k=v").error().code ==
          ErrorCode::validation_failed);
    CHECK(make("oauthy", "web_oauth", ports::CredentialKind::oauth2, "token").error().code ==
          ErrorCode::capability_unsupported);
    CHECK(make("aud", "web_cookie", ports::CredentialKind::cookie, "").error().code ==
          ErrorCode::validation_failed);
    CHECK(make("aud", "web_cookie", ports::CredentialKind::cookie, "a=b\r\nX-Evil: 1").error().code ==
          ErrorCode::validation_failed);
    CHECK(make("aud", "web_cookie", ports::CredentialKind::cookie, std::string(64 * 1024 + 1, 'a')).error().code ==
          ErrorCode::validation_failed);
    CHECK(a.stored_secrets() == 0); // nothing was written for rejected input

    const auto created = a.cookie(prn, "aud", "session=1", "主账号");
    CHECK(created.version == 1);
    CHECK(created.kind == "cookie");
    CHECK(created.auth_profile == "web_cookie");
    CHECK(created.label == "主账号");
    CHECK(created.status == ports::CredentialStatus::active);
    CHECK(a.stored_secrets() == 1);
    CHECK(created.enabled);
    CHECK(unwrap(a.credentials->get_default(prn, "aud")).credential_id == created.id);
}

TEST_CASE("updates commit the new secret before removing the old one", "[accounts]") {
    Accounts a{"update"};
    const auto prn = a.principal();
    const auto created = a.cookie(prn, "aud", "session=1");
    const auto old_ref = *created.secret_ref;

    auto updated = unwrap(a.credentials->update(prn, created.id, {ports::SecretBytes{"session=2"}, {}, {}}));
    CHECK(updated.version == 2);
    CHECK(*updated.secret_ref != old_ref);
    CHECK(a.secrets->get(old_ref).error().code == ErrorCode::not_found);
    CHECK(a.stored_secrets() == 1);

    // A stale writer loses and its new secret is removed again; the current one stays.
    CHECK(a.credentials->update(prn, created.id, {ports::SecretBytes{"session=3"}, {}, 1}).error().code ==
          ErrorCode::version_conflict);
    CHECK(a.stored_secrets() == 1);
    CHECK(unwrap(a.secrets->get(*updated.secret_ref)).reveal() == "session=2");

    // A label-only change advances the version but keeps the secret.
    const auto relabeled = unwrap(a.credentials->update(prn, created.id, {{}, std::string{"新名字"}, {}}));
    CHECK(relabeled.version == 3);
    CHECK(relabeled.secret_ref == updated.secret_ref);
    CHECK(relabeled.label == "新名字");

    CHECK(a.credentials->update(prn, created.id, {}).error().code == ErrorCode::validation_failed);
    CHECK(a.credentials->update(prn, created.id, {ports::SecretBytes{"bad\n"}, {}, {}}).error().code ==
          ErrorCode::validation_failed);
    CHECK(a.credentials->update(a.principal("other"), created.id, {{}, std::string{"x"}, {}}).error().code ==
          ErrorCode::not_found);

    // Revoking removes the secret; repeating succeeds; updates are refused afterwards.
    const auto revoked = unwrap(a.credentials->revoke(prn, created.id));
    CHECK(revoked.status == ports::CredentialStatus::revoked);
    CHECK_FALSE(revoked.secret_ref);
    CHECK(a.stored_secrets() == 0);
    CHECK(unwrap(a.credentials->revoke(prn, created.id)).status == ports::CredentialStatus::revoked);
    CHECK(a.credentials->update(prn, created.id, {{}, std::string{"x"}, {}}).error().code ==
          ErrorCode::validation_failed);
}

TEST_CASE("verification results belong to the version that was checked", "[accounts]") {
    Accounts a{"verify"};
    const auto prn = a.principal();
    const auto good = a.cookie(prn, "aud", "good");
    const auto valid = unwrap(a.credentials->verify(prn, good.id, context()));
    CHECK(valid.verification.status == ports::VerificationStatus::valid);
    CHECK(valid.credential.last_verify_result == ports::VerifyOutcome::valid);
    CHECK(valid.credential.last_verified_at == a.clock->now());
    CHECK(valid.credential.version == good.version); // verification is not a new version

    const auto bad = a.cookie(prn, "aud", "bad");
    const auto invalid = unwrap(a.credentials->verify(prn, bad.id, context()));
    CHECK(invalid.credential.status == ports::CredentialStatus::invalid);
    // A new secret clears the old verdict.
    const auto replaced = unwrap(a.credentials->update(prn, bad.id, {ports::SecretBytes{"good"}, {}, {}}));
    CHECK(replaced.status == ports::CredentialStatus::active);
    CHECK_FALSE(replaced.last_verify_result);

    // No verifier bound: unsupported, not guessed.
    const auto combo = a.cookie(prn, "both", "x=1");
    const auto unsupported = unwrap(a.credentials->verify(prn, combo.id, context()));
    CHECK(unsupported.credential.last_verify_result == ports::VerifyOutcome::unknown);
    CHECK(unsupported.credential.verification_error == "CAPABILITY_UNSUPPORTED");

    // A39: the credential changes while a verification is in flight; the late result
    // is not attached to the new version.
    const auto racing = a.cookie(prn, "hooked", "v=1");
    // Fires once. The hook must not reset a.verify_hook itself: that would destroy the
    // closure while it runs.
    bool fired = false;
    a.verify_hook = [&] {
        if (std::exchange(fired, true)) {
            return;
        }
        (void)unwrap(a.credentials->update(prn, racing.id, {ports::SecretBytes{"v=2"}, {}, {}}));
    };
    CHECK(a.credentials->verify(prn, racing.id, context()).error().code == ErrorCode::version_conflict);
    const auto after = unwrap(a.credentials->get(prn, racing.id));
    CHECK(after.version == 2);
    CHECK_FALSE(after.last_verify_result);
}

TEST_CASE("verification evidence is stored with the checked version", "[accounts]") {
    Accounts a{"evidence"};
    const auto prn = a.principal();
    const auto cred = a.cookie(prn, "hooked", "v=1");
    const std::chrono::system_clock::time_point expiry =
        std::chrono::time_point_cast<std::chrono::milliseconds>(a.clock->now() + std::chrono::hours(24));
    a.hooked_verdict = {ports::VerificationStatus::valid, std::string{"nick"}, {"vip", "lossless"}, expiry};
    const std::vector<std::string> scopes{"vip", "lossless"};

    const auto verified = unwrap(a.credentials->verify(prn, cred.id, context()));
    CHECK(verified.credential.expires_at == expiry);
    CHECK(verified.credential.scopes == scopes);

    // Persisted, not just echoed.
    a.reopen();
    const auto stored = unwrap(a.credentials->get(prn, cred.id));
    CHECK(stored.expires_at == expiry);
    CHECK(stored.scopes == scopes);

    // Cookie 再次查询失败时，会员和 scope 证据不能继续沿用旧结果。
    a.hooked_verdict = {ports::VerificationStatus::unknown, {}, {}, {}};
    const auto unknown = unwrap(a.credentials->verify(prn, cred.id, context()));
    CHECK(unknown.credential.expires_at == expiry);
    CHECK(unknown.credential.scopes.empty());

    // A label change keeps the evidence; a new secret forgets it.
    CHECK(unwrap(a.credentials->update(prn, cred.id, {{}, std::string{"renamed"}, {}})).scopes.empty());
    const auto replaced = unwrap(a.credentials->update(prn, cred.id, {ports::SecretBytes{"v=2"}, {}, {}}));
    CHECK_FALSE(replaced.expires_at);
    CHECK(replaced.scopes.empty());

    // A39: evidence from a verification racing an update is dropped with the verdict.
    a.hooked_verdict = {ports::VerificationStatus::valid, {}, {"vip"}, expiry};
    bool fired = false;
    a.verify_hook = [&] {
        if (std::exchange(fired, true)) {
            return;
        }
        (void)unwrap(a.credentials->update(prn, cred.id, {ports::SecretBytes{"v=3"}, {}, {}}));
    };
    CHECK(a.credentials->verify(prn, cred.id, context()).error().code == ErrorCode::version_conflict);
    const auto after = unwrap(a.credentials->get(prn, cred.id));
    CHECK_FALSE(after.expires_at);
    CHECK(after.scopes.empty());
}

TEST_CASE("invalid or expired credentials are not selected or leased", "[accounts]") {
    Accounts a{"unusable"};
    const auto prn = a.principal();

    // Failed verification: no new default, binding or lease until it verifies again.
    const auto bad = a.cookie(prn, "aud", "bad");
    (void)unwrap(a.credentials->set_default(prn, "aud", bad.id));
    const auto bound = unwrap(a.credentials->resolve_binding(prn, "aud", "resolve_audio", std::nullopt)).binding;
    CHECK(unwrap(a.credentials->verify(prn, bad.id, context())).credential.status ==
          ports::CredentialStatus::invalid);
    CHECK(a.credentials->lease(bound).error().code == ErrorCode::credentials_required);
    CHECK(a.credentials->resolve_binding(prn, "aud", "resolve_audio", std::nullopt).error().code ==
          ErrorCode::credentials_required);
    CHECK(a.credentials->resolve_binding(prn, "aud", "resolve_audio", bad.id).error().code ==
          ErrorCode::validation_failed);
    (void)unwrap(a.credentials->clear_default(prn, "aud"));
    CHECK(a.credentials->set_default(prn, "aud", bad.id).error().code == ErrorCode::validation_failed);
    // The repository refuses it inside its own transaction too.
    CHECK(a.credential_repo->set_default(prn, "aud", bad.id).error().code == ErrorCode::validation_failed);
    // An optional capability falls back to anonymous rather than the invalid default.
    const auto combo = a.cookie(prn, "both", "bad");
    (void)unwrap(a.credentials->set_default(prn, "both", combo.id));
    (void)unwrap(a.credential_repo->record_verification(
        prn, combo.id, combo.version, {ports::VerifyOutcome::invalid, a.clock->now(), {}, {}}));
    CHECK(unwrap(a.credentials->resolve_binding(prn, "both", "resolve_audio", std::nullopt)).binding.mode() ==
          domain::AccountMode::anonymous);

    // A new secret returns it to active and the accepted binding leases again.
    (void)unwrap(a.credentials->set_default(prn, "aud", bad.id, ports::SecretBytes{"good"}));
    CHECK(unwrap(a.credentials->lease(bound)).secret->reveal() == "good");

    // Past its expiry the credential stops leasing, also for an accepted binding.
    const auto expiring = unwrap(a.credentials->create({prn, "aud", "label", ports::CredentialKind::cookie,
                                                        "web_cookie", ports::SecretBytes{"e=1"},
                                                        a.clock->now() + std::chrono::hours(1)}));
    (void)unwrap(a.credentials->set_default(prn, "aud", expiring.id));
    const auto expiring_binding =
        unwrap(a.credentials->resolve_binding(prn, "aud", "resolve_audio", std::nullopt)).binding;
    CHECK(unwrap(a.credentials->lease(expiring_binding)).secret->reveal() == "e=1");
    a.clock->advance(std::chrono::hours(1));
    CHECK(a.credentials->lease(expiring_binding).error().code == ErrorCode::credentials_required);
    CHECK(a.credentials->resolve_binding(prn, "aud", "resolve_audio", std::nullopt).error().code ==
          ErrorCode::credentials_required);
    CHECK(a.credentials->resolve_binding(prn, "aud", "resolve_audio", expiring.id).error().code ==
          ErrorCode::validation_failed);
    CHECK(a.credentials->set_default(prn, "aud", expiring.id).error().code == ErrorCode::validation_failed);
}

TEST_CASE("default selection is owned, versioned and survives restarts", "[accounts]") {
    Accounts a{"defaults"};
    const auto prn = a.principal();
    const auto other = a.principal("other");
    const auto first = a.cookie(prn, "aud", "a=1");
    const auto second = a.cookie(prn, "aud", "b=1");
    const auto foreign = a.cookie(other, "aud", "c=1");
    const auto wrong_source = a.cookie(prn, "both", "d=1");

    CHECK(unwrap(a.credentials->get_default(prn, "aud")).selection_version == 2);
    CHECK(unwrap(a.credentials->set_default(prn, "aud", second.id)).selection_version == 2); // no-op
    CHECK_FALSE(unwrap(a.credentials->get(prn, first.id)).enabled);
    CHECK(a.credentials->set_default(prn, "aud", first.id).error().code == ErrorCode::validation_failed);
    const auto back = unwrap(a.credentials->set_default(prn, "aud", first.id, ports::SecretBytes{"a=2"})); // A -> B -> A
    CHECK(back.selection_version == 3);
    CHECK(back.credential_id == first.id);

    CHECK(a.credentials->set_default(prn, "aud", foreign.id).error().code == ErrorCode::not_found);
    CHECK(a.credentials->set_default(prn, "aud", wrong_source.id).error().code == ErrorCode::validation_failed);
    CHECK(a.credentials->set_default(prn, "ghost", first.id).error().code == ErrorCode::unknown_provider);
    CHECK(a.credentials->get_default(prn, "Bad Source").error().code == ErrorCode::validation_failed);
    // Other owners have their own selection.
    CHECK(unwrap(a.credentials->get_default(other, "aud")).credential_id == foreign.id);

    a.reopen();
    CHECK(unwrap(a.credentials->get_default(prn, "aud")).credential_id == first.id);

    // Revoking the default clears it in the same commit and never picks the other account.
    (void)unwrap(a.credentials->revoke(prn, first.id));
    const auto cleared = unwrap(a.credentials->get_default(prn, "aud"));
    CHECK_FALSE(cleared.credential_id);
    CHECK(cleared.selection_version == 4);
    CHECK(a.credentials->set_default(prn, "aud", first.id).error().code == ErrorCode::validation_failed);

    (void)unwrap(a.credentials->set_default(prn, "aud", second.id, ports::SecretBytes{"b=2"}));
    CHECK(unwrap(a.credentials->clear_default(prn, "aud")).selection_version == 6);
    CHECK(unwrap(a.credentials->clear_default(prn, "aud")).selection_version == 6); // already empty
    // Clearing works for sources that are no longer registered.
    CHECK(unwrap(a.credentials->clear_default(prn, "removed_source")).selection_version == 0);
}

TEST_CASE("bindings come from the explicit credential, the default or anonymous mode", "[accounts]") {
    Accounts a{"bindings"};
    const auto prn = a.principal();
    const auto other = a.principal("other");

    // A capability without credentials is always anonymous.
    const auto catalog = unwrap(a.credentials->resolve_binding(prn, "cat", "search_tracks", std::nullopt));
    CHECK(catalog.binding.mode() == domain::AccountMode::anonymous);
    CHECK(catalog.binding.auth_profile() == anonymous_profile);
    CHECK_FALSE(catalog.selection_version);

    // Optional credentials without a default: anonymous, bound to the selection version.
    const auto optional_anon = unwrap(a.credentials->resolve_binding(prn, "both", "resolve_audio", std::nullopt));
    CHECK(optional_anon.binding.mode() == domain::AccountMode::anonymous);
    CHECK(optional_anon.selection_version == 0);

    // Required credentials without a default.
    CHECK(a.credentials->resolve_binding(prn, "aud", "resolve_audio", std::nullopt).error().code ==
          ErrorCode::credentials_required);

    const auto cred = a.cookie(prn, "aud", "a=1");
    (void)unwrap(a.credentials->set_default(prn, "aud", cred.id));
    const auto by_default = unwrap(a.credentials->resolve_binding(prn, "aud", "resolve_audio", std::nullopt));
    CHECK(by_default.binding.mode() == domain::AccountMode::user);
    CHECK(by_default.binding.credential_id() == cred.id);
    CHECK(by_default.binding.auth_profile() == "web_cookie");
    CHECK(by_default.selection_version == 1);

    // Explicit credentials must be the caller's, of this source and usable here.
    const auto explicit_binding = unwrap(a.credentials->resolve_binding(prn, "aud", "resolve_audio", cred.id));
    CHECK(explicit_binding.binding == by_default.binding);
    CHECK_FALSE(explicit_binding.selection_version);
    CHECK(a.credentials->resolve_binding(other, "aud", "resolve_audio", cred.id).error().code ==
          ErrorCode::not_found);
    CHECK(a.credentials->resolve_binding(prn, "both", "resolve_audio", cred.id).error().code ==
          ErrorCode::validation_failed);
    CHECK(a.credentials->resolve_binding(prn, "cat", "search_tracks", cred.id).error().code ==
          ErrorCode::validation_failed);

    CHECK(a.credentials->resolve_binding(prn, "aud", "search_tracks", std::nullopt).error().code ==
          ErrorCode::capability_unsupported);
    CHECK(a.credentials->resolve_binding(prn, "off", "resolve_audio", std::nullopt).error().code ==
          ErrorCode::provider_disabled);
    CHECK(a.credentials->resolve_binding(prn, "ghost", "resolve_audio", std::nullopt).error().code ==
          ErrorCode::unknown_provider);
}

TEST_CASE("leases follow the fixed binding, never another account", "[accounts]") {
    Accounts a{"leases"};
    const auto prn = a.principal();
    const auto other = a.principal("other");
    const auto first = a.cookie(prn, "aud", "a=1");
    const auto bound = unwrap(a.credentials->resolve_binding(prn, "aud", "resolve_audio", std::nullopt)).binding;
    const auto lease_first = unwrap(a.credentials->lease(bound));
    // A38: two version-1 credentials of one principal and source are different identities.
    const auto second = a.cookie(prn, "aud", "b=1");
    const auto lease_second =
        unwrap(a.credentials->lease(unwrap(a.credentials->resolve_binding(prn, "aud", "resolve_audio", second.id))
                                        .binding));
    CHECK(lease_first.identity.versions().credential_version == 1);
    CHECK(lease_second.identity.versions().credential_version == 1);
    CHECK_FALSE(lease_first.identity == lease_second.identity);
    CHECK(lease_first.identity.canonical() != lease_second.identity.canonical());
    CHECK(lease_first.secret->reveal() == "a=1");

    // Switching disables the old account; accepted bindings never move to another account.
    (void)unwrap(a.credentials->set_default(prn, "aud", second.id));
    CHECK(a.credentials->lease(bound).error().code == ErrorCode::credentials_required);
    CHECK(a.credentials->update(prn, first.id, {ports::SecretBytes{"a=2"}, {}, {}}).error().code == ErrorCode::validation_failed);

    // Reactivation requires a fresh secret; the fixed binding can lease that new version.
    (void)unwrap(a.credentials->set_default(prn, "aud", first.id, ports::SecretBytes{"a=2"}));
    const auto renewed = unwrap(a.credentials->lease(bound));
    CHECK(renewed.identity.versions().credential_version == 3);
    CHECK(renewed.secret->reveal() == "a=2");
    CHECK_FALSE(renewed.identity == lease_first.identity);

    // A06: another principal cannot lease the credential through a forged binding.
    const auto forged = unwrap(domain::AccountBinding::make(other, source("aud"), domain::AccountMode::user,
                                                            "web_cookie", first.id, std::nullopt));
    CHECK(a.credentials->lease(forged).error().code == ErrorCode::credentials_required);
    CHECK(unwrap(a.credentials->list(other, std::nullopt)).empty());
    CHECK(a.credentials->get(other, first.id).error().code == ErrorCode::not_found);

    // Revocation stops the binding; it does not fall back to the other account.
    (void)unwrap(a.credentials->revoke(prn, first.id));
    CHECK(a.credentials->lease(bound).error().code == ErrorCode::credentials_required);

    // Anonymous leases carry no secret.
    const auto anon = unwrap(a.credentials->lease(
        unwrap(a.credentials->resolve_binding(prn, "cat", "search_tracks", std::nullopt)).binding));
    CHECK_FALSE(anon.secret);
}

TEST_CASE("orphaned secrets are reclaimed", "[accounts]") {
    Accounts a{"orphans"};
    const auto prn = a.principal();
    const auto kept = a.cookie(prn, "aud", "a=1");
    const auto stray = unwrap(a.secrets->put(ports::SecretBytes{"left behind"}));
    CHECK(a.stored_secrets() == 2);
    CHECK(unwrap(a.credentials->reclaim_orphan_secrets()) == 1);
    CHECK(a.secrets->get(stray).error().code == ErrorCode::not_found);
    CHECK(unwrap(a.secrets->get(*kept.secret_ref)).reveal() == "a=1");
    CHECK(unwrap(a.credentials->reclaim_orphan_secrets()) == 0);
}

TEST_CASE("disabled accounts keep metadata but no secrets and require fresh activation", "[accounts][activation]") {
    Accounts a{"activation"};
    const auto owner = a.principal();
    const auto first = a.cookie(owner, "aud", "first=1");
    const auto second = unwrap(a.credentials->create({owner, "aud", "备用账号", ports::CredentialKind::cookie,
        "web_cookie", {}, {}, false}));
    CHECK_FALSE(second.enabled);
    CHECK_FALSE(second.secret_ref);
    CHECK(a.stored_secrets() == 1);
    CHECK(unwrap(a.credentials->get_default(owner, "aud")).credential_id == first.id);
    CHECK_FALSE(a.credentials->create({owner, "aud", "", ports::CredentialKind::cookie,
        "web_cookie", ports::SecretBytes{"discarded=1"}, {}, false}));
    CHECK(a.credentials->set_default(owner, "aud", second.id).error().code == ErrorCode::validation_failed);
    CHECK_FALSE(a.credentials->set_default(owner, "aud", second.id, ports::SecretBytes{"bad\n"}));
    CHECK(unwrap(a.credentials->get(owner, first.id)).enabled);
    CHECK(a.credentials->disable(a.principal("stranger"), first.id).error().code == ErrorCode::not_found);

    REQUIRE(a.credentials->set_default(owner, "aud", second.id, ports::SecretBytes{"second=1"}));
    const auto disabled = unwrap(a.credentials->get(owner, first.id));
    CHECK_FALSE(disabled.enabled);
    CHECK_FALSE(disabled.secret_ref);
    CHECK_FALSE(disabled.secret_cleanup_pending);
    CHECK(a.secrets->get(*first.secret_ref).error().code == ErrorCode::not_found);
    CHECK(a.stored_secrets() == 1);
    CHECK(a.credentials->resolve_binding(owner, "aud", "resolve_audio", first.id).error().code == ErrorCode::validation_failed);
    CHECK_FALSE(a.credentials->verify(owner, first.id, context()));
    CHECK_FALSE(a.credentials->update(owner, first.id, {ports::SecretBytes{"old=2"}, {}, {}}));
    CHECK(unwrap(a.credentials->update(owner, first.id, {{}, std::string{"已停用"}, {}})).label == "已停用");
    const auto selection = unwrap(a.credentials->get_default(owner, "aud"));
    REQUIRE(a.credentials->disable(owner, first.id)); // 不影响另一个已启用账号。
    CHECK(unwrap(a.credentials->get_default(owner, "aud")).selection_version == selection.selection_version);

    const auto stopped = unwrap(a.credentials->disable(owner, second.id));
    CHECK_FALSE(stopped.enabled);
    CHECK(unwrap(a.credentials->disable(owner, second.id)).version == stopped.version);
    CHECK_FALSE(unwrap(a.credentials->get_default(owner, "aud")).credential_id);
    CHECK(a.stored_secrets() == 0);
    CHECK(a.credentials->resolve_binding(owner, "aud", "resolve_audio", {}).error().code == ErrorCode::credentials_required);
    CHECK(unwrap(a.credentials->clear_default(owner, "aud")).selection_version == selection.selection_version + 1);
    a.reopen();
    CHECK_FALSE(unwrap(a.credentials->get(owner, first.id)).enabled);
    CHECK_FALSE(unwrap(a.credentials->get(owner, second.id)).enabled);
    CHECK(a.credentials->set_default(owner, "aud", second.id).error().code == ErrorCode::validation_failed);
    REQUIRE(a.credentials->set_default(owner, "aud", second.id, ports::SecretBytes{"second=2"}));
    REQUIRE(a.credentials->clear_default(owner, "aud"));
    CHECK(a.stored_secrets() == 0);
}

TEST_CASE("failed secret cleanup is durable visible and retried without reactivation", "[accounts][activation]") {
    Accounts a{"activation-cleanup"};
    const auto owner = a.principal();
    const auto saved = a.cookie(owner, "aud", "session=1");
    const auto binding = unwrap(a.credentials->resolve_binding(owner, "aud", "resolve_audio", {})).binding;
    auto store = std::make_shared<FailingSecretRemoval>(a.secrets);
    a.credentials = std::make_unique<CredentialService>(a.credential_repo, store, a.registry, a.clock);
    CHECK(a.credentials->disable(owner, saved.id).error().code == ErrorCode::storage_unavailable);
    auto disabled = unwrap(a.credentials->get(owner, saved.id));
    CHECK_FALSE(disabled.enabled);
    CHECK_FALSE(disabled.secret_ref);
    CHECK(disabled.secret_cleanup_pending);
    CHECK(a.credentials->lease(binding).error().code == ErrorCode::credentials_required);
    CHECK_FALSE(unwrap(a.credentials->get_default(owner, "aud")).credential_id);
    CHECK(a.stored_secrets() == 1);
    CHECK(a.credentials->set_default(owner, "aud", saved.id, ports::SecretBytes{"new=1"}).error().code == ErrorCode::storage_unavailable);
    CHECK(a.stored_secrets() == 1); // 清理失败时不写入新 Cookie。
    SECTION("idempotent retry") {
        store->fail = false;
        disabled = unwrap(a.credentials->disable(owner, saved.id));
    }
    SECTION("restart cleanup") {
        a.reopen();
        CHECK(unwrap(a.credentials->get(owner, saved.id)).secret_cleanup_pending);
        CHECK(unwrap(a.credentials->reclaim_orphan_secrets()) == 1);
        disabled = unwrap(a.credentials->get(owner, saved.id));
    }
    CHECK_FALSE(disabled.enabled);
    CHECK_FALSE(disabled.secret_cleanup_pending);
    CHECK(a.stored_secrets() == 0);
    CHECK(unwrap(a.credential_repo->pending_secret_deletions({}, {})).empty());
}

TEST_CASE("activation transaction rolls back the old account and cleanup queue on failure", "[accounts][activation]") {
    Accounts a{"activation-rollback"};
    const auto owner = a.principal();
    const auto first = a.cookie(owner, "aud", "first=1");
    const auto second = unwrap(a.credentials->create({owner, "aud", "second", ports::CredentialKind::cookie,
        "web_cookie", {}, {}, false}));
    REQUIRE(a.database->write([](Connection& db) {
        return db.exec("CREATE TRIGGER fail_enable BEFORE UPDATE OF enabled ON credentials WHEN NEW.enabled = 1 "
            "BEGIN SELECT RAISE(ABORT, 'activation failure'); END");
    }));
    CHECK_FALSE(a.credentials->set_default(owner, "aud", second.id, ports::SecretBytes{"second=1"}));
    CHECK(unwrap(a.credentials->get(owner, first.id)).enabled);
    CHECK(unwrap(a.credentials->get(owner, first.id)).version == first.version);
    CHECK_FALSE(unwrap(a.credentials->get(owner, second.id)).enabled);
    CHECK(unwrap(a.credentials->get_default(owner, "aud")).credential_id == first.id);
    CHECK(unwrap(a.credential_repo->pending_secret_deletions({}, {})).empty());
    CHECK(a.stored_secrets() == 1);
    CHECK(unwrap(a.secrets->get(*first.secret_ref)).reveal() == "first=1");
}

TEST_CASE("concurrent account switches leave one selected account and one secret", "[accounts][activation]") {
    Accounts a{"activation-concurrent"};
    const auto owner = a.principal();
    const auto first = unwrap(a.credentials->create({owner, "aud", "first", ports::CredentialKind::cookie,
        "web_cookie", {}, {}, false}));
    const auto second = unwrap(a.credentials->create({owner, "aud", "second", ports::CredentialKind::cookie,
        "web_cookie", {}, {}, false}));
    for (int round = 0; round < 8; ++round) {
        REQUIRE(a.credentials->clear_default(owner, "aud"));
        std::barrier gate{3};
        std::optional<domain::Result<ports::DefaultSelection>> results[2];
        std::jthread one{[&] { gate.arrive_and_wait(); results[0] = a.credentials->set_default(owner, "aud", first.id, ports::SecretBytes{"a=1"}); }};
        std::jthread two{[&] { gate.arrive_and_wait(); results[1] = a.credentials->set_default(owner, "aud", second.id, ports::SecretBytes{"b=1"}); }};
        gate.arrive_and_wait();
        one.join(); two.join();
        REQUIRE(results[0]->has_value()); REQUIRE(results[1]->has_value());
        const auto selected = unwrap(a.credentials->get_default(owner, "aud"));
        REQUIRE(selected.credential_id);
        const auto records = unwrap(a.credentials->list(owner, "aud"));
        CHECK(std::count_if(records.begin(), records.end(), [](const auto& record) { return record.enabled; }) == 1);
        for (const auto& record : records) {
            CHECK(record.enabled == (record.id == *selected.credential_id));
            CHECK(record.secret_ref.has_value() == record.enabled);
            CHECK_FALSE(record.secret_cleanup_pending);
        }
        CHECK(a.stored_secrets() == 1);
    }
}

namespace {
class DeferredVerificationExecutor final : public ports::WorkExecutor {
public:
    std::deque<Work> work;
    bool reject{false};
    domain::Result<void> submit(Work fn) override {
        if (reject) return domain::Error{ErrorCode::service_busy, "test queue full", true, {}};
        work.push_back(std::move(fn)); return domain::ok();
    }
    void next() {
        REQUIRE_FALSE(work.empty());
        auto fn = std::move(work.front()); work.pop_front(); fn({});
    }
};
std::shared_ptr<DeferredVerificationExecutor> enable_automatic_verification(Accounts& a,
    std::chrono::milliseconds budget = std::chrono::seconds(15)) {
    auto executor = std::make_shared<DeferredVerificationExecutor>();
    a.credentials = std::make_unique<CredentialService>(a.credential_repo, a.secrets, a.registry, a.clock, executor, budget);
    return executor;
}
}

TEST_CASE("cookie save automatically schedules and persists login and independent membership", "[accounts][membership]") {
    Accounts a{"automatic-membership"};
    auto executor = enable_automatic_verification(a);
    auto owner = a.principal();
    const auto expiry = a.clock->now() + std::chrono::hours(48);
    a.hooked_verdict.membership = {"active", "svip", "测试超级会员", expiry, {}, {}};
    auto pending = a.cookie(owner, "hooked", "session=1");
    CHECK(pending.verification_state == "pending");
    REQUIRE(pending.verification_id);
    CHECK_FALSE(pending.last_verify_result);
    CHECK(pending.membership.status == "unknown");
    CHECK(executor->work.size() == 1);
    CHECK(unwrap(a.credentials->schedule_verification(owner, pending.id)).verification_id == pending.verification_id);
    CHECK(executor->work.size() == 1);
    executor->next();
    auto done = unwrap(a.credentials->get(owner, pending.id));
    CHECK(done.verification_state == "complete");
    CHECK(done.last_verify_result == ports::VerifyOutcome::valid);
    CHECK(done.membership.type == "svip");
    CHECK(done.membership.expires_at == expiry);
    CHECK(done.membership.checked_at == a.clock->now());
    CHECK_FALSE(done.expires_at); // 会员有效期不是 Cookie 有效期。
    a.reopen();
    auto persisted = unwrap(a.credentials->get(owner, pending.id));
    CHECK(persisted.membership.expires_at == expiry);
    CHECK(persisted.verification_id == pending.verification_id);
    executor = enable_automatic_verification(a);
    a.verify_hook = [] { throw std::runtime_error{"private response must never be returned"}; };
    auto again = unwrap(a.credentials->schedule_verification(owner, pending.id));
    CHECK(again.membership.status == "unknown");
    executor->next();
    done = unwrap(a.credentials->get(owner, pending.id));
    CHECK(done.last_verify_result == ports::VerifyOutcome::unknown);
    CHECK(done.verification_error == "INTERNAL_ERROR");
    CHECK_FALSE(done.membership.expires_at);
}

TEST_CASE("cookie verification handles replacement cancellation and revocation races", "[accounts][membership]") {
    Accounts a{"membership-races"};
    auto executor = enable_automatic_verification(a);
    const auto owner = a.principal();
    auto first = a.cookie(owner, "hooked", "session=old");
    bool changed = false;
    a.verify_hook = [&] {
        if (!std::exchange(changed, true))
            REQUIRE(a.credentials->update(owner, first.id, {ports::SecretBytes{"session=new"}, {}, {}}));
    };
    executor->next();
    auto current = unwrap(a.credentials->get(owner, first.id));
    CHECK(current.version == 2);
    CHECK(current.verification_state == "pending");
    CHECK(current.verification_id != first.verification_id);
    executor->next();
    CHECK(unwrap(a.credentials->get(owner, first.id)).last_verify_result == ports::VerifyOutcome::valid);
    auto pending = unwrap(a.credentials->schedule_verification(owner, first.id));
    CHECK(a.credentials->cancel_verification("another-owner", first.id, *pending.verification_id).error().code == ErrorCode::not_found);
    auto canceled = unwrap(a.credentials->cancel_verification(owner, first.id, *pending.verification_id));
    CHECK(canceled.last_verify_result == ports::VerifyOutcome::unknown);
    CHECK(canceled.verification_error == "CANCELED");
    CHECK(unwrap(a.credentials->cancel_verification(owner, first.id, *pending.verification_id)).verification_error == "CANCELED");
    auto newer = unwrap(a.credentials->schedule_verification(owner, first.id));
    CHECK(a.credentials->cancel_verification(owner, first.id, *pending.verification_id).error().code == ErrorCode::version_conflict);
    executor->next(); // 已取消的旧队列不能完成新版校验。
    CHECK(unwrap(a.credentials->get(owner, first.id)).verification_id == newer.verification_id);
    CHECK(unwrap(a.credentials->get(owner, first.id)).verification_state == "pending");
    executor->next();
    REQUIRE(a.credentials->schedule_verification(owner, first.id));
    REQUIRE(a.credentials->revoke(owner, first.id));
    executor->next();
    auto revoked = unwrap(a.credentials->get(owner, first.id));
    CHECK(revoked.status == ports::CredentialStatus::revoked);
    CHECK(revoked.membership.status == "unknown");
}

TEST_CASE("automatic credential checks stay bounded and unknown on unsupported or overloaded platforms", "[accounts][membership]") {
    Accounts a{"membership-bounds"};
    auto executor = enable_automatic_verification(a);
    const auto owner = a.principal();
    for (int i = 0; i < 4; ++i) CHECK(a.cookie(owner, "aud", "good").verification_state == "pending");
    const auto overloaded = a.cookie(owner, "aud", "good");
    CHECK(overloaded.verification_state == "complete");
    CHECK(overloaded.verification_error == "SERVICE_BUSY");
    CHECK(overloaded.last_verify_result == ports::VerifyOutcome::unknown);
    CHECK(executor->work.size() == 4);
    const auto another = a.principal("another");
    const auto unsupported = a.cookie(another, "both", "session=1");
    while (!executor->work.empty()) executor->next();
    CHECK(unwrap(a.credentials->get(another, unsupported.id)).verification_error == "CAPABILITY_UNSUPPORTED");
    executor->reject = true;
    const auto full = a.cookie(owner, "aud", "good");
    CHECK(full.verification_error == "SERVICE_BUSY");
    CHECK(full.membership.status == "unknown");
}

TEST_CASE("switching or disabling cancels old verification without overwriting the new account", "[accounts][membership][activation]") {
    Accounts a{"activation-verification"};
    auto executor = enable_automatic_verification(a);
    const auto owner = a.principal();
    const auto first = a.cookie(owner, "hooked", "first=1");
    const auto second = a.cookie(owner, "hooked", "second=1");
    bool ran = false;
    a.verify_hook = [&] { ran = true; };
    executor->next();
    CHECK_FALSE(ran);
    CHECK(unwrap(a.credentials->get(owner, first.id)).verification_error == "ACCOUNT_DISABLED");
    CHECK(unwrap(a.credentials->get(owner, second.id)).verification_state == "pending");
    executor->next();
    CHECK(ran);
    REQUIRE(a.credentials->schedule_verification(owner, second.id));
    a.verify_hook = [&] { REQUIRE(a.credentials->disable(owner, second.id)); };
    executor->next();
    const auto stopped = unwrap(a.credentials->get(owner, second.id));
    CHECK_FALSE(stopped.enabled);
    CHECK(stopped.last_verify_result == ports::VerifyOutcome::unknown);
    CHECK(stopped.membership.status == "unknown");
    CHECK(stopped.verification_error == "ACCOUNT_DISABLED");
    CHECK(a.stored_secrets() == 0);
}

TEST_CASE("queued credential deadline and interrupted restart never retain stale success", "[accounts][membership]") {
    Accounts a{"membership-recovery"};
    auto executor = enable_automatic_verification(a, std::chrono::milliseconds(1));
    const auto owner = a.principal();
    const auto queued = a.cookie(owner, "hooked", "session=1");
    bool ran = false;
    a.verify_hook = [&] { ran = true; };
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    executor->next();
    CHECK_FALSE(ran);
    CHECK(unwrap(a.credentials->get(owner, queued.id)).verification_error == "REQUEST_DEADLINE_EXCEEDED");
    a.credentials.reset();
    a.credentials = std::make_unique<CredentialService>(a.credential_repo, a.secrets, a.registry, a.clock);
    auto interrupted = unwrap(a.credential_repo->begin_verification(owner, queued.id, 1));
    REQUIRE(interrupted.verification_state == "pending");
    a.reopen();
    REQUIRE(a.credential_repo->recover_verifications());
    auto recovered = unwrap(a.credentials->get(owner, queued.id));
    CHECK(recovered.verification_state == "complete");
    CHECK(recovered.last_verify_result == ports::VerifyOutcome::unknown);
    CHECK(recovered.verification_error == "INTERRUPTED");
    CHECK(recovered.membership.status == "unknown");
}

TEST_CASE("credential persistence failure is visible as unknown and can be retried", "[accounts][membership]") {
    Accounts a{"membership-storage-failure"};
    auto executor = enable_automatic_verification(a);
    const auto owner = a.principal();
    const auto record = a.cookie(owner, "hooked", "session=1");
    REQUIRE(a.database->write([](Connection& db) {
        return db.exec("CREATE TRIGGER fail_verify BEFORE UPDATE OF last_verified_at ON credentials "
            "BEGIN SELECT RAISE(ABORT, 'test storage failure'); END");
    }));
    CHECK_THROWS(executor->next());
    auto failed = unwrap(a.credentials->get(owner, record.id));
    CHECK(failed.verification_state == "complete");
    CHECK(failed.verification_error == "STORAGE_UNAVAILABLE");
    CHECK(failed.last_verify_result == ports::VerifyOutcome::unknown);
    CHECK(failed.membership.status == "unknown");
    CHECK(unwrap(a.credentials->list(owner, {})).at(0).verification_error == "STORAGE_UNAVAILABLE");
    REQUIRE(a.database->write([](Connection& db) { return db.exec("DROP TRIGGER fail_verify"); }));
    REQUIRE(a.credentials->schedule_verification(owner, record.id));
    executor->next();
    CHECK(unwrap(a.credentials->get(owner, record.id)).last_verify_result == ports::VerifyOutcome::valid);
}
