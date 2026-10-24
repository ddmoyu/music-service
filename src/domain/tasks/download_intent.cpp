#include "domain/tasks/download_intent.hpp"

#include "domain/detail/canonical.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>
#include <variant>

namespace music_service::domain {

namespace {

Result<std::vector<AccountBinding>> normalize_bindings(const std::string& owner_id,
                                                       const std::vector<SourceId>& needed,
                                                       std::vector<AccountBinding> bindings) {
    std::vector<AccountBinding> result;
    for (const auto& source : needed) {
        const AccountBinding* chosen = nullptr;
        for (const auto& binding : bindings) {
            if (binding.source() != source) {
                continue;
            }
            if (binding.principal_id() != owner_id) {
                return Error::validation("account_bindings", "binding belongs to another principal");
            }
            if (chosen != nullptr && !(*chosen == binding)) {
                return Error::validation("account_bindings",
                                         "conflicting bindings for source " + source.value());
            }
            chosen = &binding;
        }
        if (chosen == nullptr) {
            return Error::validation("account_bindings", "missing binding for source " +
                                                             source.value());
        }
        result.push_back(*chosen);
    }
    return result;
}

std::uint64_t fnv1a64(std::string_view data) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char c : data) {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string to_hex(std::uint64_t value) {
    constexpr std::array digits{'0', '1', '2', '3', '4', '5', '6', '7',
                                '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string out(16, '0');
    for (std::size_t i = 0; i < 16; ++i) {
        out[15 - i] = digits[static_cast<std::size_t>(value & 0xFU)];
        value >>= 4U;
    }
    return out;
}

void write_ref(detail::CanonicalWriter& writer, std::string_view tag, const TrackRef& ref) {
    writer.field(std::string{tag} + ".src", std::string_view{ref.source.value()})
        .field(std::string{tag} + ".id", std::string_view{ref.id.value()});
}

} // namespace

std::string_view to_string(MetadataPolicy value) noexcept {
    switch (value) {
    case MetadataPolicy::best_effort:
        return "best_effort";
    case MetadataPolicy::required:
        return "required";
    case MetadataPolicy::skip:
        return "skip";
    }
    return "best_effort";
}

Result<MetadataPolicy> parse_metadata_policy(std::string_view text) {
    for (const auto value :
         {MetadataPolicy::best_effort, MetadataPolicy::required, MetadataPolicy::skip}) {
        if (to_string(value) == text) {
            return value;
        }
    }
    return Error::validation("metadata_policy", "must be best_effort, required or skip");
}

NormalizedDownloadIntent::NormalizedDownloadIntent(std::string owner_id, TrackRef original_track,
                                                   std::vector<AccountBinding> account_bindings,
                                                   ResourceRequirement requirement,
                                                   MetadataPolicy metadata_policy,
                                                   ProductPolicy product)
    : owner_id_(std::move(owner_id)), original_track_(std::move(original_track)),
      account_bindings_(std::move(account_bindings)),
      requirement_(requirement), metadata_policy_(metadata_policy), product_(std::move(product)) {}

Result<NormalizedDownloadIntent> NormalizedDownloadIntent::normalize(DownloadIntentDraft draft) {
    if (auto checked = validate_output_options(draft.product.output); !checked) {
        return checked.error();
    }
    if (!is_valid_service_id("prn_", draft.owner_id)) {
        return Error::validation("owner_id", "must be a prn_ service id");
    }
    if (!is_valid_service_id("dst_", draft.product.destination_id)) {
        return Error::validation("destination_id", "must be a dst_ service id");
    }
    if (draft.product.destination_config_version.empty() ||
        draft.product.naming_config_version.empty() ||
        draft.product.metadata_config_version.empty()) {
        return Error::validation("product", "configuration versions must be set");
    }

    const std::vector<SourceId> needed{draft.original_track.source};
    auto bindings = normalize_bindings(draft.owner_id, needed, std::move(draft.account_bindings));
    if (!bindings) {
        return std::move(bindings).error();
    }

    return NormalizedDownloadIntent{std::move(draft.owner_id),
                                    std::move(draft.original_track),
                                    std::move(bindings).value(),
                                    draft.requirement,
                                    draft.metadata_policy,
                                    std::move(draft.product)};
}

DownloadFingerprint fingerprint(const NormalizedDownloadIntent& intent) {
    detail::CanonicalWriter writer;
    writer.number("schema", DownloadFingerprint::current_schema)
        .field("owner", std::string_view{intent.owner_id()});
    write_ref(writer, "orig", intent.original_track());

    writer.number("acct.count", intent.account_bindings().size());
    for (const auto& binding : intent.account_bindings()) {
        writer.field("acct.src", std::string_view{binding.source().value()})
            .field("acct.mode", to_string(binding.mode()))
            .field("acct.profile", std::string_view{binding.auth_profile()})
            .optional("acct.cred", binding.credential_id())
            .optional("acct.app", binding.app_profile_id());
    }

    const auto& requirement = intent.requirement();
    writer.field("quality", to_string(requirement.requested))
        .field("quality_policy", to_string(requirement.policy))
        .flag("allow_preview", requirement.allow_preview)
        .field("metadata", to_string(intent.metadata_policy()))
        .field("dst", std::string_view{intent.product().destination_id})
        .field("dst.v", std::string_view{intent.product().destination_config_version})
        .field("naming.v", std::string_view{intent.product().naming_config_version})
        .field("metadata.v", std::string_view{intent.product().metadata_config_version});

    const auto& output = intent.product().output;
    writer.field("output.name", std::string_view{output.name_format})
        .field("output.folder", std::string_view{output.folder_layout})
        .flag("output.cover", output.embed_cover)
        .flag("output.lyrics", output.embed_lyrics)
        .number("output.cover_max", output.cover_max_bytes)
        .number("output.timeout", output.metadata_timeout_seconds)
        .flag("output.export_lyrics", output.export_lyrics)
        .flag("output.export_cover", output.export_cover)
        .flag("output.sidecars_required", output.sidecars_required)
        .field("output.lyrics_variant", output.lyrics_variant);
    DownloadFingerprint result;
    result.canonical = writer.str();
    result.digest = to_hex(fnv1a64(result.canonical));
    return result;
}

Result<void> validate_output_options(const OutputOptions& options) {
    if (options.sidecars_required && !options.export_lyrics && !options.export_cover)
        return Error::validation("output.sidecars_required", "requires an enabled sidecar");
    if (options.lyrics_variant != "lrc" && options.lyrics_variant != "verbatim" && options.lyrics_variant != "romanized")
        return Error::validation("output.lyrics_variant", "must be lrc, verbatim or romanized");
    if (options.name_format != "title" && options.name_format != "artist_title" && options.name_format != "title_artist") {
        return Error::validation("output.name_format", "must be title, artist_title or title_artist");
    }
    if (options.folder_layout != "flat" && options.folder_layout != "artist" && options.folder_layout != "artist_album") {
        return Error::validation("output.folder_layout", "must be flat, artist or artist_album");
    }
    if (options.cover_max_bytes == 0 || options.cover_max_bytes > 32 * 1024 * 1024) {
        return Error::validation("output.cover_max_bytes", "must be between 1 and 33554432");
    }
    if (options.metadata_timeout_seconds == 0 || options.metadata_timeout_seconds > 60) {
        return Error::validation("output.metadata_timeout_seconds", "must be between 1 and 60");
    }
    return ok();
}

} // namespace music_service::domain
