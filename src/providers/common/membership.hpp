#pragma once
#include "application/ports/providers/membership.hpp"
#include <nlohmann/json.hpp>
#include <charconv>
#include <chrono>
#include <initializer_list>
#include <stdexcept>
#include <string_view>

namespace music_service::providers::membership {
using Info = application::ports::MembershipInfo;
using Json = nlohmann::json;
using Time = std::chrono::system_clock::time_point;

inline Json parse(std::string_view body) {
    if (body.size() > 256 * 1024) throw std::invalid_argument{"membership response limit"};
    bool deep = false;
    auto result = Json::parse(body, [&](int depth, Json::parse_event_t, Json&) { deep |= depth > 16; return !deep; }, false);
    if (deep || !result.is_object()) throw std::invalid_argument{"membership response"};
    return result;
}
inline std::int64_t number(const Json& value, bool decimal_string = false) {
    if (value.is_number_unsigned()) {
        const auto n = value.get<std::uint64_t>();
        if (n > 7'258'118'400'000ULL) throw std::invalid_argument{"membership integer range"};
        return static_cast<std::int64_t>(n);
    }
    if (value.is_number_integer()) return value.get<std::int64_t>();
    if (decimal_string && value.is_string()) {
        const auto& s = value.get_ref<const std::string&>();
        std::int64_t n{};
        const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
        if (!s.empty() && ec == std::errc{} && end == s.data() + s.size()) return n;
    }
    throw std::invalid_argument{"membership integer"};
}
inline std::optional<Time> milliseconds(std::int64_t n) {
    if (n < 0 || n > 7'258'118'400'000LL) throw std::invalid_argument{"membership timestamp"};
    if (n == 0) return {};
    return Time{std::chrono::duration_cast<Time::duration>(std::chrono::milliseconds(n))};
}
// QQ 日期使用北京时间；不依赖宿主时区，不把仅日期伪造为精确截止时间。
inline std::optional<Time> china_datetime(const Json& value) {
    if (value.is_null()) return {};
    if (!value.is_string()) throw std::invalid_argument{"membership date"};
    const auto& s = value.get_ref<const std::string&>();
    if (s.empty() || s == "0000-00-00 00:00:00") return {};
    if (s.size() != 19 || s[4] != '-' || s[7] != '-' || s[10] != ' ' || s[13] != ':' || s[16] != ':') return {};
    auto part = [&](std::size_t at, std::size_t len) {
        unsigned n{};
        auto [end, ec] = std::from_chars(s.data() + at, s.data() + at + len, n);
        if (ec != std::errc{} || end != s.data() + at + len) throw std::invalid_argument{"membership date digits"};
        return n;
    };
    auto y = part(0, 4), m = part(5, 2), d = part(8, 2), h = part(11, 2), min = part(14, 2), sec = part(17, 2);
    std::chrono::year_month_day date{std::chrono::year{static_cast<int>(y)}, std::chrono::month{m}, std::chrono::day{d}};
    if (y < 1970 || y > 2199 || !date.ok() || h > 23 || min > 59 || sec > 59) throw std::invalid_argument{"membership date range"};
    return std::chrono::time_point_cast<Time::duration>(std::chrono::sys_days{date} + std::chrono::hours(h) +
        std::chrono::minutes(min) + std::chrono::seconds(sec) - std::chrono::hours(8));
}
inline Info none() { Info r; r.status = "none"; r.type = "none"; return r; }
inline Info by_expiry(std::string type, std::string name, std::optional<Time> expiry, Time now) {
    if (!expiry) return none();
    return {*expiry > now ? "active" : "expired", std::move(type), std::move(name), expiry, {}, {}};
}
inline Info by_flag(std::string type, std::string name, const Json& flag, std::optional<Time> expiry, Time now) {
    const auto active = number(flag);
    if (active != 0 && active != 1) throw std::invalid_argument{"membership flag"};
    if (active == 1) {
        if (expiry && *expiry <= now) throw std::invalid_argument{"membership evidence disagrees"};
        return {"active", std::move(type), std::move(name), expiry, {}, {}};
    }
    if (expiry && *expiry > now) throw std::invalid_argument{"membership evidence disagrees"};
    if (expiry && *expiry <= now) return {"expired", std::move(type), std::move(name), expiry, {}, {}};
    return none();
}
inline std::optional<std::chrono::sys_days> china_date(const Json& value) {
    if (value.is_null()) return {};
    if (!value.is_string()) throw std::invalid_argument{"membership date"};
    const auto& s = value.get_ref<const std::string&>();
    if (s.empty() || s == "0000-00-00") return {};
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return {};
    auto part = [&](std::size_t at, std::size_t len) {
        unsigned n{};
        const auto [end, ec] = std::from_chars(s.data() + at, s.data() + at + len, n);
        if (ec != std::errc{} || end != s.data() + at + len) throw std::invalid_argument{"membership date digits"};
        return n;
    };
    const auto y = part(0, 4);
    const std::chrono::year_month_day date{std::chrono::year{static_cast<int>(y)},
        std::chrono::month{part(5, 2)}, std::chrono::day{part(8, 2)}};
    if (y < 1970 || y > 2199 || !date.ok()) throw std::invalid_argument{"membership date range"};
    return std::chrono::sys_days{date};
}
inline Info by_china_flag(std::string type, std::string name, const Json& flag, const Json& expiry, Time now) {
    const auto date = china_date(expiry);
    if (!date) return by_flag(std::move(type), std::move(name), flag, china_datetime(expiry), now);
    const auto active = number(flag);
    if (active != 0 && active != 1) throw std::invalid_argument{"membership flag"};
    const auto today = std::chrono::floor<std::chrono::days>(now + std::chrono::hours(8));
    // 到期当天无法由日期判断具体时刻，以平台会员状态为准。
    if ((active == 1 && *date < today) || (active == 0 && *date > today))
        throw std::invalid_argument{"membership evidence disagrees"};
    Info result{active == 1 ? "active" : "expired", std::move(type), std::move(name), {}, {}, {}};
    result.expires_on = expiry.get<std::string>();
    return result;
}
inline Info highest(std::initializer_list<Info> products) {
    for (const auto& p : products) if (p.status == "active") return p;
    for (const auto& p : products) if (p.status == "expired") return p;
    return none();
}
inline Info unavailable(std::string code = "MEMBERSHIP_UNAVAILABLE") {
    Info r; r.error_code = std::move(code); return r;
}
} // namespace music_service::providers::membership
