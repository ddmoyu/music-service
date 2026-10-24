#include "infrastructure/storage/intent_codec.hpp"

#include <nlohmann/json.hpp>

#include <variant>

namespace music_service::infrastructure::storage {

using json = nlohmann::json;

namespace {

constexpr int schema_version = 1;

json ref_to_json(const domain::TrackRef& ref) {
    return {{"source", ref.source.value()}, {"id", ref.id.value()}};
}

domain::Result<domain::TrackRef> ref_from_json(const json& value) {
    auto source = domain::SourceId::parse(value.at("source").get<std::string>());
    if (!source) {
        return std::move(source).error();
    }
    auto id = domain::PlatformId::parse(value.at("id").get<std::string>());
    if (!id) {
        return std::move(id).error();
    }
    return domain::TrackRef{std::move(source).value(), std::move(id).value()};
}

domain::Result<domain::AccountMode> parse_mode(const std::string& text) {
    for (const auto mode : {domain::AccountMode::anonymous, domain::AccountMode::application,
                            domain::AccountMode::user}) {
        if (domain::to_string(mode) == text) {
            return mode;
        }
    }
    return domain::Error::validation("mode", "unknown account mode");
}

std::optional<std::string> optional_string(const json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) {
        return std::nullopt;
    }
    return it->get<std::string>();
}

domain::Error corrupt(std::string_view reason) {
    return domain::Error{domain::ErrorCode::internal,
                         "stored download intent is invalid: " + std::string{reason}, false, {}};
}

} // namespace

std::string encode_intent(const domain::NormalizedDownloadIntent& intent) {
    const json selection{{"kind", "same"}};

    json accounts = json::array();
    for (const auto& binding : intent.account_bindings()) {
        accounts.push_back({{"principal_id", binding.principal_id()},
                            {"source", binding.source().value()},
                            {"mode", domain::to_string(binding.mode())},
                            {"auth_profile", binding.auth_profile()},
                            {"credential_id", binding.credential_id() ? json(*binding.credential_id()) : json(nullptr)},
                            {"app_profile_id", binding.app_profile_id() ? json(*binding.app_profile_id()) : json(nullptr)}});
    }

    const auto& requirement = intent.requirement();
    const auto& product = intent.product();
    json document{
        {"schema", product.output ? ((product.output->export_lyrics || product.output->export_cover) ? 4 : (product.output->lyrics_variant == "lrc" ? 2 : 3)) : schema_version},
        {"owner_id", intent.owner_id()},
        {"original_track", ref_to_json(intent.original_track())},
        {"selection", selection},
        {"accounts", accounts},
        {"quality", domain::to_string(requirement.requested)},
        {"quality_policy", domain::to_string(requirement.policy)},
        {"allow_preview", requirement.allow_preview},
        {"metadata_policy", domain::to_string(intent.metadata_policy())},
        {"product",
         {{"destination_id", product.destination_id},
          {"destination_config_version", product.destination_config_version},
          {"naming_config_version", product.naming_config_version},
          {"metadata_config_version", product.metadata_config_version}}},
    };
    if (product.output) {
        const auto& o = *product.output;
        document["product"]["output"] = {{"name_format", o.name_format}, {"folder_layout", o.folder_layout},
            {"embed_cover", o.embed_cover}, {"embed_lyrics", o.embed_lyrics},
            {"cover_max_bytes", o.cover_max_bytes}, {"metadata_timeout_seconds", o.metadata_timeout_seconds}};
        if (o.export_lyrics || o.export_cover) {
            document["product"]["output"]["export_lyrics"] = o.export_lyrics;
            document["product"]["output"]["export_cover"] = o.export_cover;
            document["product"]["output"]["sidecars_required"] = o.sidecars_required;
        }
        if (o.lyrics_variant != "lrc") document["product"]["output"]["lyrics_variant"] = o.lyrics_variant;
    }
    return document.dump();
}

domain::Result<domain::NormalizedDownloadIntent> decode_intent(std::string_view text) {
    const auto doc = json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        return corrupt("not a JSON object");
    }
    try {
        const auto schema = doc.at("schema").get<int>();
        if (schema != schema_version && schema != 2 && schema != 3 && schema != 4) {
            return corrupt("unsupported schema");
        }
        auto original = ref_from_json(doc.at("original_track"));
        if (!original) {
            return std::move(original).error();
        }

        domain::SourceSelection selection{domain::SameSource{}};
        const auto& sel = doc.at("selection");
        const auto kind = sel.at("kind").get<std::string>();
        if (kind != "same") {
            return corrupt("source switching has been removed; only same-source intents are supported");
        }

        std::vector<domain::AccountBinding> bindings;
        for (const auto& item : doc.at("accounts")) {
            auto source = domain::SourceId::parse(item.at("source").get<std::string>());
            auto mode = parse_mode(item.at("mode").get<std::string>());
            if (!source || !mode) {
                return corrupt("invalid account binding");
            }
            auto binding = domain::AccountBinding::make(
                item.at("principal_id").get<std::string>(), std::move(source).value(), *mode,
                item.at("auth_profile").get<std::string>(), optional_string(item, "credential_id"),
                optional_string(item, "app_profile_id"));
            if (!binding) {
                return std::move(binding).error();
            }
            bindings.push_back(std::move(binding).value());
        }

        auto quality = domain::parse_quality(doc.at("quality").get<std::string>());
        auto policy = domain::parse_quality_policy(doc.at("quality_policy").get<std::string>());
        auto metadata = domain::parse_metadata_policy(doc.at("metadata_policy").get<std::string>());
        if (!quality || !policy || !metadata) {
            return corrupt("invalid policy value");
        }
        const auto& product = doc.at("product");

        domain::DownloadIntentDraft draft{
            doc.at("owner_id").get<std::string>(),
            std::move(original).value(),
            std::move(selection),
            std::move(bindings),
            {*quality, *policy, doc.at("allow_preview").get<bool>()},
            *metadata,
            {product.at("destination_id").get<std::string>(),
             product.at("destination_config_version").get<std::string>(),
             product.at("naming_config_version").get<std::string>(),
             product.at("metadata_config_version").get<std::string>()},
        };
        if (schema >= 2) {
            const auto& o = product.at("output");
            // 先以宽类型读取和核验，避免负数或溢出被截成合法的无符号值。
            const auto bytes = o.at("cover_max_bytes").get<std::int64_t>();
            const auto timeout = o.at("metadata_timeout_seconds").get<std::int64_t>();
            if (bytes <= 0 || bytes > 32 * 1024 * 1024 || timeout <= 0 || timeout > 60) {
                return corrupt("invalid output limits");
            }
            draft.product.output = domain::OutputOptions{o.at("name_format").get<std::string>(),
                o.at("folder_layout").get<std::string>(), o.at("embed_cover").get<bool>(),
                o.at("embed_lyrics").get<bool>(), static_cast<std::uint64_t>(bytes),
                static_cast<std::uint32_t>(timeout)};
            if (schema == 3) draft.product.output->lyrics_variant = o.at("lyrics_variant").get<std::string>();
            if (schema == 4) draft.product.output->lyrics_variant = o.value("lyrics_variant", std::string{"lrc"});
            if (schema == 4) {
                draft.product.output->export_lyrics = o.at("export_lyrics").get<bool>();
                draft.product.output->export_cover = o.at("export_cover").get<bool>();
                draft.product.output->sidecars_required = o.at("sidecars_required").get<bool>();
            }
        }
        return domain::NormalizedDownloadIntent::normalize(std::move(draft));
    } catch (const json::exception&) {
        return corrupt("missing or mistyped field");
    }
}

} // namespace music_service::infrastructure::storage
