#pragma once

#include "application/ports/storage/secret_store.hpp"
#include "domain/result.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace music_service::application::ports {

// Generates service access tokens and derives the hash stored in SQLite. Only the
// hash is ever persisted; the plaintext leaves the process once, through
// TokenDelivery.
class AccessTokenCodec {
public:
    virtual ~AccessTokenCodec() = default;

    // A new token from a cryptographic random source.
    virtual domain::Result<SecretBytes> generate() = 0;
    // Deterministic, one-way; the same token always gives the same hash.
    [[nodiscard]] virtual std::string hash(std::string_view token) const = 0;
    // OAuth PKCE S256；随机 verifier 使用 generate()，只允许 URL 安全字符。
    virtual domain::Result<std::string> pkce_challenge(std::string_view) const {
        return domain::Error{domain::ErrorCode::capability_unsupported, "PKCE is unavailable", false, {}};
    }
};

// Hands a token's plaintext to the operator through a new owner-only file. Never
// overwrites: an existing file is an error. The plaintext goes nowhere else (no
// arguments, stdout or logs).
class TokenDelivery {
public:
    virtual ~TokenDelivery() = default;

    // Creates the file exclusively, writes and flushes the token.
    virtual domain::Result<void> deliver(const std::filesystem::path& path, const SecretBytes& token) = 0;
    // Best-effort removal of a file this delivery created whose token never became valid.
    virtual void discard(const std::filesystem::path& path) noexcept = 0;
};

} // namespace music_service::application::ports
