#pragma once
#include "providers/common/membership.hpp"
namespace music_service::providers::qq {
inline membership::Info parse_membership(const membership::Json& data, membership::Time now) {
    try {
        const auto& identity = data.at("identity");
        // 官网旧字段 svip 指豪华绿钻，超级会员使用 HugeVip。
        return membership::highest({
            membership::by_china_flag("svip", "超级会员", identity.at("HugeVip"), identity.at("HugeVipEnd"), now),
            membership::by_china_flag("vip", "豪华绿钻", data.at("svip"), data.at("send"), now),
            membership::by_china_flag("vip", "绿钻", identity.at("vip"), identity.at("overdate"), now)});
    } catch (const std::exception&) { return membership::unavailable("UPSTREAM_RESPONSE_INVALID"); }
}
}
