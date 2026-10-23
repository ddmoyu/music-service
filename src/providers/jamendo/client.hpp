#pragma once
#include "application/ports/providers/bundle.hpp"
#include <nlohmann/json.hpp>
#include <map>
#include <stdexcept>

namespace music_service::providers::jamendo {
namespace ports = application::ports;
using Json = nlohmann::json;
using domain::Error;
using domain::ErrorCode;
inline constexpr std::string_view user_agent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/134.0.0.0 Safari/537.36";
inline constexpr std::size_t max_collection = 10000;

// 错误不带上游正文、地址或会话字段；所有端口统一转换解析异常。
template<class F> auto checked(F&& fn) -> domain::Result<decltype(fn())> {
    try { return fn(); }
    catch (const Error& error) { return error; }
    catch (const Json::exception&) { return Error{ErrorCode::upstream_failed, "invalid Jamendo response fields", false, {}}; }
    catch (const std::invalid_argument&) { return Error{ErrorCode::upstream_failed, "invalid Jamendo response values", false, {}}; }
}
template<class T> T take(domain::Result<T> result) {
    if (!result) throw result.error();
    return std::move(*result);
}
bool decimal(std::string_view value);
void check_ref(const domain::PlatformRef& ref);
std::string digest(std::string_view value);
std::string call_header(std::string_view path, std::string_view nonce);
std::uint32_t offset(const ports::ListQuery& query, bool collection = false);
void same_revision(const ports::ListQuery& query, std::string_view revision);
void check_search(std::string_view text);

class Client {
public:
    explicit Client(const ports::ProviderSetup& setup) : setup_(setup) {}
    Json get(std::string_view operation, std::string_view path, std::string query,
             const ports::ProviderContext& context) const;
    Json one(std::string_view operation, std::string_view kind, const domain::PlatformRef& ref,
             const ports::ProviderContext& context) const;
    // 一次有界批量请求，严格核对返回 ID；缺失项由调用者按原位置计数。
    std::map<std::string, Json> batch(std::string_view operation, std::string_view kind,
        const std::vector<std::string>& ids, const ports::ProviderContext& context) const;
private:
    ports::ProviderSetup setup_;
};

template<class Port, class Impl> void bind(ports::ProviderBundle& bundle) {
    bundle.bindings.push_back(ports::CapabilityBinding::bind<Port>([](const ports::ProviderSetup& setup)
        -> domain::Result<std::shared_ptr<Port>> {
        if (!setup.upstream) return Error{ErrorCode::internal, "Jamendo requires the upstream client", false, {}};
        return std::shared_ptr<Port>{std::make_shared<Impl>(setup)};
    }));
}
}
