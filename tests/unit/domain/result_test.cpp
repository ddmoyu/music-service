#include "domain/result.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

using namespace music_service::domain;

TEST_CASE("result holds a value or an error", "[domain][result]") {
    Result<int> value{42};
    REQUIRE(value.has_value());
    CHECK(*value == 42);

    Result<int> failure{Error{ErrorCode::not_found, "missing", false, {}}};
    REQUIRE_FALSE(failure.has_value());
    CHECK(failure.error().code == ErrorCode::not_found);
    CHECK(to_string(failure.error().code) == "NOT_FOUND");
}

TEST_CASE("result supports move-only values", "[domain][result]") {
    Result<std::unique_ptr<std::string>> result{std::make_unique<std::string>("x")};
    auto owned = std::move(result).value();
    CHECK(*owned == "x");
}

TEST_CASE("void results", "[domain][result]") {
    CHECK(ok().has_value());
    const Result<void> failure{Error::validation("field", "reason")};
    REQUIRE_FALSE(failure);
    CHECK(failure.error().code == ErrorCode::validation_failed);
    CHECK(to_string(failure.error().code) == "VALIDATION_FAILED");
}
