#include "infrastructure/filesystem/destination_files.hpp"

#include "infrastructure/filesystem/data_layout.hpp"
#include "infrastructure/filesystem/files.hpp"
#include "infrastructure/filesystem/paths.hpp"

#include "domain/catalog/refs.hpp"

#include <mbedtls/sha256.h>

#include <array>
#include <fstream>
#include <utility>
#include <vector>

namespace music_service::infrastructure::filesystem {

namespace fs = std::filesystem;
namespace ports = application::ports;

namespace {

domain::Error unavailable(const std::string& destination_id) {
    return domain::Error{domain::ErrorCode::destination_unavailable,
                         "destination '" + destination_id + "' is not available", true, {}};
}

// A parent that is a file means the path cannot exist; Windows reports it as not found.
bool absent(const std::error_code& ec) {
    return ec == std::errc::no_such_file_or_directory || ec == std::errc::not_a_directory;
}

std::string hex(const unsigned char* data, std::size_t size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 0x0F]);
    }
    return out;
}

} // namespace

LocalDestinationFiles::LocalDestinationFiles(std::map<std::string, fs::path> roots) : roots_(std::move(roots)) {}

domain::Result<fs::path> LocalDestinationFiles::root(const std::string& destination_id) const {
    const auto found = roots_.find(destination_id);
    std::error_code ec;
    if (found == roots_.end() || !fs::is_directory(found->second, ec)) {
        return unavailable(destination_id);
    }
    return found->second;
}

domain::Result<fs::path> LocalDestinationFiles::resolve(const std::string& destination_id,
                                                        const std::string& relative_path) const {
    auto base = root(destination_id);
    if (!base) {
        return base;
    }
    return resolve_within(base.value(), relative_path);
}

domain::Result<bool> LocalDestinationFiles::exists(const std::string& destination_id,
                                                   const std::string& relative_path) {
    auto path = resolve(destination_id, relative_path);
    if (!path) {
        return std::move(path).error();
    }
    std::error_code ec;
    const auto status = fs::symlink_status(path.value(), ec);
    if (ec && !absent(ec)) {
        return file_error("inspect", path.value(), ec);
    }
    return !ec && fs::exists(status);
}

domain::Result<std::optional<ports::FileFingerprint>>
LocalDestinationFiles::fingerprint(const std::string& destination_id, const std::string& relative_path) {
    auto path = resolve(destination_id, relative_path);
    if (!path) {
        return std::move(path).error();
    }
    std::error_code ec;
    const auto status = fs::symlink_status(path.value(), ec);
    if (absent(ec) || (!ec && !fs::exists(status))) {
        return std::optional<ports::FileFingerprint>{};
    }
    if (ec) {
        return file_error("inspect", path.value(), ec);
    }
    if (!fs::is_regular_file(status)) {
        return domain::Error{domain::ErrorCode::file_system, "not a regular file", false,
                             {{"path", relative_path}}};
    }
    std::ifstream in{path.value(), std::ios::binary};
    if (!in) {
        return file_error("open", path.value(), std::make_error_code(std::errc::io_error));
    }
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    std::vector<char> buffer(64 * 1024);
    std::uint64_t size = 0;
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto got = static_cast<std::size_t>(in.gcount());
        size += got;
        mbedtls_sha256_update(&sha, reinterpret_cast<const unsigned char*>(buffer.data()), got);
    }
    const bool failed = in.bad();
    std::array<unsigned char, 32> digest{};
    mbedtls_sha256_finish(&sha, digest.data());
    mbedtls_sha256_free(&sha);
    if (failed) {
        return file_error("read", path.value(), std::make_error_code(std::errc::io_error));
    }
    return std::optional<ports::FileFingerprint>{ports::FileFingerprint{size, hex(digest.data(), digest.size())}};
}

domain::Result<void> LocalDestinationFiles::move(const std::string& destination_id, const std::string& from,
                                                 const std::string& to) {
    auto base = root(destination_id);
    if (!base) {
        return std::move(base).error();
    }
    auto source = resolve_within(base.value(), from);
    if (!source) {
        return std::move(source).error();
    }
    auto target = resolve_within(base.value(), to);
    if (!target) {
        return std::move(target).error();
    }
    const auto parent = target->parent_path();
    if (auto created = create_directories(parent, Privacy::normal); !created) {
        return created;
    }
    // A link swapped in while the parents were created must not lead outside the root.
    auto resolved_parent = canonical_directory(parent);
    if (!resolved_parent) {
        return std::move(resolved_parent).error();
    }
    if (!is_within(base.value(), resolved_parent.value())) {
        return domain::Error::validation("path", "resolves outside the root directory");
    }
    auto present = exists(destination_id, to);
    if (!present) {
        return std::move(present).error();
    }
    if (present.value()) {
        return domain::Error{domain::ErrorCode::conflict, "target file already exists", true, {}};
    }
    if (auto renamed = rename_no_replace(source.value(), target.value()); !renamed) {
        // Lost a race with another writer: still never replaced.
        auto now_present = exists(destination_id, to);
        if (now_present && now_present.value()) {
            std::error_code ec;
            if (fs::exists(source.value(), ec)) {
                return domain::Error{domain::ErrorCode::conflict, "target file already exists", true, {}};
            }
        }
        return renamed;
    }
    return domain::ok();
}

domain::Result<void> LocalDestinationFiles::remove_workspace(const std::string& destination_id,
                                                             const std::string& task_id) {
    if (!domain::is_valid_service_id("tsk_", task_id)) {
        return domain::Error::validation("task_id", "must be a tsk_ service id");
    }
    auto path = resolve(destination_id, std::string{workspace_directory_name} + "/" + task_id);
    if (!path) {
        return std::move(path).error();
    }
    std::error_code ec;
    // remove_all does not follow a link planted at the path; it removes the link itself.
    fs::remove_all(path.value(), ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
        return file_error("remove workspace", path.value(), ec);
    }
    return domain::ok();
}

} // namespace music_service::infrastructure::filesystem
