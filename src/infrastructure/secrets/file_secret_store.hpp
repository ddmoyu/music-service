#pragma once

#include "application/ports/storage/secret_store.hpp"
#include "infrastructure/observability/correlation.hpp"

#include <filesystem>

namespace music_service::infrastructure::secrets {

// One owner-only file per secret in data_dir/secrets/credentials, named after its
// reference. Protection relies on operating-system permissions in the first version
// (configuration-and-deployment.md, 凭证与 TLS); a system key store can replace this
// adapter later without changing callers.
class FileSecretStore final : public application::ports::SecretStore {
public:
    explicit FileSecretStore(std::filesystem::path directory);

    domain::Result<std::string> put(const application::ports::SecretBytes& secret) override;
    domain::Result<application::ports::SecretBytes> get(std::string_view ref) override;
    domain::Result<void> remove(std::string_view ref) override;
    domain::Result<std::vector<std::string>> list() override;

private:
    [[nodiscard]] std::filesystem::path file_for(std::string_view ref) const;

    std::filesystem::path directory_;
    observability::IdGenerator ids_;
};

} // namespace music_service::infrastructure::secrets
