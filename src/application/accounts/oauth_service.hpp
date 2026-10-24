#pragma once

#include "application/accounts/credential_service.hpp"
#include "application/ports/identity/access_tokens.hpp"
#include "application/ports/runtime/executor.hpp"
#include "application/ports/storage/identity_repository.hpp"

#include <map>

namespace music_service::application::accounts {

struct OAuthSessionView {
    std::string id, source, auth_profile, status;
    std::chrono::system_clock::time_point expires_at;
    std::optional<std::string> credential_id, error_code;
};
struct OAuthSessionCreated { OAuthSessionView session; std::string authorization_url; };
struct OAuthBrowserStart { std::string authorization_url, cookie; };
struct OAuthLimits {
    std::size_t sessions{128}, per_owner{8};
    std::chrono::seconds session_ttl{600}, operation_budget{30};
};

// 回调地址固定到应用专属路径，HTTPS 或明确的本机回环 HTTP，拒绝查询、片段和用户信息。
domain::Result<void> validate_oauth_redirect(std::string_view id, std::string_view uri);

// 会话只存内存：重启必须重新登录；token 保存在 SecretStore，刷新意图保存在 SQLite。
// HTTP 线程只校验及入队，协议请求使用独立有界执行器。
class OAuthService {
public:
    using Applications = std::map<std::string, std::shared_ptr<const ports::OAuthApplication>>;
    OAuthService(Applications applications, std::shared_ptr<const ports::ProviderDirectory> providers,
        std::shared_ptr<CredentialService> credentials, std::shared_ptr<ports::CredentialRepository> repository,
        std::shared_ptr<ports::IdentityRepository> identities, std::shared_ptr<ports::AccessTokenCodec> crypto,
        std::shared_ptr<const ports::Clock> clock, std::shared_ptr<ports::WorkExecutor> executor,
        OAuthLimits limits = {});
    ~OAuthService();
    domain::Result<OAuthSessionCreated> create(const std::string& owner, std::string_view bearer,
        const std::string& source, const std::string& profile, const std::string& label);
    domain::Result<OAuthSessionView> get(const std::string& owner, const std::string& id);
    domain::Result<OAuthSessionView> cancel(const std::string& owner, const std::string& id);
    domain::Result<OAuthBrowserStart> authorize(const std::string& id, std::string_view ticket);
    domain::Result<void> callback(const std::string& application_id, std::string_view state,
        std::string_view cookies, ports::SecretBytes code, std::string_view error = {});
    void maintain(); // 有界清理与后台扫描；不在调用线程执行上游请求
    void stop();

private:
    struct State;
    std::shared_ptr<State> state_;
    std::shared_ptr<ports::WorkExecutor> executor_;
};

} // namespace music_service::application::accounts
