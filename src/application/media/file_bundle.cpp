#include "application/media/file_bundle.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <set>

namespace music_service::application::media {
domain::Result<std::vector<BundleMember>> bundle_members(std::string_view properties) {
    const auto fail = [] { return domain::Error::validation("bundle", "invalid artifact manifest"); };
    const auto doc = nlohmann::json::parse(properties, nullptr, false);
    if (!doc.is_object()) return fail();
    std::vector<BundleMember> result;
    if (!doc.contains("bundle")) return result;
    const auto& entries = doc["bundle"];
    if (!entries.is_array() || entries.empty() || entries.size() > 3) return fail();
    std::set<std::string> kinds;
    std::set<std::string> names;
    try {
        for (const auto& entry : entries) {
            BundleMember member{entry.at("name").get<std::string>(), entry.at("kind").get<std::string>(),
                entry.at("container").get<std::string>(),
                {entry.at("size_bytes").get<std::uint64_t>(), entry.at("sha256").get<std::string>()}};
            if (member.name.empty() || member.name == "." || member.name == ".." ||
                member.name.find_first_of("/\\:\0", 0, 4) != std::string::npos ||
                (member.kind != "audio" && member.kind != "lyrics" && member.kind != "cover") ||
                !kinds.insert(member.kind).second || !names.insert(member.name).second ||
                !entry.at("size_bytes").is_number_unsigned() || member.file.sha256.size() != 64 ||
                !std::all_of(member.file.sha256.begin(), member.file.sha256.end(), [](char c) {
                    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) return fail();
            result.push_back(std::move(member));
        }
    } catch (const nlohmann::json::exception&) { return fail(); }
    if (result.front().kind != "audio") return fail();
    return result;
}
std::string with_bundle(std::string_view properties, const std::vector<BundleMember>& members) {
    auto doc = nlohmann::json::parse(properties);
    doc["bundle"] = nlohmann::json::array();
    for (const auto& member : members) doc["bundle"].push_back({{"name", member.name}, {"kind", member.kind},
        {"container", member.container}, {"size_bytes", member.file.size_bytes}, {"sha256", member.file.sha256}});
    return doc.dump();
}
std::string sibling_path(std::string_view audio_path, std::string_view filename) {
    const auto slash = audio_path.rfind('/');
    return (slash == std::string_view::npos ? std::string{} : std::string{audio_path.substr(0, slash + 1)}) + std::string{filename};
}
} // namespace music_service::application::media
