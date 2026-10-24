#pragma once
#include "application/ports/providers/bundle.hpp"
#include <nlohmann/json.hpp>
#include <map>
#include <stdexcept>

namespace music_service::providers::kugou {
namespace ports = application::ports;
using Json = nlohmann::json;
using domain::Error;
using domain::ErrorCode;
inline constexpr std::string_view user_agent =
    "Mozilla/5.0 (iPhone; CPU iPhone OS 13_2_3 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like "
    "Gecko) Version/13.0.3 Mobile/15E148 Safari/604.1";
inline constexpr std::string_view mobile_api = "http://mobilecdn.kugou.com/api/v3/";

// 错误不包含上游正文、歌词访问密钥或临时音源地址。
template <class F> auto checked(F&& fn) -> domain::Result<decltype(fn())> {
    try {
        return fn();
    } catch (const Error& e) {
        return e;
    } catch (const Json::exception&) {
        return Error{ErrorCode::upstream_failed, "invalid Kugou response fields", false, {}};
    } catch (const std::invalid_argument&) {
        return Error{ErrorCode::upstream_failed, "invalid Kugou response values", false, {}};
    }
}
template <class T> T take(domain::Result<T> result) {
    if (!result)
        throw result.error();
    return std::move(*result);
}
bool decimal(std::string_view value, bool zero = false);
bool hash(std::string_view value);
std::string lower(std::string value);
std::string digest(std::string_view value);
void check_ref(const domain::PlatformRef& ref, bool track = false);
std::uint32_t offset(const std::optional<std::string>& cursor, std::uint32_t limit,
                     bool collection);
void same_revision(const std::optional<std::string>& cursor, std::string_view revision);
std::vector<ports::PlaintextEndpoint> plaintext_endpoints();
Json parse_response(std::string_view body);
void api_success(const Json& doc);
std::string signed_web_url(std::string_view endpoint,
                           std::map<std::string, std::string> params);

class Client {
  public:
    explicit Client(const ports::ProviderSetup& setup) : setup_(setup) {}
    Json get(std::string_view operation, std::string url,
             const ports::ProviderContext& context, bool cookie = false) const;
    Json api(std::string_view operation, std::string path,
             const ports::ProviderContext& context) const;

  private:
    ports::ProviderSetup setup_;
};
template <class Port, class Impl> void bind(ports::ProviderBundle& bundle) {
    bundle.bindings.push_back(ports::CapabilityBinding::bind<Port>(
        [](const ports::ProviderSetup& setup) -> domain::Result<std::shared_ptr<Port>> {
            if (!setup.upstream)
                return Error{ErrorCode::internal, "Kugou requires the upstream client", false, {}};
            return std::shared_ptr<Port>{std::make_shared<Impl>(setup)};
        }));
}
} // namespace music_service::providers::kugou
