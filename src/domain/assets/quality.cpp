#include "domain/assets/quality.hpp"

#include <algorithm>

namespace music_dl::domain {

std::string_view to_string(Quality value) noexcept {
    switch (value) {
    case Quality::standard:
        return "standard";
    case Quality::high:
        return "high";
    case Quality::lossless:
        return "lossless";
    case Quality::hires:
        return "hires";
    }
    return "standard";
}

std::string_view to_string(QualityPolicy value) noexcept {
    return value == QualityPolicy::strict ? "strict" : "allow_lower";
}

std::string_view to_string(Completeness value) noexcept {
    switch (value) {
    case Completeness::full:
        return "full";
    case Completeness::preview:
        return "preview";
    case Completeness::unknown:
        return "unknown";
    }
    return "unknown";
}

std::string_view to_string(ResourceWarning value) noexcept {
    switch (value) {
    case ResourceWarning::full_length_unverified:
        return "FULL_LENGTH_UNVERIFIED";
    case ResourceWarning::quality_unverified:
        return "QUALITY_UNVERIFIED";
    case ResourceWarning::quality_lowered:
        return "QUALITY_LOWERED";
    case ResourceWarning::preview_resource:
        return "PREVIEW_RESOURCE";
    }
    return "FULL_LENGTH_UNVERIFIED";
}

Result<Quality> parse_quality(std::string_view text) {
    for (const auto value : {Quality::standard, Quality::high, Quality::lossless, Quality::hires}) {
        if (to_string(value) == text) {
            return value;
        }
    }
    return Error::validation("quality", "must be standard, high, lossless or hires");
}

Result<QualityPolicy> parse_quality_policy(std::string_view text) {
    if (text == "strict") {
        return QualityPolicy::strict;
    }
    if (text == "allow_lower") {
        return QualityPolicy::allow_lower;
    }
    return Error::validation("quality_policy", "must be strict or allow_lower");
}

Result<Quality> choose_quality(const ResourceRequirement& requirement,
                               std::span<const Quality> offered) {
    if (std::find(offered.begin(), offered.end(), requirement.requested) != offered.end()) {
        return requirement.requested;
    }
    if (requirement.policy == QualityPolicy::allow_lower) {
        std::optional<Quality> best;
        for (const auto quality : offered) {
            if (quality < requirement.requested && (!best || quality > *best)) {
                best = quality;
            }
        }
        if (best) {
            return *best;
        }
    }
    return Error{ErrorCode::quality_unavailable, "requested quality is not available", false,
                 {}};
}

Result<ResourceVerdict> evaluate_resource(const ResourceRequirement& requirement,
                                          const ResourceFacts& facts) {
    ResourceVerdict verdict;

    switch (facts.completeness) {
    case Completeness::preview:
        if (!requirement.allow_preview) {
            return Error{ErrorCode::preview_rejected, "resource is a preview", false, {}};
        }
        verdict.warnings.push_back(ResourceWarning::preview_resource);
        break;
    case Completeness::unknown:
        verdict.warnings.push_back(ResourceWarning::full_length_unverified);
        break;
    case Completeness::full:
        break;
    }

    if (!facts.actual_quality) {
        verdict.warnings.push_back(ResourceWarning::quality_unverified);
    } else if (*facts.actual_quality < requirement.requested) {
        if (requirement.policy == QualityPolicy::strict) {
            return Error{ErrorCode::quality_unavailable, "resource quality is lower than requested",
                         false, {}};
        }
        verdict.warnings.push_back(ResourceWarning::quality_lowered);
    }

    return verdict;
}

} // namespace music_dl::domain
