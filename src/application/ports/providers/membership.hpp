#pragma once

#include <chrono>
#include <optional>
#include <string>

namespace music_service::application::ports {

// 会员是平台返回的权益证据，不是 OAuth scope，也不保证具体歌曲的播放或下载权限。
// 缺失类型、有效期或查询失败均保留未知；不得由 Cookie 存在或登录成功推断会员。
struct MembershipInfo {
    std::string status{"unknown"}; // unknown | none | active | expired
    std::string type{"unknown"};   // unknown | none | vip | svip | music_package
    std::optional<std::string> name;
    std::optional<std::chrono::system_clock::time_point> expires_at;
    std::optional<std::chrono::system_clock::time_point> checked_at;
    std::optional<std::string> error_code;
    // 平台只提供日期时保留原始日历日期；与 expires_at 互斥，不补造时分秒。
    std::optional<std::string> expires_on;
};

} // namespace music_service::application::ports
