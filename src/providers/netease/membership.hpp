#pragma once
#include "providers/common/membership.hpp"
namespace music_service::providers::netease {
inline membership::Info parse_membership(std::string_view body, membership::Time now) {
    try {
        const auto doc = membership::parse(body);
        if (membership::number(doc.at("code")) != 200) return membership::unavailable();
        const auto& data = doc.at("data");
        auto product = [&](const char* field, const char* type, const char* name) {
            return membership::by_expiry(type, name,
                membership::milliseconds(membership::number(data.at(field).at("expireTime"))), now);
        };
        return membership::highest({product("redplus", "svip", "黑胶SVIP"),
            product("associator", "vip", "黑胶VIP"), product("musicPackage", "music_package", "音乐包")});
    } catch (const std::exception&) { return membership::unavailable("UPSTREAM_RESPONSE_INVALID"); }
}
}
