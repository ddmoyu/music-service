#include "test_support.hpp"

#include "music_dl/domain/assets/quality.hpp"

#include <algorithm>
#include <array>

using namespace music_dl::domain;

namespace {

bool has_warning(const ResourceVerdict& verdict, ResourceWarning warning) {
    return std::find(verdict.warnings.begin(), verdict.warnings.end(), warning) !=
           verdict.warnings.end();
}

} // namespace

TEST_CASE("defaults are standard, strict and no preview", "[domain][quality]") {
    const ResourceRequirement requirement;
    CHECK(requirement.requested == Quality::standard);
    CHECK(requirement.policy == QualityPolicy::strict);
    CHECK_FALSE(requirement.allow_preview);
}

TEST_CASE("quality names parse and round-trip", "[domain][quality]") {
    for (const auto quality : {Quality::standard, Quality::high, Quality::lossless, Quality::hires}) {
        CHECK(music_dl::test::unwrap(parse_quality(to_string(quality))) == quality);
    }
    CHECK_FALSE(parse_quality("HIGH")); // enums are case sensitive
    CHECK_FALSE(parse_quality_policy("lower"));
}

TEST_CASE("choose_quality never downgrades under strict", "[domain][quality]") {
    const std::array offered{Quality::standard, Quality::high};

    CHECK(*choose_quality({Quality::high, QualityPolicy::strict, false}, offered) == Quality::high);

    const auto strict = choose_quality({Quality::lossless, QualityPolicy::strict, false}, offered);
    REQUIRE_FALSE(strict);
    CHECK(strict.error().code == ErrorCode::quality_unavailable);

    CHECK(*choose_quality({Quality::lossless, QualityPolicy::allow_lower, false}, offered) ==
          Quality::high);
}

TEST_CASE("choose_quality never upgrades silently", "[domain][quality]") {
    const std::array offered{Quality::lossless};
    CHECK_FALSE(choose_quality({Quality::standard, QualityPolicy::strict, false}, offered));
    CHECK_FALSE(choose_quality({Quality::standard, QualityPolicy::allow_lower, false}, offered));
}

TEST_CASE("known previews are rejected unless allowed", "[domain][quality]") {
    const ResourceFacts preview{Quality::standard, Completeness::preview};

    const auto rejected = evaluate_resource({}, preview);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == ErrorCode::preview_rejected);

    const auto allowed = evaluate_resource({Quality::standard, QualityPolicy::strict, true}, preview);
    REQUIRE(allowed);
    CHECK(has_warning(*allowed, ResourceWarning::preview_resource));
}

TEST_CASE("unknown completeness is accepted but never reported as full", "[domain][quality]") {
    const auto verdict = evaluate_resource({}, {Quality::standard, Completeness::unknown});
    REQUIRE(verdict);
    CHECK(has_warning(*verdict, ResourceWarning::full_length_unverified));

    const auto full = evaluate_resource({}, {Quality::standard, Completeness::full});
    REQUIRE(full);
    CHECK(full->warnings.empty());
}

TEST_CASE("resource quality is checked against the policy", "[domain][quality]") {
    const ResourceRequirement strict_high{Quality::high, QualityPolicy::strict, false};
    const ResourceRequirement lower_high{Quality::high, QualityPolicy::allow_lower, false};
    const ResourceFacts standard_full{Quality::standard, Completeness::full};

    const auto strict = evaluate_resource(strict_high, standard_full);
    REQUIRE_FALSE(strict);
    CHECK(strict.error().code == ErrorCode::quality_unavailable);

    const auto lowered = evaluate_resource(lower_high, standard_full);
    REQUIRE(lowered);
    CHECK(has_warning(*lowered, ResourceWarning::quality_lowered));

    const auto unknown = evaluate_resource(strict_high, {std::nullopt, Completeness::full});
    REQUIRE(unknown);
    CHECK(has_warning(*unknown, ResourceWarning::quality_unverified));
}
