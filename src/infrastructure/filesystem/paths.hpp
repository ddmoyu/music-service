#pragma once

#include "domain/result.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace music_service::infrastructure::filesystem {

// API and configuration text is UTF-8; std::filesystem::path(std::string) would use the
// Windows ANSI code page, so conversions always go through char8_t.
std::filesystem::path path_from_utf8(std::string_view text);
std::string path_to_utf8(const std::filesystem::path& path);

// Builds a FILE_SYSTEM_ERROR (or the given code) for logs and startup diagnostics.
// The OS error is reported as category and number, which stays ASCII on every system.
// Server paths must not be copied into public API responses.
domain::Error file_error(std::string_view operation, const std::filesystem::path& path,
                         const std::error_code& ec,
                         domain::ErrorCode code = domain::ErrorCode::file_system);

// Absolute, normalized form of an existing directory with symlinks and reparse points
// resolved. Fails when the directory is missing, so an offline mount never silently
// becomes a new empty local directory.
domain::Result<std::filesystem::path> canonical_directory(const std::filesystem::path& path);

// True when candidate equals root or lies below it, comparing normalized components.
bool is_within(const std::filesystem::path& root, const std::filesystem::path& candidate);

// Checks one relative path written with '/' separators before it is joined to a root:
// non-empty components only, no "." or "..", no absolute or drive-relative forms, no
// backslash, colon, control characters or characters Windows forbids, no trailing dot
// or space, no Windows reserved device names. Applied on every system so files remain
// portable between Windows, Linux and NAS shares.
domain::Result<void> check_relative_path(std::string_view relative_utf8);

// Joins a checked relative path to a canonical root and verifies that the deepest
// existing ancestor of the result, after resolving symlinks and reparse points, is
// still inside the root.
domain::Result<std::filesystem::path> resolve_within(const std::filesystem::path& canonical_root,
                                                     std::string_view relative_utf8);

// Turns display text (title, artist) into one safe path component: forbidden and
// control characters become '_', leading/trailing spaces and trailing dots are
// removed, reserved names get a '_' prefix, and the result is cut at a UTF-8
// boundary to max_bytes. Never returns an empty string.
std::string sanitize_file_name(std::string_view text, std::size_t max_bytes = 180);

} // namespace music_service::infrastructure::filesystem
