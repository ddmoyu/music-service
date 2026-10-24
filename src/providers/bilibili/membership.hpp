#pragma once
#include "providers/common/membership.hpp"
namespace music_service::providers::bilibili {
inline membership::Info parse_membership(const membership::Json& data, membership::Time now) {
    try {
        const auto type = membership::number(data.at("vipType"));
        const auto status = membership::number(data.at("vipStatus"));
        const auto expiry = data.contains("vipDueDate") && !data.at("vipDueDate").is_null()
            ? membership::milliseconds(membership::number(data.at("vipDueDate"))) : std::optional<membership::Time>{};
        if (type == 0 && status == 0) {
            if (expiry && *expiry > now) return membership::unavailable("UPSTREAM_RESPONSE_INVALID");
            return membership::none();
        }
        if (type != 1 && type != 2) return membership::unavailable("UPSTREAM_RESPONSE_INVALID");
        auto result = membership::by_flag("vip", type == 2 ? "年度大会员" : "月度大会员", data.at("vipStatus"), expiry, now);
        if (status == 0 && result.status == "none") result = {"expired", "vip", type == 2 ? "年度大会员" : "月度大会员", {}, {}, {}};
        return result;
    } catch (const std::exception&) { return membership::unavailable("UPSTREAM_RESPONSE_INVALID"); }
}
}
