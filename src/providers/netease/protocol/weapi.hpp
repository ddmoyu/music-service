#pragma once

#include <string>
#include <string_view>

// The web client's request encryption ("weapi"), implemented natively with Mbed TLS:
//   params    = base64(AES-128-CBC(base64(AES-128-CBC(text, preset_key)), secret_key))
//   encSecKey = hex(reverse(secret_key) ^ 65537 mod N), textbook RSA without padding
// Both AES layers use PKCS#7 padding and the fixed IV "0102030405060708". The secret
// key is 16 characters the client picks per request.
namespace music_service::providers::netease::protocol {

inline constexpr std::size_t weapi_secret_size = 16;

// A fresh secret key: 16 characters from [A-Za-z0-9]. It only hides the payload from
// casual readers; it protects nothing of the user's.
[[nodiscard]] std::string random_secret_key();

struct WeapiForm {
    std::string params;
    std::string enc_sec_key; // 256 lowercase hex digits
};

// Encrypts the JSON text of a request with the given 16-byte secret key.
[[nodiscard]] WeapiForm weapi_encrypt(std::string_view json_text, std::string_view secret_key);

// The x-www-form-urlencoded body: params=...&encSecKey=...
[[nodiscard]] std::string weapi_body(const WeapiForm& form);

} // namespace music_service::providers::netease::protocol
