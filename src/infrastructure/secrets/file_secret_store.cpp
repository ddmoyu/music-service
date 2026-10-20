#include "infrastructure/secrets/file_secret_store.hpp"

#include "infrastructure/filesystem/files.hpp"
#include "infrastructure/filesystem/paths.hpp"

#include "domain/catalog/refs.hpp"

#include <system_error>
#include <utility>

namespace music_service::infrastructure::secrets {

namespace fs = std::filesystem;
using application::ports::SecretBytes;

namespace {

constexpr std::string_view prefix = "sec_";
constexpr std::string_view extension = ".secret";

domain::Result<void> check_ref(std::string_view ref) {
    // The reference becomes a file name, so only the service id alphabet is accepted.
    if (!domain::is_valid_service_id(prefix, ref)) {
        return domain::Error::validation("secret_ref", "must be a sec_ service id");
    }
    return domain::ok();
}

} // namespace

FileSecretStore::FileSecretStore(fs::path directory) : directory_(std::move(directory)) {}

fs::path FileSecretStore::file_for(std::string_view ref) const {
    return directory_ / filesystem::path_from_utf8(std::string{ref} + std::string{extension});
}

domain::Result<std::string> FileSecretStore::put(const SecretBytes& secret) {
    if (secret.empty() || secret.size() > max_secret_bytes) {
        return domain::Error::validation("secret", "must be 1 byte to 64 KiB");
    }
    for (int attempt = 0; attempt < 3; ++attempt) {
        auto ref = ids_.next(prefix);
        const auto path = file_for(ref);
        std::error_code ec;
        if (fs::exists(path, ec)) {
            continue; // never reuse or overwrite a reference
        }
        auto written = filesystem::write_new_file(path, secret.reveal(),
                                                  filesystem::Privacy::owner_only);
        if (!written) {
            return written.error();
        }
        return ref;
    }
    return domain::Error{domain::ErrorCode::internal, "could not allocate a secret reference",
                         false, {}};
}

domain::Result<SecretBytes> FileSecretStore::get(std::string_view ref) {
    if (auto valid = check_ref(ref); !valid) {
        return valid.error();
    }
    auto content = filesystem::read_file(file_for(ref), max_secret_bytes);
    if (!content) {
        auto error = std::move(content).error();
        if (error.code == domain::ErrorCode::not_found) {
            error.message = "secret reference does not exist";
        }
        return error;
    }
    return SecretBytes{std::move(content).value()};
}

domain::Result<void> FileSecretStore::remove(std::string_view ref) {
    if (auto valid = check_ref(ref); !valid) {
        return valid;
    }
    return filesystem::remove_file(file_for(ref));
}

domain::Result<std::vector<std::string>> FileSecretStore::list() {
    std::vector<std::string> refs;
    std::error_code ec;
    for (fs::directory_iterator it{directory_, ec}, end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != extension) {
            continue;
        }
        auto ref = filesystem::path_to_utf8(it->path().stem());
        if (domain::is_valid_service_id(prefix, ref)) {
            refs.push_back(std::move(ref));
        }
    }
    if (ec) {
        return filesystem::file_error("list secrets", directory_, ec);
    }
    return refs;
}

} // namespace music_service::infrastructure::secrets
