#pragma once

#include "domain/result.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace music_service::application::ports {

// Secret material (cookie, token, API key). Move-only, wiped on destruction and never
// printable: there is deliberately no stream operator or implicit string conversion.
class SecretBytes {
public:
    SecretBytes() = default;
    explicit SecretBytes(std::string bytes) : bytes_(std::move(bytes)) {}
    SecretBytes(const SecretBytes&) = delete;
    SecretBytes& operator=(const SecretBytes&) = delete;
    SecretBytes(SecretBytes&& other) noexcept : bytes_(std::move(other.bytes_)) {
        other.wipe();
    }
    SecretBytes& operator=(SecretBytes&& other) noexcept {
        if (this != &other) {
            wipe();
            bytes_ = std::move(other.bytes_);
            other.wipe();
        }
        return *this;
    }
    ~SecretBytes() { wipe(); }

    // Read access for the code that must send the secret upstream.
    [[nodiscard]] std::string_view reveal() const noexcept { return bytes_; }
    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
    [[nodiscard]] bool empty() const noexcept { return bytes_.empty(); }

    // Safe for logs: never contains the value.
    [[nodiscard]] std::string describe() const {
        return "[secret " + std::to_string(bytes_.size()) + " bytes]";
    }

private:
    void wipe() noexcept {
        volatile char* data = bytes_.data();
        for (std::size_t i = 0; i < bytes_.size(); ++i) {
            data[i] = 0;
        }
        bytes_.clear();
    }

    std::string bytes_;
};

// Stores secrets under immutable references ("sec_..."). An update writes a new
// reference and the database switches to it; the old one is removed afterwards
// (internal-contracts.md, 凭证和秘密提交). Implementations never overwrite.
class SecretStore {
public:
    static constexpr std::size_t max_secret_bytes = 64 * 1024;

    virtual ~SecretStore() = default;

    virtual domain::Result<std::string> put(const SecretBytes& secret) = 0;
    virtual domain::Result<SecretBytes> get(std::string_view ref) = 0;
    // Removing an unknown reference succeeds, so cleanup can be retried.
    virtual domain::Result<void> remove(std::string_view ref) = 0;
    // All stored references, for reconciling orphans against the database.
    virtual domain::Result<std::vector<std::string>> list() = 0;
};

} // namespace music_service::application::ports
