#include "providers/kuwo/bundle.hpp"
#include "providers/kuwo/client.hpp"
#include "providers/kuwo/membership.hpp"
#include "application/transport/url.hpp"

namespace music_service::providers::kuwo {
namespace {
std::string cookie_value(std::string_view cookie, std::string_view name) {
    while (!cookie.empty()) {
        const auto end = cookie.find(';');
        auto pair = cookie.substr(0, end);
        cookie = end == cookie.npos ? std::string_view{} : cookie.substr(end + 1);
        while (!pair.empty() && pair.front() == ' ') pair.remove_prefix(1);
        while (!pair.empty() && pair.back() == ' ') pair.remove_suffix(1);
        if (pair.starts_with(std::string{name} + "=")) return std::string{pair.substr(name.size() + 1)};
    }
    return {};
}

class Verifier final : public ports::ICredentialVerifier {
public:
    explicit Verifier(const ports::ProviderSetup& setup) : client_(setup) {}
    domain::Result<ports::CredentialVerification> verify(const ports::CredentialSnapshot& snapshot,
        const ports::OperationContext& operation) override {
        ports::CredentialVerification result;
        const auto cookie = snapshot.secret.reveal();
        const auto uid = cookie_value(cookie, "userid"), sid = cookie_value(cookie, "websid");
        if (uid.empty() || uid.size() > 19 || uid.find_first_not_of("0123456789") != uid.npos ||
            uid.front() == '0' || sid.empty() || sid.size() > 256) {
            result.status = ports::VerificationStatus::invalid;
            return result;
        }
        ports::ProviderContext context{operation, snapshot.identity, {}, {}};
        auto request = [&](bool member) {
            ports::HttpRequest r;
            r.method = ports::HttpMethod::post;
            r.url = member ? "https://vip1.kuwo.cn/vip/v2/user/vip" : "https://vip1.kuwo.cn/vip/v2/sysinfo";
            r.operation = "verify_credentials";
            r.headers = {{"Content-Type", "application/x-www-form-urlencoded"}, {"Referer", "https://vip1.kuwo.cn/"},
                {"User-Agent", "Mozilla/5.0"}};
            r.secret_headers = {{"Cookie", std::string{cookie}}};
            r.body = std::string{"op="} + (member ? "ui" : "gui") + "&uid=" + application::transport::encode_query_component(uid) +
                "&sid=" + application::transport::encode_query_component(sid);
            r.allow_redirects = false;
            r.max_response_bytes = 256 * 1024;
            return r;
        };
        auto account = client_.body(request(false), context);
        if (!account) return account.error();
        try {
            const auto doc = membership::parse(*account);
            if (membership::number(doc.at("meta").at("code")) != 200)
                return domain::Error{domain::ErrorCode::upstream_failed, "Kuwo account query failed", false, {}};
            const auto& data = doc.at("data");
            if (data.is_null()) { result.status = ports::VerificationStatus::invalid; return result; }
            const auto actual = data.at("UID");
            const auto actual_uid = actual.is_string() ? actual.get<std::string>() : std::to_string(membership::number(actual));
            result.status = actual_uid == uid ? ports::VerificationStatus::valid : ports::VerificationStatus::invalid;
        } catch (const std::exception&) {
            return domain::Error{domain::ErrorCode::upstream_failed, "invalid Kuwo account response", false, {}};
        }
        if (result.status == ports::VerificationStatus::valid) {
            try {
                auto body = client_.body(request(true), context);
                result.membership = body ? parse_membership(*body, std::chrono::system_clock::now())
                    : membership::unavailable(std::string{domain::to_string(body.error().code)});
            } catch (const std::exception&) { result.membership = membership::unavailable("UPSTREAM_RESPONSE_INVALID"); }
        }
        return result;
    }
private:
    Client client_;
};
}

void add_auth(ports::ProviderBundle& bundle) {
    bundle.capabilities.push_back({.key = "verify_credentials", .credentials = ports::CredentialRequirement::required,
        .auth_profiles = {"web_cookie"}});
    bundle.auth_profiles = {ports::AuthProfileSpec{.auth_profile = "web_cookie", .kind = ports::CredentialKind::cookie,
        .version = 1, .input_fields = {{.name = "cookie", .required = true, .secret = true}},
        .verifier = [](const ports::ProviderSetup& setup) -> domain::Result<std::shared_ptr<ports::ICredentialVerifier>> {
            if (auto ready = require_upstream(setup); !ready) return ready.error();
            return std::shared_ptr<ports::ICredentialVerifier>{std::make_shared<Verifier>(setup)};
        }}};
}
} // namespace music_service::providers::kuwo
