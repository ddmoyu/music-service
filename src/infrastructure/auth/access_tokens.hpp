#pragma once

#include "application/ports/identity/access_tokens.hpp"

#include <memory>

namespace music_service::infrastructure::auth {

// Tokens are "mst_" followed by 43 base64url characters (32 random bytes from Mbed TLS
// CTR_DRBG seeded by the platform entropy source). The stored hash is the lowercase
// hex SHA-256 of the whole token. Thread-safe.
class MbedTlsTokenCodec final : public application::ports::AccessTokenCodec {
public:
    static constexpr std::string_view prefix = "mst_";

    MbedTlsTokenCodec();
    ~MbedTlsTokenCodec() override;
    MbedTlsTokenCodec(const MbedTlsTokenCodec&) = delete;
    MbedTlsTokenCodec& operator=(const MbedTlsTokenCodec&) = delete;

    domain::Result<application::ports::SecretBytes> generate() override;
    [[nodiscard]] std::string hash(std::string_view token) const override;
    domain::Result<std::string> pkce_challenge(std::string_view verifier) const override;

private:
    struct State;
    std::unique_ptr<State> state_;
};

// Writes the token, followed by a newline, to a new owner-only file and flushes it.
// Refuses existing files; never reads them back.
class FileTokenDelivery final : public application::ports::TokenDelivery {
public:
    domain::Result<void> deliver(const std::filesystem::path& path,
                                 const application::ports::SecretBytes& token) override;
    void discard(const std::filesystem::path& path) noexcept override;
};

} // namespace music_service::infrastructure::auth
