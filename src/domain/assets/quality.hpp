#pragma once

#include "domain/result.hpp"

#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace music_dl::domain {

// Ordered from lowest to highest.
enum class Quality { standard, high, lossless, hires };
enum class QualityPolicy { strict, allow_lower };
enum class Completeness { full, preview, unknown };

std::string_view to_string(Quality value) noexcept;
std::string_view to_string(QualityPolicy value) noexcept;
std::string_view to_string(Completeness value) noexcept;
Result<Quality> parse_quality(std::string_view text);
Result<QualityPolicy> parse_quality_policy(std::string_view text);

// What the caller asked for. Defaults follow the API: standard, strict, no preview.
struct ResourceRequirement {
    Quality requested{Quality::standard};
    QualityPolicy policy{QualityPolicy::strict};
    bool allow_preview{false};
};

// Chooses the quality tier to request from the tiers a platform offers for a track.
// The requested tier wins when offered; allow_lower falls back to the best lower tier.
// A higher tier is never chosen silently.
Result<Quality> choose_quality(const ResourceRequirement& requirement,
                               std::span<const Quality> offered);

enum class ResourceWarning {
    full_length_unverified, // completeness unknown
    quality_unverified,     // actual quality unknown; an extension alone is no proof
    quality_lowered,        // allow_lower accepted a lower tier
    preview_resource,       // allow_preview accepted a known preview
};

std::string_view to_string(ResourceWarning value) noexcept;

// Evidence about a concrete resource gathered during resolve/probe.
struct ResourceFacts {
    std::optional<Quality> actual_quality;
    Completeness completeness{Completeness::unknown};
};

struct ResourceVerdict {
    std::vector<ResourceWarning> warnings;
};

// Accepts or rejects a concrete resource against the requirement.
// Known previews are rejected unless allowed; known lower quality is rejected under strict.
// Unknown completeness or quality is accepted with a warning, never reported as verified.
Result<ResourceVerdict> evaluate_resource(const ResourceRequirement& requirement,
                                          const ResourceFacts& facts);

} // namespace music_dl::domain
