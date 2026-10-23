#pragma once
#include "application/ports/providers/bundle.hpp"
#include <nlohmann/json.hpp>
#include <map>
#include <stdexcept>

namespace music_service::providers::qianqian {
namespace ports = application::ports;
using Json = nlohmann::json;
using domain::Error;
using domain::ErrorCode;
using Params = std::map<std::string, std::string>;
inline constexpr std::string_view user_agent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/143.0.0.0 Safari/537.36";
inline constexpr std::size_t max_body = 4 * 1024 * 1024;

// 上游正文和临时资源地址不进入对外错误或日志。
template<class F> auto checked(F&& fn) -> domain::Result<decltype(fn())> {
    try { return fn(); }
    catch (const Error& e) { return e; }
    catch (const Json::exception&) { return Error{ErrorCode::upstream_failed, "invalid Qianqian response fields", false, {}}; }
    catch (const std::invalid_argument&) { return Error{ErrorCode::upstream_failed, "invalid Qianqian response values", false, {}}; }
}
template<class T> T take(domain::Result<T> value) {
    if (!value) throw value.error();
    return std::move(*value);
}
bool identifier(std::string_view value, char prefix = '\0');
void check_ref(const domain::PlatformRef& ref, char prefix);
std::string signed_query(Params params, std::int64_t unix_seconds);
std::string digest(std::string_view value);
Json parse_response(std::string_view body);
std::string decode_body(std::string body, std::string encoding, std::size_t limit, const ports::OperationContext& context);

class Client {
public:
    explicit Client(const ports::ProviderSetup& setup) : setup_(setup) {}
    Json get(std::string_view operation, std::string_view endpoint, Params params,
             const ports::ProviderContext& context) const;
    std::string lyrics(std::string url, const ports::ProviderContext& context) const;
private:
    std::string fetch(ports::HttpRequest request, const ports::ProviderContext& context) const;
    ports::ProviderSetup setup_;
};

template<class Port, class Impl> void bind(ports::ProviderBundle& bundle) {
    bundle.bindings.push_back(ports::CapabilityBinding::bind<Port>([](const ports::ProviderSetup& setup) -> domain::Result<std::shared_ptr<Port>> {
        if (!setup.upstream) return Error{ErrorCode::internal, "Qianqian requires the upstream client", false, {}};
        return std::shared_ptr<Port>{std::make_shared<Impl>(setup)};
    }));
}
}
