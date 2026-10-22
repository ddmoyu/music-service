#include "infrastructure/filesystem/destination_files.hpp"

#include "infrastructure/filesystem/data_layout.hpp"
#include "infrastructure/filesystem/files.hpp"
#include "infrastructure/filesystem/paths.hpp"

#include "domain/catalog/refs.hpp"

#include <mbedtls/sha256.h>

#include <algorithm>
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

domain::Result<bool> LocalDestinationFiles::matches(const std::string& destination_id, const std::string& relative_path,
                                                    const ports::FileFingerprint& expected) {
    auto actual = fingerprint(destination_id, relative_path);
    if (!actual) return actual.error();
    return actual.value() && **actual == expected;
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

domain::Result<std::vector<std::string>> LocalDestinationFiles::list_workspaces(const std::string& destination_id) {
    auto base = root(destination_id);
    if (!base) {
        return std::move(base).error();
    }
    const auto directory = base.value() / std::string{workspace_directory_name};
    std::vector<std::string> ids;
    std::error_code ec;
    fs::directory_iterator it{directory, ec};
    if (ec) {
        if (absent(ec)) {
            return ids; // nothing was ever downloaded here
        }
        return file_error("list workspaces", directory, ec);
    }
    for (const auto end = fs::directory_iterator{}; it != end; it.increment(ec)) {
        if (ec) {
            return file_error("list workspaces", directory, ec);
        }
        const auto name = path_to_utf8(it->path().filename());
        if (domain::is_valid_service_id("tsk_", name)) {
            ids.push_back(name);
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

namespace {

class LocalReadableFile final : public ports::ReadableFile {
public:
    LocalReadableFile(std::ifstream in, fs::path path, std::uint64_t size)
        : in_(std::move(in)), path_(std::move(path)), size_(size) {}

    [[nodiscard]] std::uint64_t size() const noexcept override { return size_; }

    domain::Result<std::size_t> read(std::uint64_t offset, std::span<char> buffer) override {
        if (offset >= size_ || buffer.empty()) {
            return std::size_t{0};
        }
        in_.clear();
        in_.seekg(static_cast<std::streamoff>(offset));
        const auto wanted = static_cast<std::streamsize>(std::min<std::uint64_t>(buffer.size(), size_ - offset));
        in_.read(buffer.data(), wanted);
        if (in_.gcount() <= 0) {
            return file_error("read file", path_, std::make_error_code(std::errc::io_error));
        }
        return static_cast<std::size_t>(in_.gcount());
    }

private:
    std::ifstream in_;
    fs::path path_;
    std::uint64_t size_;
};

class LocalWorkFile final : public ports::WorkFile {
public:
    explicit LocalWorkFile(AppendFile file) : file_(std::move(file)) {}

    domain::Result<void> write(std::span<const char> bytes) override {
        return file_.write(std::string_view{bytes.data(), bytes.size()});
    }
    domain::Result<void> flush() override { return file_.flush(); }
    domain::Result<void> truncate(std::uint64_t length) override { return file_.truncate(length); }
    [[nodiscard]] std::uint64_t size() const noexcept override { return file_.size(); }

private:
    AppendFile file_;
};

} // namespace

std::string LocalDestinationFiles::relative_path(const std::string& task_id, const std::string& name) const {
    return std::string{workspace_directory_name} + "/" + task_id + "/" + name;
}

domain::Result<std::unique_ptr<ports::WorkFile>> LocalDestinationFiles::open(const std::string& destination_id,
                                                                             const std::string& task_id,
                                                                             const std::string& name,
                                                                             std::uint64_t keep_bytes) {
    auto path = resolve(destination_id, relative_path(task_id, name));
    if (!path) {
        return std::move(path).error();
    }
    if (auto made = create_directories(path->parent_path(), Privacy::normal); !made) {
        return made.error();
    }
    auto file = AppendFile::open(path.value());
    if (!file) {
        return std::move(file).error();
    }
    if (auto cut = file->truncate(keep_bytes); !cut) {
        return cut.error();
    }
    return std::unique_ptr<ports::WorkFile>{std::make_unique<LocalWorkFile>(std::move(file).value())};
}

domain::Result<std::unique_ptr<ports::ReadableFile>>
LocalDestinationFiles::open_read(const std::string& destination_id, const std::string& relative_path) {
    auto path = resolve(destination_id, relative_path);
    if (!path) {
        return std::move(path).error();
    }
    std::error_code ec;
    const auto status = fs::status(path.value(), ec);
    if ((ec && absent(ec)) || (!ec && !fs::is_regular_file(status))) {
        return domain::Error{domain::ErrorCode::not_found, "no published file at this path", false, {}};
    }
    if (ec) {
        return file_error("inspect", path.value(), ec);
    }
    const auto size = fs::file_size(path.value(), ec);
    if (ec) {
        return file_error("inspect", path.value(), ec);
    }
    std::ifstream in{path.value(), std::ios::binary};
    if (!in) {
        return file_error("open file", path.value(), std::make_error_code(std::errc::io_error));
    }
    return std::unique_ptr<ports::ReadableFile>{
        std::make_unique<LocalReadableFile>(std::move(in), path.value(), static_cast<std::uint64_t>(size))};
}

domain::Result<fs::path> LocalDestinationFiles::local_path(const std::string& destination_id,
                                                           const std::string& relative_path) {
    return resolve(destination_id, relative_path);
}

} // namespace music_service::infrastructure::filesystem
