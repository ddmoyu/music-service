#include "providers/netease/protocol/weapi.hpp"

#include "application/transport/url.hpp"

#include <mbedtls/aes.h>
#include <mbedtls/base64.h>
#include <mbedtls/bignum.h>

#include <algorithm>
#include <array>
#include <random>
#include <stdexcept>
#include <vector>

namespace music_service::providers::netease::protocol {

namespace {

constexpr std::string_view preset_key = "0CoJUm6Qyw8W8jud";
constexpr std::string_view iv_text = "0102030405060708";
constexpr std::string_view public_exponent = "010001";
// The modulus the web client embeds (1024 bits).
constexpr std::string_view modulus =
    "00e0b509f6259df8642dbc35662901477df22677ec152b5ff68ace615bb7b725152b3ab17a876aea8a5aa76d2e417629ec4ee341f56"
    "135fccf695280104e0312ecbda92557c93870114af6c9d05c4f7f0c3685b7a46bee255932575cce10b424d813cfe4875d3e82047b9"
    "7ddef52741d546b8e289dc6935b3ece0462db0a22b8e7";

// Mbed TLS failures here mean a broken build or out of memory, not bad input.
void check(int rc, const char* what) {
    if (rc != 0) {
        throw std::runtime_error(std::string{"weapi: "} + what + " failed");
    }
}

std::string aes_cbc(std::string_view text, std::string_view key) {
    const auto pad = 16 - text.size() % 16;
    std::vector<unsigned char> input(text.begin(), text.end());
    input.insert(input.end(), pad, static_cast<unsigned char>(pad));
    std::vector<unsigned char> output(input.size());
    std::array<unsigned char, 16> iv{};
    std::copy(iv_text.begin(), iv_text.end(), iv.begin());

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    const int rc_key =
        mbedtls_aes_setkey_enc(&aes, reinterpret_cast<const unsigned char*>(key.data()), 128);
    const int rc =
        rc_key != 0 ? rc_key
                    : mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, input.size(), iv.data(), input.data(),
                                            output.data());
    mbedtls_aes_free(&aes);
    check(rc, "AES");
    return {output.begin(), output.end()};
}

std::string base64(std::string_view bytes) {
    std::size_t needed = 0;
    mbedtls_base64_encode(nullptr, 0, &needed, reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
    std::string out(needed, '\0');
    std::size_t written = 0;
    check(mbedtls_base64_encode(reinterpret_cast<unsigned char*>(out.data()), out.size(), &written,
                                reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()),
          "base64");
    out.resize(written);
    return out;
}

std::string rsa_no_padding(std::string_view secret_key) {
    std::string reversed{secret_key.rbegin(), secret_key.rend()};
    mbedtls_mpi m, e, n, c;
    mbedtls_mpi_init(&m);
    mbedtls_mpi_init(&e);
    mbedtls_mpi_init(&n);
    mbedtls_mpi_init(&c);
    int rc = mbedtls_mpi_read_binary(&m, reinterpret_cast<const unsigned char*>(reversed.data()), reversed.size());
    if (rc == 0) {
        rc = mbedtls_mpi_read_string(&e, 16, std::string{public_exponent}.c_str());
    }
    if (rc == 0) {
        rc = mbedtls_mpi_read_string(&n, 16, std::string{modulus}.c_str());
    }
    if (rc == 0) {
        rc = mbedtls_mpi_exp_mod(&c, &m, &e, &n, nullptr);
    }
    std::array<unsigned char, 128> out{};
    if (rc == 0) {
        rc = mbedtls_mpi_write_binary(&c, out.data(), out.size());
    }
    mbedtls_mpi_free(&m);
    mbedtls_mpi_free(&e);
    mbedtls_mpi_free(&n);
    mbedtls_mpi_free(&c);
    check(rc, "RSA");

    static constexpr std::string_view digits = "0123456789abcdef";
    std::string hex;
    hex.reserve(out.size() * 2);
    for (const auto byte : out) {
        hex += digits[byte >> 4];
        hex += digits[byte & 0x0F];
    }
    return hex;
}

} // namespace

std::string random_secret_key() {
    static constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    thread_local std::mt19937_64 engine{std::random_device{}()};
    std::uniform_int_distribution<std::size_t> pick{0, alphabet.size() - 1};
    std::string key(weapi_secret_size, '\0');
    for (auto& c : key) {
        c = alphabet[pick(engine)];
    }
    return key;
}

WeapiForm weapi_encrypt(std::string_view json_text, std::string_view secret_key) {
    if (secret_key.size() != weapi_secret_size) {
        throw std::invalid_argument("weapi secret key must be 16 bytes");
    }
    const auto inner = base64(aes_cbc(json_text, preset_key));
    return {base64(aes_cbc(inner, secret_key)), rsa_no_padding(secret_key)};
}

std::string weapi_body(const WeapiForm& form) {
    using application::transport::encode_query_component;
    return "params=" + encode_query_component(form.params) + "&encSecKey=" + form.enc_sec_key;
}

} // namespace music_service::providers::netease::protocol
