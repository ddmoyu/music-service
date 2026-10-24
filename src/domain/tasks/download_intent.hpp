#pragma once

#include "domain/assets/quality.hpp"
#include "domain/catalog/refs.hpp"
#include "domain/identity/account.hpp"
#include "domain/result.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace music_service::domain {

enum class MetadataPolicy { best_effort, required, skip };

std::string_view to_string(MetadataPolicy value) noexcept;
Result<MetadataPolicy> parse_metadata_policy(std::string_view text);

// Persist the same-source marker to preserve existing task fingerprints and recovery.
struct SameSource {
    friend bool operator==(const SameSource&, const SameSource&) = default;
};
using SourceSelection = std::variant<SameSource>;

// Effective product settings resolved from configuration by the application layer.
// Versions are deterministic digests of the relevant configuration, not timestamps.
struct OutputOptions {
    std::string name_format{"artist_title"}; // title | artist_title | title_artist
    std::string folder_layout{"flat"};      // flat | artist | artist_album
    bool embed_cover{true};
    bool embed_lyrics{true};
    std::uint64_t cover_max_bytes{8 * 1024 * 1024};
    std::uint32_t metadata_timeout_seconds{10};
    std::string lyrics_variant{"lrc"}; // lrc | verbatim | romanized; fallback to available original

    bool export_lyrics{false};
    bool export_cover{false};
    bool sidecars_required{false};

    friend bool operator==(const OutputOptions&, const OutputOptions&) = default;
};

Result<void> validate_output_options(const OutputOptions& options);

struct ProductPolicy {
    std::string destination_id;
    std::string destination_config_version;
    std::string naming_config_version;
    std::string metadata_config_version;
    // 旧意图不含此字段：恢复时仍只写基础标签并沿用原命名。
    std::optional<OutputOptions> output{};

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
    // Bindings must belong to the owner; only the original source is required.
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

} // namespace music_service::domain
