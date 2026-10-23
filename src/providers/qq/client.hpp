#pragma once
#include "application/ports/providers/bundle.hpp"
#include <nlohmann/json.hpp>
#include <charconv>
#include <stdexcept>

namespace music_service::providers::qq {
namespace ports = application::ports;
using Json = nlohmann::json;
using domain::Error;
using domain::ErrorCode;
inline constexpr std::string_view cookie_profile = "web_cookie";
inline constexpr std::string_view user_agent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/134.0.0.0 Safari/537.36";

// 所有协议错误只返回类别，不带上游正文、Cookie 或带签名的 URL。
template<class F> auto checked(F&& fn) -> domain::Result<decltype(fn())> {
    try { return fn(); }
    catch (const Error& e) { return e; }
    catch (const Json::exception&) { return Error{ErrorCode::upstream_failed, "invalid QQ response fields", false, {}}; }
    catch (const std::invalid_argument&) { return Error{ErrorCode::upstream_failed, "invalid QQ response values", false, {}}; }
}
template<class T> T take(domain::Result<T> result) {
    if (!result) throw result.error();
    return std::move(*result);
}
bool mid(std::string_view value);
bool decimal(std::string_view value);
std::string cookie_value(std::string_view cookie, std::string_view name);
std::string digest(std::string_view value);
void check_ref(const domain::PlatformRef& ref, bool numeric = false);
std::uint32_t offset(std::optional<std::string> cursor, std::uint32_t limit);
std::string revision(std::optional<std::string> cursor);
void api_code(const Json& object);
Json parse_response(std::string_view body);

class Client {
public:
    explicit Client(const ports::ProviderSetup& setup) : setup_(setup) {}
    Json rpc(std::string_view op, std::string_view module, std::string_view method,
             Json param, const ports::ProviderContext& context, bool auth = true) const;
    Json get(std::string_view op, std::string path, const ports::ProviderContext& context,
             bool auth = false) const;
private:
    Json fetch(ports::HttpRequest request, const ports::ProviderContext& context, bool auth) const;
    ports::ProviderSetup setup_;
};
}
