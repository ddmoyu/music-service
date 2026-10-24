#include "application/accounts/oauth_service.hpp"
#include "application/accounts/scopes.hpp"
#include "application/ports/runtime/scope_exit.hpp"
#include "application/transport/url.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <set>

namespace music_service::application::accounts {
namespace {
using domain::Error;
using domain::ErrorCode;
using namespace std::chrono_literals;
Error missing() { return {ErrorCode::not_found, "OAuth session was not found", false, {}}; }
Error conflict() { return {ErrorCode::conflict, "OAuth session is no longer available", false, {}}; }
Error busy() { return {ErrorCode::service_busy, "OAuth capacity is not available", true, {{"retry_after_ms", "1000"}}}; }
bool terminal(std::string_view s) { return s != "pending" && s != "exchanging"; }
bool opaque(std::string_view s) {
    return s.size() >= 32 && s.size() <= 128 && std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '~';
    });
}
bool cookie_matches(std::string_view header, std::string_view name, std::string_view hash,
                    const ports::AccessTokenCodec& crypto) {
    unsigned matches = 0;
    bool valid = false;
    while (!header.empty()) {
        const auto end = header.find(';');
        auto item = header.substr(0, end);
        while (!item.empty() && item.front() == ' ') item.remove_prefix(1);
        const auto eq = item.find('=');
        if (eq != std::string_view::npos && item.substr(0, eq) == name) {
            ++matches;
            auto value = item.substr(eq + 1);
            valid = opaque(value) && crypto.hash(value) == hash;
        }
        if (end == std::string_view::npos) break;
        header.remove_prefix(end + 1);
    }
    return matches == 1 && valid;
}
}

domain::Result<void> validate_oauth_redirect(std::string_view id, std::string_view uri) {
    const bool valid_id = !id.empty() && id.size() <= 64 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
    auto url = transport::Url::parse(uri);
    if (!valid_id || !url || uri.find_first_of("?#") != std::string_view::npos ||
        url->path() != "/api/v1/oauth/callback/" + std::string{id} ||
        (!url->is_https() && url->host() != "127.0.0.1" && url->host() != "::1"))
        return Error::validation("redirect_uri", "must be HTTPS (or loopback HTTP) with path /api/v1/oauth/callback/{application_id}");
    return domain::ok();
}

struct OAuthService::State {
    struct Session {
        std::mutex mutex;
        OAuthSessionView view;
        std::string owner, label, token_hash, state_hash, ticket_hash, cookie_hash;
        ports::SecretBytes verifier, provider_url, code;
        std::shared_ptr<const ports::OAuthApplication> app;
        std::shared_ptr<ports::IOAuth2> protocol;
        std::chrono::steady_clock::time_point deadline;
        std::chrono::steady_clock::time_point exchange_deadline;
        bool started{false}, running{false};
        std::stop_source stop;
    };
    Applications applications;
    std::shared_ptr<const ports::ProviderDirectory> providers;
    std::shared_ptr<CredentialService> credentials;
    std::shared_ptr<ports::CredentialRepository> repository;
    std::shared_ptr<ports::IdentityRepository> identities;
    std::shared_ptr<ports::AccessTokenCodec> crypto;
    std::shared_ptr<const ports::Clock> clock;
    OAuthLimits limits;
    std::mutex mutex;
    std::map<std::string, std::shared_ptr<Session>> sessions;
    std::atomic<bool> stopping{false}, scanning{false};
    std::chrono::steady_clock::time_point next_scan{};

    std::shared_ptr<Session> find(const std::string& id) {
        std::lock_guard lock{mutex};
        const auto it = sessions.find(id);
        return it == sessions.end() ? nullptr : it->second;
    }
    void expire(Session& s) const {
        if (!terminal(s.view.status) && clock->steady_now() >= s.deadline) {
            s.view.status = "expired";
            s.view.error_code = "SESSION_EXPIRED";
            s.verifier = {}; s.code = {}; s.provider_url = {};
        }
    }
    bool permitted(const Session& s) const {
        auto auth = identities->authenticate(s.token_hash);
        return auth && *auth && (*auth)->principal.id == s.owner && has_scope((*auth)->token.scopes, scope::credentials_write);
    }
    void exchange(const std::shared_ptr<Session>& s, std::stop_token pool_stop) {
        std::stop_callback shutdown{pool_stop, [&] { s->stop.request_stop(); }};
        ports::ScopeExit finished{[&] { std::lock_guard lock{s->mutex}; s->running = false; }};
        try {
            ports::SecretBytes code, verifier;
            ports::OperationContext context;
            {
                std::lock_guard lock{s->mutex};
                expire(*s);
                if (s->view.status != "exchanging") return;
                context = {std::min(s->deadline, s->exchange_deadline), s->stop.get_token()};
                code = std::move(s->code); verifier = std::move(s->verifier);
            }
            if (context.stop.stop_requested() || context.expired(clock->steady_now())) {
                std::lock_guard lock{s->mutex};
                s->view.status = "canceled"; return;
            }
            const auto issued_at = clock->now();
            if (!permitted(*s)) {
                std::lock_guard lock{s->mutex};
                if (!terminal(s->view.status)) { s->view.status = "failed"; s->view.error_code = "OAUTH_IDENTITY_REVOKED"; }
                return;
            }
            auto grant = s->protocol->exchange(s->owner, code, verifier, context);
            std::lock_guard lock{s->mutex};
            expire(*s);
            if (s->view.status != "exchanging") return;
            if (!grant || context.stop.stop_requested() || context.expired(clock->steady_now()) || !permitted(*s)) {
                s->view.status = "failed"; s->view.error_code = "OAUTH_EXCHANGE_FAILED"; return;
            }
            auto stored = credentials->create_oauth(s->owner, s->label, *s->app, std::move(*grant), issued_at);
            if (!stored) { s->view.status = "failed"; s->view.error_code = "OAUTH_CREDENTIAL_SAVE_FAILED"; return; }
            s->view.credential_id = stored->id;
            s->view.status = "succeeded";
        } catch (...) {
            std::lock_guard lock{s->mutex};
            if (!terminal(s->view.status)) {
                s->view.status = "failed"; s->view.error_code = "OAUTH_EXCHANGE_FAILED";
            }
        }
    }
};

OAuthService::OAuthService(Applications apps, std::shared_ptr<const ports::ProviderDirectory> providers,
    std::shared_ptr<CredentialService> credentials, std::shared_ptr<ports::CredentialRepository> repository,
    std::shared_ptr<ports::IdentityRepository> identities, std::shared_ptr<ports::AccessTokenCodec> crypto,
    std::shared_ptr<const ports::Clock> clock, std::shared_ptr<ports::WorkExecutor> executor, OAuthLimits limits)
    : state_(std::make_shared<State>()), executor_(std::move(executor)) {
    state_->applications = std::move(apps); state_->providers = std::move(providers);
    state_->credentials = std::move(credentials); state_->repository = std::move(repository);
    state_->identities = std::move(identities); state_->crypto = std::move(crypto); state_->clock = std::move(clock);
    state_->limits = limits;
}
OAuthService::~OAuthService() { stop(); }

domain::Result<OAuthSessionCreated> OAuthService::create(const std::string& owner, std::string_view bearer,
    const std::string& source, const std::string& profile, const std::string& label) {
    auto& st = *state_;
    if (st.stopping) return busy();
    if (label.size() > 128 || !domain::is_valid_utf8(label)) return Error::validation("label", "invalid label");
    auto protocol = st.providers->require_oauth(source, profile);
    if (!protocol) return protocol.error();
    std::shared_ptr<const ports::OAuthApplication> app;
    for (const auto& [id, candidate] : st.applications)
        if (candidate->source == source && candidate->auth_profile == profile) { app = candidate; break; }
    if (!app) return Error{ErrorCode::capability_unsupported, "OAuth application is not configured", false, {}};
    if (auto valid = validate_oauth_redirect(app->id, app->redirect_uri); !valid) return valid.error();
    auto nonce = st.crypto->generate(), ticket = st.crypto->generate(), verifier = st.crypto->generate();
    if (!nonce || !ticket || !verifier) return Error{ErrorCode::internal, "OAuth random generation failed", false, {}};
    if (!opaque(nonce->reveal()) || !opaque(ticket->reveal()) || !opaque(verifier->reveal()))
        return Error{ErrorCode::internal, "invalid OAuth random encoding", false, {}};
    auto challenge = st.crypto->pkce_challenge(verifier->reveal());
    if (!challenge) return challenge.error();
    auto authorization = (*protocol)->authorize_url(nonce->reveal(), *challenge);
    if (!authorization) return Error{ErrorCode::upstream_failed, "OAuth authorization URL is unavailable", false, {}};
    auto url = transport::Url::parse(*authorization);
    if (!url || !url->is_https() || authorization->find('#') != std::string::npos)
        return Error{ErrorCode::upstream_failed, "invalid OAuth authorization URL", false, {}};
    auto s = std::make_shared<State::Session>();
    s->state_hash = st.crypto->hash(nonce->reveal());
    s->view = {"oas_" + s->state_hash.substr(0, 32), source, profile, "pending", st.clock->now() + st.limits.session_ttl, {}, {}};
    s->deadline = st.clock->steady_now() + st.limits.session_ttl;
    s->owner = owner; s->label = label; s->token_hash = st.crypto->hash(bearer);
    s->ticket_hash = st.crypto->hash(ticket->reveal()); s->verifier = std::move(*verifier);
    s->provider_url = ports::SecretBytes{std::move(*authorization)};
    s->app = app; s->protocol = *protocol;
    if (!st.permitted(*s)) return Error{ErrorCode::credentials_required, "service identity is no longer authorized", false, {}};
    const auto start_url = transport::Url::parse(app->redirect_uri)->origin() +
        "/api/v1/oauth/authorize?session_id=" + s->view.id + "&ticket=" + std::string{ticket->reveal()};
    {
        std::lock_guard lock{st.mutex};
        const auto owned = std::count_if(st.sessions.begin(), st.sessions.end(), [&](const auto& item) { return item.second->owner == owner; });
        if (st.stopping || st.sessions.size() >= st.limits.sessions || static_cast<std::size_t>(owned) >= st.limits.per_owner) return busy();
        if (!st.sessions.emplace(s->view.id, s).second) return busy();
    }
    return OAuthSessionCreated{s->view, start_url};
}

domain::Result<OAuthSessionView> OAuthService::get(const std::string& owner, const std::string& id) {
    auto s = state_->find(id);
    if (!s || s->owner != owner) return missing();
    std::lock_guard lock{s->mutex}; state_->expire(*s); return s->view;
}

domain::Result<OAuthSessionView> OAuthService::cancel(const std::string& owner, const std::string& id) {
    auto s = state_->find(id);
    if (!s || s->owner != owner) return missing();
    OAuthSessionView view;
    { std::lock_guard lock{s->mutex};
      state_->expire(*s);
      if (!terminal(s->view.status)) { s->view.status = "canceled"; s->verifier = {}; s->code = {}; s->provider_url = {}; }
      view = s->view;
    }
    s->stop.request_stop(); return view;
}

domain::Result<OAuthBrowserStart> OAuthService::authorize(const std::string& id, std::string_view ticket) {
    if (!opaque(ticket)) return missing();
    auto s = state_->find(id);
    if (!s) return missing();
    std::lock_guard lock{s->mutex};
    state_->expire(*s);
    if (s->ticket_hash != state_->crypto->hash(ticket)) return missing();
    if (state_->stopping || s->view.status != "pending" || s->started || !state_->permitted(*s)) return conflict();
    auto cookie = state_->crypto->generate();
    if (!cookie) return cookie.error();
    s->cookie_hash = state_->crypto->hash(cookie->reveal());
    s->started = true;
    const auto url = transport::Url::parse(s->app->redirect_uri);
    OAuthBrowserStart result{std::string{s->provider_url.reveal()}, "ms_oauth_" + s->view.id + "=" + std::string{cookie->reveal()} +
        "; Path=/api/v1/oauth/callback/" + s->app->id + "; HttpOnly; SameSite=Lax; Max-Age=" + std::to_string(state_->limits.session_ttl.count()) +
        (url->is_https() ? "; Secure" : "")};
    s->provider_url = {};
    return result;
}

domain::Result<void> OAuthService::callback(const std::string& application_id, std::string_view state,
    std::string_view cookies, ports::SecretBytes code, std::string_view error) {
    if (!opaque(state) || cookies.size() > 8192 || code.size() > 4096 || error.size() > 128 || (!error.empty() == !code.empty()))
        return Error::validation("callback", "invalid OAuth callback");
    const auto hash = state_->crypto->hash(state);
    auto s = state_->find("oas_" + hash.substr(0, 32));
    if (!s) return missing();
    std::lock_guard lock{s->mutex};
    state_->expire(*s);
    if (s->state_hash != hash || s->app->id != application_id || !s->started ||
        !cookie_matches(cookies, "ms_oauth_" + s->view.id, s->cookie_hash, *state_->crypto)) return missing();
    if (state_->stopping || s->view.status != "pending") return conflict();
    if (!error.empty()) {
        s->view.status = error == "access_denied" ? "denied" : "failed";
        if (s->view.status == "failed") s->view.error_code = "OAUTH_AUTHORIZATION_FAILED";
        s->verifier = {}; return domain::ok();
    }
    if (!state_->permitted(*s)) { s->view.status = "failed"; s->verifier = {}; return conflict(); }
    s->code = std::move(code); s->view.status = "exchanging"; s->running = true;
    s->exchange_deadline = state_->clock->steady_now() + state_->limits.operation_budget;
    auto accepted = executor_->submit([st = state_, s](std::stop_token stop) { st->exchange(s, stop); });
    if (!accepted) {
        s->view.status = "failed"; s->view.error_code = "SERVICE_BUSY";
        s->running = false; s->code = {}; s->verifier = {}; return busy();
    }
    return domain::ok();
}

void OAuthService::maintain() {
    auto st = state_;
    if (st->stopping) return;
    {
        std::lock_guard lock{st->mutex};
        for (auto it = st->sessions.begin(); it != st->sessions.end();) {
            auto session = it->second;
            std::lock_guard session_lock{session->mutex};
            st->expire(*session);
            if (!session->running && st->clock->steady_now() >= session->deadline + 60s) it = st->sessions.erase(it);
            else ++it;
        }
        if (st->applications.empty() || st->clock->steady_now() < st->next_scan || st->scanning.exchange(true)) return;
        st->next_scan = st->clock->steady_now() + 5s;
    }
    // 每次最多 32 个应用、每应用 2 个凭证；单个扫描任务串行刷新，无重复入队或同池等待。
    auto accepted = executor_->submit([st](std::stop_token stop) {
        ports::ScopeExit finished{[&] { st->scanning = false; }};
        for (const auto& [id, app] : st->applications) {
            if (stop.stop_requested() || st->stopping) break;
            auto available = st->providers->require_oauth(app->source, app->auth_profile);
            if (!available) continue;
            auto due = st->repository->due_refreshes(id, app->version, st->clock->now(), 2);
            if (!due) continue;
            for (const auto& c : *due) {
                if (stop.stop_requested() || st->stopping) break;
                (void)st->credentials->refresh_oauth(c, *app, {st->clock->steady_now() + st->limits.operation_budget, stop});
            }
        }
    });
    if (!accepted) st->scanning = false;
}

void OAuthService::stop() {
    if (state_->stopping.exchange(true)) return;
    std::vector<std::shared_ptr<State::Session>> sessions;
    { std::lock_guard lock{state_->mutex}; for (const auto& [id, s] : state_->sessions) sessions.push_back(s); }
    for (const auto& s : sessions) (void)cancel(s->owner, s->view.id);
}
} // namespace music_service::application::accounts
