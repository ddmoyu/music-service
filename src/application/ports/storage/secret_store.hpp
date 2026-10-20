#pragma once

#include "domain/result.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace music_service::application::ports {

// Secret material (cookie, token, API key). Move-only, wiped on destruction and never
// printable: there is deliberately no stream operator or implicit string conversion.
// The bytes live in a heap buffer owned by this object, so a move transfers the pointer
// and leaves no copy behind (std::string's inline short-string buffer would).
class SecretBytes {
public:
    SecretBytes() = default;
    // Copies the bytes into the owned buffer and wipes the argument.
    explicit SecretBytes(std::string bytes) : SecretBytes(std::string_view{bytes}) {
        wipe(bytes.data(), bytes.size());
    }
    explicit SecretBytes(std::string_view bytes)
        : data_(bytes.empty() ? nullptr : std::make_unique<char[]>(bytes.size())),
          size_(bytes.size()) {
        std::copy(bytes.begin(), bytes.end(), data_.get());
    }
    explicit SecretBytes(const char* bytes) : SecretBytes(std::string_view{bytes}) {}
    SecretBytes(const SecretBytes&) = delete;
    SecretBytes& operator=(const SecretBytes&) = delete;
    SecretBytes(SecretBytes&& other) noexcept
        : data_(std::move(other.data_)), size_(std::exchange(other.size_, 0)) {}
    SecretBytes& operator=(SecretBytes&& other) noexcept {
        if (this != &other) {
            clear();
            data_ = std::move(other.data_);
            size_ = std::exchange(other.size_, 0);
        }
        return *this;
    }
    ~SecretBytes() { clear(); }

    // Read access for the code that must send the secret upstream.
    [[nodiscard]] std::string_view reveal() const noexcept { return {data_.get(), size_}; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    // Safe for logs: never contains the value.
    [[nodiscard]] std::string describe() const {
        return "[secret " + std::to_string(size_) + " bytes]";
    }

private:
    static void wipe(char* bytes, std::size_t size) noexcept {
        volatile char* data = bytes;
        for (std::size_t i = 0; i < size; ++i) {
            data[i] = 0;
        }
    }
    void clear() noexcept {
        wipe(data_.get(), size_);
        data_.reset();
        size_ = 0;
    }

    std::unique_ptr<char[]> data_;
    std::size_t size_{0};
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
