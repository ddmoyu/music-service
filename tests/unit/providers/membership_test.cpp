#include "providers/netease/membership.hpp"
#include "providers/qq/membership.hpp"
#include "providers/kuwo/membership.hpp"
#include "providers/bilibili/membership.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace music_service::providers;
using Json = nlohmann::json;
namespace {
const auto now = *membership::milliseconds(1'800'000'000'000);
constexpr std::int64_t future = 2'000'000'000'000;
constexpr std::int64_t past = 1'700'000'000'000;
}

TEST_CASE("membership maps platform tiers and exact expiry without guessing", "[membership][providers]") {
    SECTION("netease priority and expired super membership with active ordinary membership") {
        Json d{{"code", 200}, {"data", {{"redplus", {{"expireTime", future}}},
            {"associator", {{"expireTime", future + 1000}}}, {"musicPackage", {{"expireTime", 0}}}}}};
        auto result = netease::parse_membership(d.dump(), now);
        CHECK(result.type == "svip"); CHECK(result.name == "黑胶SVIP");
        CHECK(result.expires_at == membership::milliseconds(future));
        d["data"]["redplus"]["expireTime"] = past;
        CHECK(netease::parse_membership(d.dump(), now).type == "vip");
        d["data"]["associator"]["expireTime"] = 0;
        CHECK(netease::parse_membership(d.dump(), now).status == "expired");
        d["data"].erase("redplus");
        CHECK(netease::parse_membership(d.dump(), now).status == "unknown");
    }
    SECTION("QQ svip field means luxury green diamond; HugeVip means super") {
        Json d{{"identity", {{"HugeVip", 0}, {"HugeVipEnd", ""}, {"vip", 1}, {"overdate", "2100-01-01 00:00:00"}}},
            {"svip", 1}, {"send", "2100-01-01 00:00:00"}};
        auto r = qq::parse_membership(d, now);
        CHECK(r.type == "vip"); CHECK(r.name == "豪华绿钻");
        CHECK(r.expires_at == membership::milliseconds(4'102'416'000'000));
        CHECK_FALSE(r.expires_on);
        d["identity"]["HugeVip"] = 1;
        d["identity"]["HugeVipEnd"] = "2100-02-01 12:30:45";
        r = qq::parse_membership(d, now);
        CHECK(r.type == "svip"); CHECK(r.name == "超级会员");
        d["identity"]["HugeVipEnd"] = "";
        CHECK_FALSE(qq::parse_membership(d, now).expires_at);
        d["identity"]["HugeVip"] = 7;
        CHECK(qq::parse_membership(d, now).status == "unknown");
    }
    SECTION("kuwo decimal strings, server clock and distinct expiry") {
        Json d{{"meta", {{"code", 200}}}, {"ctime", 1'800'000'000'000LL}, {"data", {
            {"svipExpire", "0"}, {"vipLuxuryExpire", std::to_string(future)},
            {"vipExpire", std::to_string(future + 1000)}, {"vipmExpire", "0"}, {"time", "1000"}}}};
        auto r = kuwo::parse_membership(d.dump(), now);
        CHECK(r.type == "vip"); CHECK(r.name == "豪华VIP");
        CHECK(r.expires_at == membership::milliseconds(future));
        d["data"]["svipExpire"] = std::to_string(future + 5000);
        CHECK(kuwo::parse_membership(d.dump(), now).type == "svip");
        d["data"]["svipExpire"] = "not-a-number";
        CHECK(kuwo::parse_membership(d.dump(), now).status == "unknown");
    }
    SECTION("bilibili yearly membership is not a fabricated SVIP") {
        Json d{{"vipType", 2}, {"vipStatus", 1}, {"vipDueDate", future}};
        auto r = bilibili::parse_membership(d, now);
        CHECK(r.type == "vip"); CHECK(r.name == "年度大会员");
        d["vipStatus"] = 0; d["vipDueDate"] = past;
        CHECK(bilibili::parse_membership(d, now).status == "expired");
        d["vipType"] = 0; d["vipDueDate"] = 0;
        CHECK(bilibili::parse_membership(d, now).status == "none");
    }
}

TEST_CASE("QQ date expiry keeps calendar precision and uses the Beijing day boundary", "[membership][providers][qq]") {
    const auto checked = *membership::china_datetime("2026-10-24 20:18:00");
    // 实测字段形态：豪华绿钻只有日期，普通绿钻另有时分秒，不能互相替代。
    Json data{{"identity", {{"HugeVip", 0}, {"HugeVipEnd", ""}, {"vip", 1},
        {"overdate", "2026-10-27 20:15:16"}}}, {"svip", 1}, {"send", "2026-10-27"}};
    auto result = qq::parse_membership(data, checked);
    CHECK(result.status == "active");
    CHECK(result.name == "豪华绿钻");
    CHECK(result.expires_on == "2026-10-27");
    CHECK_FALSE(result.expires_at);

    data["identity"]["vip"] = 0;
    data["identity"]["overdate"] = "";
    const auto expiry_day = *membership::china_datetime("2026-10-27 23:59:59");
    CHECK(qq::parse_membership(data, expiry_day).status == "active");
    data["svip"] = 0;
    result = qq::parse_membership(data, expiry_day);
    CHECK(result.status == "expired");
    CHECK(result.expires_on == "2026-10-27");
    CHECK_FALSE(result.expires_at);
    CHECK(qq::parse_membership(data, checked).status == "unknown"); // 未来日期与非会员标记矛盾。

    data["svip"] = 1;
    data["send"] = "2026-10-26";
    const auto beijing_midnight = std::chrono::sys_days{std::chrono::year{2026}/10/26} + std::chrono::hours(16);
    CHECK(qq::parse_membership(data, beijing_midnight - std::chrono::milliseconds(1)).status == "active");
    CHECK(qq::parse_membership(data, beijing_midnight).status == "unknown");
    for (const auto* malformed : {"2026-02-30", "2026-00-15", "2026-10-00", "2026-13-01",
            "2026-01-32", "1969-12-31", "2200-01-01", "2026-1x-01", "2100-02-29"}) {
        data["send"] = malformed;
        CHECK(qq::parse_membership(data, checked).status == "unknown");
    }
    CHECK(membership::china_date("2028-02-29").has_value());
    data["send"] = "";
    result = qq::parse_membership(data, checked);
    CHECK(result.status == "active");
    CHECK_FALSE(result.expires_at);
    CHECK_FALSE(result.expires_on);
}

TEST_CASE("missing malformed or contradictory membership never becomes nonmember", "[membership][providers]") {
    for (auto body : {"{}", "null", "[]", "invalid", "{\"code\":403}"}) {
        CHECK(netease::parse_membership(body, now).status == "unknown");
        CHECK(kuwo::parse_membership(body, now).status == "unknown");
    }
    CHECK(bilibili::parse_membership(Json::object(), now).status == "unknown");
    CHECK(qq::parse_membership(Json::object(), now).status == "unknown");
    CHECK(bilibili::parse_membership({{"vipType", 1}, {"vipStatus", 1}, {"vipDueDate", past}}, now).status == "unknown");
    CHECK(bilibili::parse_membership({{"vipType", 1}, {"vipStatus", 0}, {"vipDueDate", future}}, now).status == "unknown");
    CHECK(bilibili::parse_membership({{"vipType", 0}, {"vipStatus", 0}, {"vipDueDate", future}}, now).status == "unknown");
    CHECK(bilibili::parse_membership({{"vipType", 1}, {"vipStatus", true}}, now).status == "unknown");
    CHECK(bilibili::parse_membership({{"vipType", 1}, {"vipStatus", 1}, {"vipDueDate", UINT64_MAX}}, now).status == "unknown");
    CHECK_THROWS(membership::china_datetime("2100-02-31 00:00:00"));
    CHECK_FALSE(membership::china_datetime("2100-01-01"));
    CHECK_THROWS(membership::parse(std::string(256 * 1024 + 1, ' ')));
    CHECK_THROWS(membership::parse(std::string(40, '[') + "0" + std::string(40, ']')));
}
