#include "service_harness.hpp"
#include "application/accounts/oauth_service.hpp"
#include "application/accounts/scopes.hpp"
#include "application/transport/url.hpp"
#include "infrastructure/auth/access_tokens.hpp"
#include "infrastructure/clock.hpp"
#include "infrastructure/secrets/file_secret_store.hpp"
#include "infrastructure/storage/sqlite/account_repositories.hpp"
#include "infrastructure/storage/sqlite/schema.hpp"
#include "providers/registry/provider_registry.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

using namespace music_service;
using namespace music_service::test;
using namespace music_service::application::accounts;
namespace sqlite = infrastructure::storage::sqlite;
using domain::ErrorCode;

namespace {
class Queue final : public ports::WorkExecutor {
public:
    bool reject{false};
    domain::Result<void> submit(Work work) override {
        std::lock_guard lock{mutex};
        if (reject || jobs.size() == 32) return domain::Error{ErrorCode::service_busy, "full", true, {}};
        jobs.push_back(std::move(work)); return domain::ok();
    }
    void run() {
        for (;;) {
            Work job;
            { std::lock_guard lock{mutex}; if (jobs.empty()) return; job = std::move(jobs.front()); jobs.pop_front(); }
            job({});
        }
    }
    std::mutex mutex;
    std::deque<Work> jobs;
};

class Protocol final : public ports::IOAuth2 {
public:
    domain::Result<std::string> authorize_url(std::string_view state, std::string_view challenge) const override {
        return "https://auth.example/authorize?state=" + std::string{state} + "&code_challenge=" + std::string{challenge};
    }
    domain::Result<ports::OAuthGrant> exchange(std::string_view, const ports::SecretBytes& code,
        const ports::SecretBytes& verifier, const ports::OperationContext& context) override {
        ++exchanges;
        { std::lock_guard lock{record_mutex};
          got_code = std::string{code.reveal()}; got_verifier = std::string{verifier.reveal()}; }
        if (hook) hook();
        if (throw_exchange) throw std::runtime_error("private-code-must-not-leak");
        if (context.stop.stop_requested()) return domain::Error{ErrorCode::canceled, "stopped", false, {}};
        return grant();
    }
    domain::Result<ports::OAuthGrant> refresh(const ports::CredentialSnapshot& snapshot,
        const ports::OperationContext&) override {
        ++refreshes; { std::lock_guard lock{record_mutex}; got_old = std::string{snapshot.secret.reveal()}; }
        if (hook) hook();
        if (fail_refresh) return domain::Error{ErrorCode::upstream_failed, "private-refresh-must-not-leak", true, {}};
        if (throw_refresh) throw std::runtime_error("private-refresh-must-not-leak");
        return grant("next-access", "next-refresh");
    }
    ports::OAuthGrant grant(std::string access = "old-access", std::string refresh = "old-refresh") const {
        return {ports::SecretBytes{json{{"access_token", access}, {"refresh_token", refresh}}.dump()}, lifetime, {"read"}};
    }
    std::atomic<int> exchanges{0}, refreshes{0};
    std::chrono::seconds lifetime{3600};
    bool fail_refresh{false}, throw_exchange{false}, throw_refresh{false};
    std::function<void()> hook;
    std::string got_code, got_verifier, got_old;
    std::mutex record_mutex;
};

ports::ProviderBundle oauth_bundle(const std::shared_ptr<Protocol>& protocol) {
    auto bundle = mock_catalog("oauthsrc");
    bundle.auth_profiles = {{.auth_profile = "web", .kind = ports::CredentialKind::oauth2,
        .input_fields = {{"access_token", true, true}, {"refresh_token", true, true}},
        .validate_secret = [](const ports::SecretBytes& secret) -> domain::Result<void> {
            auto body = json::parse(secret.reveal(), nullptr, false);
            if (!body.is_object() || !body.value("access_token", "").size() || !body.value("refresh_token", "").size())
                return domain::Error::validation("secret", "invalid token");
            return domain::ok();
        },
        .oauth = [protocol](const ports::ProviderSetup&) -> domain::Result<std::shared_ptr<ports::IOAuth2>> {
            return std::shared_ptr<ports::IOAuth2>{protocol};
        }}};
    return bundle;
}

std::string parameter(std::string_view url, std::string_view key) {
    const auto pos = url.find(std::string{key} + "=");
    if (pos == std::string_view::npos) return {};
    auto value = url.substr(pos + key.size() + 1);
    return std::string{value.substr(0, value.find('&'))};
}

struct Fixture {
    explicit Fixture(std::string_view name, OAuthLimits limits = {}) : deployment(name) {
        std::filesystem::create_directories(deployment.dir / "secrets");
        database = unwrap(sqlite::Database::open(deployment.dir / "oauth.sqlite3", {}));
        REQUIRE(sqlite::initialize_schema(*database, clock->now()));
        sqlite::RepositoryContext context{database.get(), clock, ids};
        identities = std::make_shared<sqlite::SqliteIdentityRepository>(context);
        repository = std::make_shared<sqlite::SqliteCredentialRepository>(context);
        secrets = std::make_shared<infrastructure::secrets::FileSecretStore>(deployment.dir / "secrets");
        app = std::make_shared<ports::OAuthApplication>(ports::OAuthApplication{"app", "oauthsrc", "web", 1,
            "client", "https://service.example/api/v1/oauth/callback/app", std::make_shared<ports::SecretBytes>("app-secret")});
        application::config::ProviderConfig config; config.app_profile_id = "app";
        registry = unwrap(providers::ProviderRegistry::build({oauth_bundle(protocol)}, {{"oauthsrc", config}},
            {nullptr, {{"app", app}}}));
        credentials = std::make_shared<CredentialService>(repository, secrets, registry, clock);
        owner = unwrap(identities->create_principal("owner", ports::PrincipalRole::user, {})).id;
        token = unwrap(codec->generate());
        token_record = unwrap(identities->insert_token(owner, codec->hash(token.reveal()), {std::string{scope::credentials_write}}));
        oauth = std::make_unique<OAuthService>(OAuthService::Applications{{"app", app}}, registry, credentials,
            repository, identities, codec, clock, queue, limits);
    }
    ~Fixture() { oauth->stop(); queue->run(); }
    OAuthSessionCreated create() { return unwrap(oauth->create(owner, token.reveal(), "oauthsrc", "web", "账号")); }
    struct Login { OAuthSessionCreated created; OAuthBrowserStart browser; std::string state; };
    Login start() {
        auto c = create();
        auto browser = unwrap(oauth->authorize(c.session.id, parameter(c.authorization_url, "ticket")));
        auto state = parameter(browser.authorization_url, "state");
        return {std::move(c), std::move(browser), state};
    }
    void callback(const Login& login) { REQUIRE(oauth->callback("app", login.state, login.browser.cookie, ports::SecretBytes{"test-code"})); }
    ports::CredentialRecord login() {
        auto started = start(); callback(started); queue->run();
        auto view = unwrap(oauth->get(owner, started.created.session.id));
        REQUIRE(view.status == "succeeded"); REQUIRE(view.credential_id);
        return unwrap(credentials->get(owner, *view.credential_id));
    }
    ports::OperationContext context() { return {clock->steady_now() + 30s, {}}; }
    Deployment deployment;
    std::shared_ptr<infrastructure::ManualClock> clock = std::make_shared<infrastructure::ManualClock>();
    std::shared_ptr<infrastructure::observability::IdGenerator> ids = std::make_shared<infrastructure::observability::IdGenerator>(123);
    std::shared_ptr<infrastructure::auth::MbedTlsTokenCodec> codec = std::make_shared<infrastructure::auth::MbedTlsTokenCodec>();
    std::unique_ptr<sqlite::Database> database;
    std::shared_ptr<sqlite::SqliteIdentityRepository> identities;
    std::shared_ptr<sqlite::SqliteCredentialRepository> repository;
    std::shared_ptr<infrastructure::secrets::FileSecretStore> secrets;
    std::shared_ptr<Protocol> protocol = std::make_shared<Protocol>();
    std::shared_ptr<ports::OAuthApplication> app;
    std::shared_ptr<const providers::ProviderRegistry> registry;
    std::shared_ptr<CredentialService> credentials;
    std::shared_ptr<Queue> queue = std::make_shared<Queue>();
    std::unique_ptr<OAuthService> oauth;
    std::string owner;
    ports::SecretBytes token;
    ports::AccessTokenRecord token_record;
};
}

TEST_CASE("OAuth uses RFC 7636 S256 and strict app callback addresses", "[oauth]") {
    infrastructure::auth::MbedTlsTokenCodec crypto;
    CHECK(unwrap(crypto.pkce_challenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk")) == "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
    CHECK(validate_oauth_redirect("app", "https://service.example/api/v1/oauth/callback/app"));
    CHECK(validate_oauth_redirect("app", "http://127.0.0.1:123/api/v1/oauth/callback/app"));
    for (const auto* uri : {"http://service.example/api/v1/oauth/callback/app", "https://service.example/callback",
        "https://service.example/api/v1/oauth/callback/app?x=1", "https://service.example/api/v1/oauth/callback/app#fragment",
        "https://user:pass@service.example/api/v1/oauth/callback/app", "http://localhost/api/v1/oauth/callback/app"})
        CHECK_FALSE(validate_oauth_redirect("app", uri));
}

TEST_CASE("OAuth login binds browser owner app and PKCE and consumes the code once", "[oauth][accounts]") {
    Fixture f{"oauth-login"}; auto started = f.start();
    CHECK(started.browser.cookie.find("HttpOnly; SameSite=Lax") != std::string::npos);
    CHECK(started.browser.cookie.find("Secure") != std::string::npos);
    CHECK_FALSE(f.oauth->authorize(started.created.session.id, parameter(started.created.authorization_url, "ticket")));
    CHECK_FALSE(f.oauth->callback("app", started.state, "", ports::SecretBytes{"test-code"}));
    CHECK_FALSE(f.oauth->callback("another-app", started.state, started.browser.cookie, ports::SecretBytes{"test-code"}));
    CHECK_FALSE(f.oauth->get("another-owner", started.created.session.id));
    CHECK_FALSE(f.oauth->cancel("another-owner", started.created.session.id));
    f.callback(started);
    CHECK(f.protocol->exchanges == 0);
    CHECK_FALSE(f.oauth->callback("app", started.state, started.browser.cookie, ports::SecretBytes{"test-code"}));
    f.queue->run();
    auto view = unwrap(f.oauth->get(f.owner, started.created.session.id));
    REQUIRE(view.status == "succeeded"); REQUIRE(view.credential_id);
    auto record = unwrap(f.credentials->get(f.owner, *view.credential_id));
    CHECK(record.oauth_application_id == "app"); CHECK(record.expires_at == f.clock->now() + 3600s);
    CHECK(record.scopes == std::vector<std::string>{"read"});
    CHECK(f.protocol->got_code == "test-code");
    CHECK(unwrap(f.codec->pkce_challenge(f.protocol->got_verifier)) == parameter(started.browser.authorization_url, "code_challenge"));
    CHECK(unwrap(f.credentials->get_default(f.owner, "oauthsrc")).credential_id == record.id);
}

TEST_CASE("OAuth sessions are bounded cancellable expiring and authorization aware", "[oauth][accounts]") {
    Fixture f{"oauth-expiry", OAuthLimits{2, 1}};
    auto started = f.start();
    CHECK_FALSE(f.oauth->create(f.owner, f.token.reveal(), "oauthsrc", "web", "second"));
    SECTION("expiry and cleanup") {
        f.clock->advance(601s);
        CHECK(unwrap(f.oauth->get(f.owner, started.created.session.id)).status == "expired");
        CHECK_FALSE(f.oauth->callback("app", started.state, started.browser.cookie, ports::SecretBytes{"code"}));
        f.clock->advance(61s); f.oauth->maintain(); f.queue->run();
        CHECK(f.oauth->create(f.owner, f.token.reveal(), "oauthsrc", "web", "new"));
    }
    SECTION("cancel queued exchange") {
        f.callback(started); CHECK(unwrap(f.oauth->cancel(f.owner, started.created.session.id)).status == "canceled");
        f.queue->run(); CHECK(f.protocol->exchanges == 0);
    }
    SECTION("queue deadline prevents dispatch") {
        f.callback(started); f.clock->advance(31s); f.queue->run(); CHECK(f.protocol->exchanges == 0);
    }
    SECTION("provider denial") {
        REQUIRE(f.oauth->callback("app", started.state, started.browser.cookie, {}, "access_denied"));
        CHECK(unwrap(f.oauth->get(f.owner, started.created.session.id)).status == "denied");
    }
    SECTION("provider outage is a failure rather than user denial") {
        REQUIRE(f.oauth->callback("app", started.state, started.browser.cookie, {}, "server_error"));
        CHECK(unwrap(f.oauth->get(f.owner, started.created.session.id)).status == "failed");
        CHECK(unwrap(f.oauth->get(f.owner, started.created.session.id)).error_code == "OAUTH_AUTHORIZATION_FAILED");
    }
    SECTION("revoked service token wins exchange") {
        f.protocol->hook = [&] { REQUIRE(f.identities->revoke_token(f.token_record.id)); };
        f.callback(started); f.queue->run();
        CHECK(unwrap(f.oauth->get(f.owner, started.created.session.id)).status == "failed");
    }
    SECTION("disabled principal wins exchange") {
        f.protocol->hook = [&] { REQUIRE(f.identities->set_principal_enabled(f.owner, false)); };
        f.callback(started); f.queue->run();
        CHECK(unwrap(f.oauth->get(f.owner, started.created.session.id)).status == "failed");
    }
    SECTION("revoked token cancels queued exchange before upstream access") {
        f.callback(started); REQUIRE(f.identities->revoke_token(f.token_record.id)); f.queue->run();
        CHECK(f.protocol->exchanges == 0);
        CHECK(unwrap(f.oauth->get(f.owner, started.created.session.id)).status == "failed");
    }
    SECTION("cancel while provider is running discards late grant") {
        f.protocol->hook = [&] { REQUIRE(f.oauth->cancel(f.owner, started.created.session.id)); };
        f.callback(started); f.queue->run();
        CHECK(unwrap(f.oauth->get(f.owner, started.created.session.id)).status == "canceled");
    }
    SECTION("provider exceptions remain private") {
        f.protocol->throw_exchange = true; f.callback(started); f.queue->run();
        CHECK(unwrap(f.oauth->get(f.owner, started.created.session.id)).error_code == "OAUTH_EXCHANGE_FAILED");
    }
    SECTION("full queue consumes callback without upstream replay") {
        f.queue->reject = true;
        CHECK_FALSE(f.oauth->callback("app", started.state, started.browser.cookie, ports::SecretBytes{"code"}));
        f.queue->reject = false;
        CHECK_FALSE(f.oauth->callback("app", started.state, started.browser.cookie, ports::SecretBytes{"code"}));
    }
    CHECK(unwrap(f.credentials->list(f.owner, {})).empty());
}

TEST_CASE("OAuth automatic refresh rotates secrets atomically and preserves reserved app secrets", "[oauth][accounts]") {
    Fixture f{"oauth-rotation"}; auto old = f.login();
    auto app_ref = unwrap(f.secrets->put(*f.app->client_secret));
    auto orphan = unwrap(f.secrets->put(ports::SecretBytes{"orphan"}));
    CHECK(unwrap(f.credentials->reclaim_orphan_secrets({app_ref})) == 1);
    CHECK(f.secrets->get(app_ref)); CHECK_FALSE(f.secrets->get(orphan));
    f.clock->advance(3550s);
    for (int n = 0; n < 10; ++n) f.oauth->maintain();
    f.queue->run();
    auto now = unwrap(f.credentials->get(f.owner, old.id));
    CHECK(f.protocol->refreshes == 1); CHECK(now.version == old.version + 1);
    CHECK(now.refresh_state == "idle"); CHECK(now.expires_at == f.clock->now() + 3600s);
    CHECK_FALSE(f.secrets->get(*old.secret_ref));
    CHECK(unwrap(f.secrets->get(*now.secret_ref)).reveal().find("next-refresh") != std::string_view::npos);
    CHECK(f.protocol->got_old.find("old-refresh") != std::string::npos);
}

TEST_CASE("OAuth refresh failures and interrupted refreshes cannot replay rotating tokens", "[oauth][accounts]") {
    Fixture f{"oauth-failure"}; auto old = f.login();
    SECTION("upstream failure") { f.protocol->fail_refresh = true; CHECK_FALSE(f.credentials->refresh_oauth(old, *f.app, f.context())); }
    SECTION("exception") { f.protocol->throw_refresh = true; CHECK_FALSE(f.credentials->refresh_oauth(old, *f.app, f.context())); }
    SECTION("deadline") { f.protocol->hook = [&] { f.clock->advance(31s); }; CHECK_FALSE(f.credentials->refresh_oauth(old, *f.app, f.context())); }
    SECTION("storage failure after token rotation") {
        REQUIRE(f.database->write([](sqlite::Connection& db) { return db.exec(
            "CREATE TRIGGER fail_token BEFORE UPDATE OF secret_ref ON credentials WHEN NEW.secret_ref != OLD.secret_ref BEGIN SELECT RAISE(ABORT, 'test failure'); END"); }));
        CHECK_FALSE(f.credentials->refresh_oauth(old, *f.app, f.context()));
        CHECK(unwrap(f.secrets->list()).size() == 1);
    }
    SECTION("process restart") {
        REQUIRE(f.repository->begin_refresh(f.owner, old.id, old.version));
        auto database = unwrap(sqlite::Database::open(f.deployment.dir / "oauth.sqlite3", {}));
        auto reopened = std::make_shared<sqlite::SqliteCredentialRepository>(sqlite::RepositoryContext{database.get(), f.clock, f.ids});
        REQUIRE(unwrap(reopened->get(f.owner, old.id)).refresh_state == "pending");
        REQUIRE(reopened->recover_refreshes());
    }
    SECTION("disabled owner wins refresh commit") {
        f.protocol->hook = [&] { REQUIRE(f.identities->set_principal_enabled(f.owner, false)); };
        CHECK_FALSE(f.credentials->refresh_oauth(old, *f.app, f.context()));
    }
    auto now = unwrap(f.credentials->get(f.owner, old.id));
    CHECK(now.status == ports::CredentialStatus::invalid); CHECK(now.refresh_state == "reauthorization_required");
    const auto calls = f.protocol->refreshes.load();
    CHECK_FALSE(f.credentials->refresh_oauth(now, *f.app, f.context()));
    f.clock->advance(3601s); f.oauth->maintain(); f.queue->run(); CHECK(f.protocol->refreshes == calls);
}

TEST_CASE("OAuth short token lifetime does not cause continuous refresh", "[oauth][accounts]") {
    Fixture f{"oauth-short-token"}; f.protocol->lifetime = 30s;
    auto old = f.login();
    CHECK(old.refresh_after == f.clock->now() + 27s);
    f.oauth->maintain(); f.queue->run(); CHECK(f.protocol->refreshes == 0);
    f.clock->advance(27s); f.oauth->maintain(); f.queue->run(); CHECK(f.protocol->refreshes == 1);
    f.clock->advance(5s); f.oauth->maintain(); f.queue->run(); CHECK(f.protocol->refreshes == 1);
}

TEST_CASE("OAuth refresh never overwrites manual update revoke or another refresh", "[oauth][accounts]") {
    Fixture f{"oauth-races"}; auto old = f.login();
    SECTION("new secret wins") {
        f.protocol->hook = [&] { REQUIRE(f.credentials->update(f.owner, old.id,
            {ports::SecretBytes{R"({"access_token":"manual","refresh_token":"manual"})"}, {}, old.version})); };
        CHECK_FALSE(f.credentials->refresh_oauth(old, *f.app, f.context()));
        auto now = unwrap(f.credentials->get(f.owner, old.id));
        CHECK(now.status == ports::CredentialStatus::active); CHECK_FALSE(now.oauth_application_id);
        CHECK(unwrap(f.secrets->get(*now.secret_ref)).reveal().find("manual") != std::string_view::npos);
    }
    SECTION("label change cannot strand pending refresh") {
        f.protocol->hook = [&] { REQUIRE(f.credentials->update(f.owner, old.id, {{}, "new label", old.version})); };
        CHECK_FALSE(f.credentials->refresh_oauth(old, *f.app, f.context()));
        CHECK(unwrap(f.credentials->get(f.owner, old.id)).refresh_state == "reauthorization_required");
    }
    SECTION("revoke wins") {
        f.protocol->hook = [&] { REQUIRE(f.credentials->revoke(f.owner, old.id)); };
        CHECK_FALSE(f.credentials->refresh_oauth(old, *f.app, f.context()));
        CHECK(unwrap(f.credentials->get(f.owner, old.id)).status == ports::CredentialStatus::revoked);
        CHECK(unwrap(f.secrets->list()).empty());
    }
    SECTION("simultaneous refresh uses the old token once") {
        std::promise<void> entered, release;
        auto wait = release.get_future();
        f.protocol->hook = [&] { entered.set_value(); (void)wait.wait_for(5s); };
        bool first_ok = false;
        std::jthread first([&] { first_ok = f.credentials->refresh_oauth(old, *f.app, f.context()).has_value(); });
        REQUIRE(entered.get_future().wait_for(5s) == std::future_status::ready);
        auto second = f.credentials->refresh_oauth(old, *f.app, f.context());
        release.set_value(); first.join();
        CHECK_FALSE(second); CHECK(first_ok); CHECK(f.protocol->refreshes == 1);
    }
    SECTION("changed application cannot consume old refresh token") {
        auto changed = *f.app; ++changed.version;
        CHECK_FALSE(f.credentials->refresh_oauth(old, changed, f.context())); CHECK(f.protocol->refreshes == 0);
    }
}

TEST_CASE("OAuth HTTP login callback polling and offline secret setup work through the process host", "[oauth][server]") {
    Deployment d{"oauth-http"}; auto protocol = std::make_shared<Protocol>();
    server::CommandHooks hooks; hooks.host.bundles = std::vector<ports::ProviderBundle>{oauth_bundle(protocol)};
    REQUIRE(d.run({"init"}, hooks).code == 0);
    const auto secret_file = d.dir / "client-secret.txt";
    REQUIRE(infrastructure::filesystem::write_new_file(secret_file, "application-private", infrastructure::filesystem::Privacy::owner_only));
    auto imported = d.run({"secret-import", "--input", infrastructure::filesystem::path_to_utf8(secret_file)}, hooks);
    REQUIRE(imported.code == 0); CHECK(imported.out.find("application-private") == std::string::npos);
    auto ref = imported.out; while (!ref.empty() && (ref.back() == '\n' || ref.back() == '\r')) ref.pop_back();
    d.config["app_profiles"]["app"] = {{"source", "oauthsrc"}, {"auth_profile", "web"}, {"client_id", "client"},
        {"client_secret_ref", ref}, {"redirect_uri", "http://127.0.0.1/api/v1/oauth/callback/app"}};
    d.config["providers"]["oauthsrc"]["app_profile_id"] = "app";
    d.write_config();
    REQUIRE(d.run({"check-config"}, hooks).code == 0);
    auto sink = std::make_shared<infrastructure::observability::MemorySink>();
    RunningService server{d, sink, std::vector<ports::ProviderBundle>{oauth_bundle(protocol)}};
    auto api = server.client(d.read(d.bootstrap())); auto browser = server.client();
    auto request = R"({"source":"oauthsrc","auth_profile":"web","label":"HTTP account"})";
    CHECK(browser.Post("/api/v1/oauth/sessions", request, "application/json")->status == 401);
    auto created = api.Post("/api/v1/oauth/sessions", request, "application/json");
    REQUIRE(created); REQUIRE(created->status == 201);
    auto data = body(created)["data"]; const auto id = data["session_id"].get<std::string>();
    auto start = browser.Get(unwrap(application::transport::Url::parse(data["authorization_url"].get<std::string>())).target());
    REQUIRE(start); REQUIRE(start->status == 303);
    CHECK(start->get_header_value("Cache-Control") == "no-store");
    CHECK(start->get_header_value("Referrer-Policy") == "no-referrer");
    const auto state = parameter(start->get_header_value("Location"), "state");
    const auto callback = "/api/v1/oauth/callback/app?state=" + state + "&code=test-code";
    CHECK(browser.Get(callback)->status == 404);
    browser.set_default_headers({{"Cookie", start->get_header_value("Set-Cookie")}});
    CHECK(browser.Get(callback + "&state=" + state)->status == 400);
    CHECK(browser.Get(callback + "&st%61te=different")->status == 400);
    CHECK(browser.Get(callback + "&code=another")->status == 400);
    CHECK(browser.Get(callback + "&error=access_denied")->status == 400);
    REQUIRE(browser.Get(callback)->status == 202);
    CHECK(browser.Get(callback)->status == 409);
    json polled;
    for (int i = 0; i < 200; ++i) {
        polled = body(api.Get("/api/v1/oauth/sessions/" + id))["data"];
        if (polled["status"] != "exchanging") break;
        std::this_thread::sleep_for(10ms);
    }
    CHECK(polled["status"] == "succeeded");
    CHECK_FALSE(polled.contains("authorization_url"));
    auto credentials = body(api.Get("/api/v1/credentials"));
    CHECK(credentials.dump().find("old-access") == std::string::npos);
    CHECK(credentials.dump().find("old-refresh") == std::string::npos);
    CHECK(api.Get("/health/ready")->status == 200);
    // 占满两个 OAuth worker，并让其余会话排队；控制接口和取消仍能及时完成。
    std::promise<void> release;
    auto released = release.get_future().share();
    std::atomic<int> blocked{0};
    protocol->hook = [&] { ++blocked; (void)released.wait_for(5s); };
    std::vector<std::string> busy_ids;
    for (int i = 0; i < 7; ++i) {
        auto next = body(api.Post("/api/v1/oauth/sessions", request, "application/json"))["data"];
        const auto next_id = next["session_id"].get<std::string>(); busy_ids.push_back(next_id);
        auto url = unwrap(application::transport::Url::parse(next["authorization_url"].get<std::string>()));
        auto redirect = browser.Get(url.target()); REQUIRE(redirect); REQUIRE(redirect->status == 303);
        browser.set_default_headers({{"Cookie", redirect->get_header_value("Set-Cookie")}});
        REQUIRE(browser.Get("/api/v1/oauth/callback/app?state=" + parameter(redirect->get_header_value("Location"), "state") + "&code=slow-code")->status == 202);
    }
    for (int i = 0; i < 200 && blocked < 2; ++i) std::this_thread::sleep_for(5ms);
    CHECK(blocked == 2);
    const auto control_start = std::chrono::steady_clock::now();
    CHECK(api.Post("/api/v1/oauth/sessions", request, "application/json")->status == 503);
    CHECK(api.Get("/health/ready")->status == 200);
    CHECK(api.Get("/api/v1/admin/status")->status == 200);
    for (const auto& busy_id : busy_ids) CHECK(api.Delete("/api/v1/oauth/sessions/" + busy_id)->status == 200);
    CHECK(std::chrono::steady_clock::now() - control_start < 2s);
    release.set_value();
    CHECK(server.stop().code == 0);
    for (const auto& record : sink->records()) {
        std::ostringstream log;
        infrastructure::observability::JsonLineSink encoded{log}; encoded.write(record);
        const auto serialized = log.str();
        CHECK(serialized.find("old-access") == std::string::npos);
        CHECK(serialized.find("test-code") == std::string::npos);
        CHECK(serialized.find(state) == std::string::npos);
        CHECK(serialized.find("application-private") == std::string::npos);
    }
    infrastructure::secrets::FileSecretStore store{d.dir / "data" / "secrets" / "credentials"};
    CHECK(store.get(ref));
}
