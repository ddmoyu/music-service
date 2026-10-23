#pragma once
#include "application/ports/providers/bundle.hpp"
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace music_service::providers::migu {
namespace ports = application::ports;
using Json = nlohmann::json;
using domain::Error;
using domain::ErrorCode;
inline constexpr std::string_view api = "https://app.c.nf.migu.cn";
inline constexpr std::string_view user_agent = "Android_migu/7.41.13 okhttp/3.12.13";

template <class F> auto checked(F&& fn) -> domain::Result<decltype(fn())> {
    try {
        return fn();
    } catch (const Error& e) {
        return e;
    } catch (const Json::exception&) {
        return Error{ErrorCode::upstream_failed, "invalid Migu response fields", false, {}};
    } catch (const std::invalid_argument&) {
        return Error{ErrorCode::upstream_failed, "invalid Migu response values", false, {}};
    }
}
template <class T> T take(domain::Result<T> result) {
    if (!result)
        throw result.error();
    return std::move(*result);
}
bool decimal(std::string_view value);
void check_ref(const domain::PlatformRef& ref, bool album = false);
std::string digest(std::string_view value);
Json parse_json(std::string_view body);
void array_shape(const Json& value, std::size_t max);

struct Cursor {
    std::uint32_t offset{};
    std::string revision;
};
Cursor cursor(const std::optional<std::string>& value, std::uint32_t limit);
void same_revision(const Cursor& value, std::string_view revision);

class Client {
  public:
    explicit Client(const ports::ProviderSetup& setup) : setup_(setup) {}
    Json get(std::string_view operation, std::string url, const ports::ProviderContext&,
             bool auth = false) const;
    std::string lyrics(std::string url, const ports::ProviderContext&) const;
    Json resource(std::string_view operation, const domain::PlatformRef&, std::string_view type,
                  const ports::ProviderContext&) const;

  private:
    std::string fetch(std::string_view operation, std::string url, const ports::ProviderContext&,
                      bool auth) const;
    ports::ProviderSetup setup_;
};

template <class Port, class Impl> void bind(ports::ProviderBundle& bundle) {
    bundle.bindings.push_back(ports::CapabilityBinding::bind<Port>(
        [](const ports::ProviderSetup& s) -> domain::Result<std::shared_ptr<Port>> {
            if (!s.upstream)
                return Error{ErrorCode::internal, "Migu requires the upstream client", false, {}};
            return std::shared_ptr<Port>{std::make_shared<Impl>(s)};
        }));
}
} // namespace music_service::providers::migu
