#include "infrastructure/auth/access_tokens.hpp"

#include "infrastructure/filesystem/files.hpp"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/sha256.h>

#include <array>
#include <mutex>
#include <string>

namespace music_service::infrastructure::auth {

namespace {

constexpr std::size_t token_random_bytes = 32;

std::string base64url(const unsigned char* data, std::size_t size) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((size * 4 + 2) / 3);
    std::size_t i = 0;
    for (; i + 2 < size; i += 3) {
        const std::uint32_t n = (std::uint32_t{data[i]} << 16) | (std::uint32_t{data[i + 1]} << 8) | data[i + 2];
        out.push_back(alphabet[(n >> 18) & 63]);
        out.push_back(alphabet[(n >> 12) & 63]);
        out.push_back(alphabet[(n >> 6) & 63]);
        out.push_back(alphabet[n & 63]);
    }
    if (const auto rest = size - i; rest > 0) {
        std::uint32_t n = std::uint32_t{data[i]} << 16;
        if (rest == 2) {
            n |= std::uint32_t{data[i + 1]} << 8;
        }
        out.push_back(alphabet[(n >> 18) & 63]);
        out.push_back(alphabet[(n >> 12) & 63]);
        if (rest == 2) {
            out.push_back(alphabet[(n >> 6) & 63]);
        }
    }
    return out;
}

domain::Error crypto_error(const char* what) {
    return domain::Error{domain::ErrorCode::internal, what, false, {}};
}

} // namespace

struct MbedTlsTokenCodec::State {
    std::mutex mutex;
    mbedtls_entropy_context entropy{};
    mbedtls_ctr_drbg_context drbg{};
    bool seeded{false};

    State() {
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);
    }
    ~State() {
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);
    }
    State(const State&) = delete;
    State& operator=(const State&) = delete;
};

MbedTlsTokenCodec::MbedTlsTokenCodec() : state_(std::make_unique<State>()) {}

MbedTlsTokenCodec::~MbedTlsTokenCodec() = default;

domain::Result<application::ports::SecretBytes> MbedTlsTokenCodec::generate() {
    std::scoped_lock lock{state_->mutex};
    if (!state_->seeded) {
        static constexpr unsigned char personalization[] = "music-service access token";
        if (mbedtls_ctr_drbg_seed(&state_->drbg, mbedtls_entropy_func, &state_->entropy,
                                  personalization, sizeof(personalization) - 1) != 0) {
            return crypto_error("random generator could not be seeded");
        }
        state_->seeded = true;
    }
    std::array<unsigned char, token_random_bytes> random{};
    if (mbedtls_ctr_drbg_random(&state_->drbg, random.data(), random.size()) != 0) {
        return crypto_error("random generator failed");
    }
    auto token = std::string{prefix} + base64url(random.data(), random.size());
    random.fill(0);
    return application::ports::SecretBytes{std::move(token)};
}

domain::Result<std::string> MbedTlsTokenCodec::pkce_challenge(std::string_view verifier) const {
    std::array<unsigned char, 32> digest{};
    if (mbedtls_sha256(reinterpret_cast<const unsigned char*>(verifier.data()), verifier.size(), digest.data(), 0) != 0)
        return crypto_error("PKCE digest failed");
    return base64url(digest.data(), digest.size());
}

std::string MbedTlsTokenCodec::hash(std::string_view token) const {
    std::array<unsigned char, 32> digest{};
    mbedtls_sha256(reinterpret_cast<const unsigned char*>(token.data()), token.size(), digest.data(), 0);
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(digest.size() * 2);
    for (const auto byte : digest) {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0x0F]);
    }
    return out;
}

domain::Result<void> FileTokenDelivery::deliver(const std::filesystem::path& path,
                                                const application::ports::SecretBytes& token) {
    std::string content{token.reveal()};
    content.push_back('\n');
    auto written = filesystem::write_new_file(path, content, filesystem::Privacy::owner_only);
    application::ports::SecretBytes wipe{std::move(content)}; // clears the temporary copy
    return written;
}

void FileTokenDelivery::discard(const std::filesystem::path& path) noexcept {
    try {
        (void)filesystem::remove_file(path);
    } catch (...) {
        // Best effort: a leftover file never becomes a valid token.
    }
}

} // namespace music_service::infrastructure::auth
