#pragma once

#include "domain/result.hpp"

#include <algorithm>
#include <array>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace music_service::application::accounts {

// Access token scopes defined by the API (api-contract.md, 身份、权限与请求约定).
namespace scope {
inline constexpr std::string_view catalog_read = "catalog:read";
inline constexpr std::string_view downloads_read = "downloads:read";
inline constexpr std::string_view downloads_write = "downloads:write";
inline constexpr std::string_view files_read = "files:read";
inline constexpr std::string_view media_read = "media:read";
inline constexpr std::string_view credentials_write = "credentials:write";
inline constexpr std::string_view admin = "admin";
} // namespace scope

inline constexpr std::array<std::string_view, 7> all_scopes{
    scope::catalog_read, scope::downloads_read,    scope::downloads_write, scope::files_read,
    scope::media_read,   scope::credentials_write, scope::admin,
};

// Scopes for a new token must be given explicitly, be known and not repeat.
inline domain::Result<void> validate_scopes(const std::vector<std::string>& scopes) {
    if (scopes.empty()) {
        return domain::Error::validation("scopes", "at least one scope is required");
    }
    std::set<std::string_view> seen;
    for (const auto& value : scopes) {
        if (std::find(all_scopes.begin(), all_scopes.end(), value) == all_scopes.end()) {
            return domain::Error::validation("scopes", "unknown scope " + value);
        }
        if (!seen.insert(value).second) {
            return domain::Error::validation("scopes", "lists " + value + " twice");
        }
    }
    return domain::ok();
}

[[nodiscard]] inline bool has_scope(const std::vector<std::string>& granted, std::string_view wanted) {
    return std::find(granted.begin(), granted.end(), wanted) != granted.end();
}

} // namespace music_service::application::accounts
