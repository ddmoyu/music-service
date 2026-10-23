#pragma once

#include "application/transport/url.hpp"
#include "providers/bilibili/bundle.hpp"

#include <nlohmann/json.hpp>
#include <stdexcept>

namespace music_service::providers::bilibili {
namespace ports = application::ports;
using Json = nlohmann::json;
using domain::Error;
using domain::ErrorCode;
inline constexpr std::string_view cookie_profile = "web_cookie";
inline constexpr std::string_view user_agent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/134.0.0.0 Safari/537.36";
inline constexpr std::uint32_t max_parts = 1000;

template <class T> T take(domain::Result<T> result) {
    if (!result)
        throw result.error();
    return std::move(*result);
}
template <class F> auto checked(F&& fn) -> domain::Result<decltype(fn())> {
    try {
        return fn();
    } catch (const Error& e) {
        return e;
    } catch (const Json::exception&) {
        return Error{ErrorCode::upstream_failed, "invalid Bilibili response fields", false, {}};
    } catch (const std::invalid_argument&) {
        return Error{ErrorCode::upstream_failed, "invalid Bilibili response values", false, {}};
    }
}

bool valid_bvid(std::string_view value);
std::uint64_t decimal(std::string_view value, std::uint64_t maximum);
std::int64_t integer(const Json& value, std::int64_t minimum, std::int64_t maximum);
std::string text(const Json& object, const char* key, bool required = true);
std::string clean_title(std::string title);
std::optional<std::string> artwork(const Json& object);
domain::TrackRef make_ref(std::string id);
std::string cookie_value(std::string_view cookie, std::string_view key);

// 页码只是链接／搜索别名；实际资源身份必须固定到 bvid + cid。
struct VideoId {
    std::string bvid;
    std::uint32_t page{1};
    std::optional<std::uint64_t> cid;
};
VideoId parse_id(const domain::TrackRef& ref);
struct VideoPart {
    domain::Track track;
    std::string bvid;
    std::uint64_t cid;
    bool paid_preview{false};
};

class Client {
  public:
    explicit Client(const ports::ProviderSetup& setup) : setup_(setup) {}
    // nav 的 -101 仍包含匿名登录状态，只有凭证校验可以读取该响应。
    Json get(std::string_view operation, std::string path, const ports::ProviderContext& context,
             bool allow_logged_out = false) const;

  private:
    ports::ProviderSetup setup_;
};
VideoPart view(const Client&, const ports::ProviderContext&, const domain::TrackRef&,
               std::string_view operation);
void add_catalog(ports::ProviderBundle&);
void add_audio(ports::ProviderBundle&);
} // namespace music_service::providers::bilibili
