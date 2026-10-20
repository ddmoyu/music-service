#pragma once

#include "music_dl/domain/assets/quality.hpp"
#include "music_dl/domain/catalog/refs.hpp"
#include "music_dl/domain/identity/account.hpp"
#include "music_dl/domain/result.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace music_dl::domain {

enum class MetadataPolicy { best_effort, required, skip };

std::string_view to_string(MetadataPolicy value) noexcept;
Result<MetadataPolicy> parse_metadata_policy(std::string_view text);

// Where the audio comes from. Short-lived match/context handles never appear here;
// a manual choice is stored as its verified target.
struct SameSource {
    friend bool operator==(const SameSource&, const SameSource&) = default;
};
struct ManualTarget {
    TrackRef target;
    friend bool operator==(const ManualTarget&, const ManualTarget&) = default;
};
struct AutoFallback {
    std::vector<SourceId> candidate_sources; // order is meaningful
    std::uint32_t strategy_version{0};
    friend bool operator==(const AutoFallback&, const AutoFallback&) = default;
};
using SourceSelection = std::variant<SameSource, ManualTarget, AutoFallback>;

// Effective product settings resolved from configuration by the application layer.
// Versions are deterministic digests of the relevant configuration, not timestamps.
struct ProductPolicy {
    std::string destination_id;
    std::string destination_config_version;
    std::string naming_config_version;
    std::string metadata_config_version;

    friend bool operator==(const ProductPolicy&, const ProductPolicy&) = default;
};

// Input prepared by application/tasks after defaults, authorization and target checks.
struct DownloadIntentDraft {
    std::string owner_id;
    TrackRef original_track;
    SourceSelection selection{SameSource{}};
    std::vector<AccountBinding> account_bindings;
    ResourceRequirement requirement;
    MetadataPolicy metadata_policy{MetadataPolicy::best_effort};
    ProductPolicy product;
};

// The persisted, canonical description of what a download must produce.
// Equal intents may reuse each other's results; anything that changes the produced
// file is part of it. Request ids, task ids, times, tokens, URLs and
// duplicate_policy are deliberately absent.
class NormalizedDownloadIntent {
public:
    // Validates and normalizes:
    // - a manual target equal to the original track becomes SameSource;
    // - auto candidates drop duplicates (first occurrence wins) and the original source,
    //   and must leave at least one candidate;
    // - bindings must belong to the owner, are reduced to the sources the selection
    //   needs, and are ordered by source; every needed source must have one.
    static Result<NormalizedDownloadIntent> normalize(DownloadIntentDraft draft);

    [[nodiscard]] const std::string& owner_id() const noexcept { return owner_id_; }
    [[nodiscard]] const TrackRef& original_track() const noexcept { return original_track_; }
    [[nodiscard]] const SourceSelection& selection() const noexcept { return selection_; }
    [[nodiscard]] const std::vector<AccountBinding>& account_bindings() const noexcept {
        return account_bindings_;
    }
    [[nodiscard]] const ResourceRequirement& requirement() const noexcept { return requirement_; }
    [[nodiscard]] MetadataPolicy metadata_policy() const noexcept { return metadata_policy_; }
    [[nodiscard]] const ProductPolicy& product() const noexcept { return product_; }

private:
    NormalizedDownloadIntent(std::string owner_id, TrackRef original_track,
                             SourceSelection selection,
                             std::vector<AccountBinding> account_bindings,
                             ResourceRequirement requirement, MetadataPolicy metadata_policy,
                             ProductPolicy product);

    std::string owner_id_;
    TrackRef original_track_;
    SourceSelection selection_;
    std::vector<AccountBinding> account_bindings_;
    ResourceRequirement requirement_;
    MetadataPolicy metadata_policy_{MetadataPolicy::best_effort};
    ProductPolicy product_;
};

// canonical is the full encoded intent; digest is a short index key over it.
// Equality compares schema and canonical form, so a digest collision cannot merge
// two different intents.
struct DownloadFingerprint {
    static constexpr std::uint32_t current_schema = 1;

    std::uint32_t schema_version{current_schema};
    std::string canonical;
    std::string digest;

    friend bool operator==(const DownloadFingerprint& a, const DownloadFingerprint& b) {
        return a.schema_version == b.schema_version && a.canonical == b.canonical;
    }
};

DownloadFingerprint fingerprint(const NormalizedDownloadIntent& intent);

} // namespace music_dl::domain
