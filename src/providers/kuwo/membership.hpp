#pragma once
#include "providers/common/membership.hpp"
namespace music_service::providers::kuwo {
inline membership::Info parse_membership(std::string_view body, membership::Time now) {
    try {
        const auto doc = membership::parse(body);
        if (membership::number(doc.at("meta").at("code")) != 200) return membership::unavailable();
        const auto& data = doc.at("data");
        // data.time 可能是缓存生成时间，使用本次应答 ctime。
        if (doc.contains("ctime")) now = membership::milliseconds(membership::number(doc.at("ctime"))).value_or(now);
        auto product = [&](const char* field, const char* type, const char* name) {
            return membership::by_expiry(type, name, membership::milliseconds(membership::number(data.at(field), true)), now);
        };
        return membership::highest({product("svipExpire", "svip", "超级VIP"),
            product("vipLuxuryExpire", "vip", "豪华VIP"), product("vipExpire", "vip", "VIP"),
            product("vipmExpire", "music_package", "音乐包")});
    } catch (const std::exception&) { return membership::unavailable("UPSTREAM_RESPONSE_INVALID"); }
}
}
