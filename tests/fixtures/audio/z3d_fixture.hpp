#pragma once

// Synthetic PCM only. The independent key pair is the published music-lib test vector;
// the fixture does not call our key derivation or decryption implementation.
#include <cstdint>
#include <string>
#include <string_view>

namespace music_service::test::z3d {
inline constexpr std::string_view file_key = "9A14CB2B6AA5DA30CE036BBD6664373E";
inline constexpr std::string_view stream_key = "397BE2C84E0F983C65ED04FC407C8F99";
inline void put(std::string& b, std::size_t at, std::uint32_t value, unsigned count = 4) {
    for (unsigned i = 0; i < count; ++i)
        b[at + i] = static_cast<char>(value >> (8 * i));
}
inline std::string chunk(std::string_view id, const std::string& data) {
    std::string b{id};
    b.resize(8);
    put(b, 4, static_cast<std::uint32_t>(data.size()));
    b += data;
    if (data.size() & 1)
        b += '\0';
    return b;
}
inline std::string riff(std::string chunks) {
    std::string b = "RIFF0000WAVE" + std::move(chunks);
    put(b, 4, static_cast<std::uint32_t>(b.size() - 8));
    return b;
}
inline std::string wav(unsigned frames = 16000, unsigned fmt_size = 16) {
    std::string fmt(fmt_size, '\0');
    put(fmt, 0, fmt_size == 40 ? 0xfffe : 1, 2);
    put(fmt, 2, 2, 2);
    put(fmt, 4, 8000);
    put(fmt, 8, 32000);
    put(fmt, 12, 4, 2);
    put(fmt, 14, 16, 2);
    if (fmt_size == 40) {
        put(fmt, 16, 22, 2);
        put(fmt, 18, 16, 2);
        put(fmt, 20, 3);
        constexpr unsigned char guid[]{1,    0, 0, 0,    0, 0,    0x10, 0,
                                       0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};
        for (unsigned i = 0; i < 16; ++i)
            fmt[24 + i] = static_cast<char>(guid[i]);
    }
    std::string pcm(static_cast<std::size_t>(frames) * 4, '\0');
    for (std::size_t i = 256; i < pcm.size(); ++i)
        pcm[i] = static_cast<char>((i * 29 + 17) & 255);
    return riff(chunk("fmt ", fmt) + chunk("data", pcm));
}
inline std::string encrypt(std::string plain, bool marker = false) {
    for (std::size_t i = 0; i < plain.size(); ++i)
        plain[i] = static_cast<char>(static_cast<unsigned char>(plain[i]) +
                                     static_cast<unsigned char>(stream_key[i % stream_key.size()]));
    return (marker ? "Z3D" : "") + plain;
}
} // namespace music_service::test::z3d
