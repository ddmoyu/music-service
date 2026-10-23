#include "audio_processing/format/wav.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <mbedtls/sha256.h>

namespace music_service::audio_processing::format {
namespace {
namespace ports = application::ports;
domain::Error bad(std::string reason) {
    return {domain::ErrorCode::audio_invalid,
            "invalid PCM WAV structure",
            false,
            {{"audio", "wav:" + std::move(reason)}}};
}
domain::Result<void> active(const ports::OperationContext& c) {
    if (c.stop.stop_requested())
        return domain::Error{domain::ErrorCode::canceled, "WAV validation canceled", false, {}};
    if (c.expired(std::chrono::steady_clock::now()))
        return domain::Error{
            domain::ErrorCode::deadline_exceeded, "WAV validation timed out", true, {}};
    return domain::ok();
}
std::uint32_t le(std::string_view b, std::size_t pos, std::size_t count) {
    std::uint32_t v{};
    for (std::size_t i = 0; i < count; ++i)
        v |= static_cast<std::uint32_t>(static_cast<unsigned char>(b[pos + i])) << (8 * i);
    return v;
}
struct Format {
    std::uint32_t rate{}, channels{}, bits{}, valid_bits{}, align{}, byte_rate{};
};
std::optional<Format> parse_format(std::string_view b) {
    if (b.size() < 16 || b.size() > 4096)
        return {};
    const auto type = le(b, 0, 2);
    Format f{le(b, 4, 4), le(b, 2, 2), le(b, 14, 2), le(b, 14, 2), le(b, 12, 2), le(b, 8, 4)};
    if (type == 1) {
        if (b.size() != 16 && (b.size() != 18 || le(b, 16, 2) != 0))
            return {};
    } else if (type == 0xfffe) {
        if (b.size() < 40 || le(b, 16, 2) < 22 || le(b, 16, 2) + 18 != b.size())
            return {};
        constexpr std::array<unsigned char, 16> pcm{1,    0, 0, 0,    0, 0,    0x10, 0,
                                                    0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};
        for (std::size_t i = 0; i < pcm.size(); ++i)
            if (static_cast<unsigned char>(b[24 + i]) != pcm[i])
                return {};
        f.valid_bits = le(b, 18, 2);
        const auto mask = le(b, 20, 4);
        if (mask && static_cast<std::uint32_t>(std::popcount(mask)) != f.channels)
            return {};
    } else
        return {};
    if (!f.rate || f.rate > 768000 || !f.channels || f.channels > 32 ||
        (f.bits != 8 && f.bits != 16 && f.bits != 24 && f.bits != 32) || !f.valid_bits ||
        f.valid_bits > f.bits || f.align != f.channels * (f.bits / 8) ||
        f.byte_rate != f.rate * f.align)
        return {};
    return f;
}
bool riff(std::string_view b, std::uint64_t size) {
    return size >= 44 && b.size() >= 12 && b.substr(0, 4) == "RIFF" && b.substr(8, 4) == "WAVE" &&
           static_cast<std::uint64_t>(le(b, 4, 4)) + 8 == size;
}
bool fourcc(std::string_view b) {
    return std::all_of(b.begin(), b.end(), [](unsigned char c) { return c >= 32 && c <= 126; });
}
} // namespace

bool wav_header_matches(std::string_view prefix, std::uint64_t file_size) {
    if (!riff(prefix, file_size))
        return false;
    for (std::size_t at = 12, chunks = 0; at + 8 <= prefix.size() && chunks < 128; ++chunks) {
        const auto size = le(prefix, at + 4, 4);
        if (at + 8 > file_size || !fourcc(prefix.substr(at, 4)) || size > file_size - at - 8)
            return false;
        const auto next = static_cast<std::uint64_t>(at) + 8 + size + (size & 1U);
        if (next > prefix.size())
            return false;
        if (prefix.substr(at, 4) == "fmt ")
            return parse_format(prefix.substr(at + 8, size)).has_value();
        at = static_cast<std::size_t>(next);
    }
    return false;
}

domain::Result<WavInfo> scan_wav(ByteReader& reader, const ports::OperationContext& context) {
    auto head = reader.read(0, 12);
    if (!head)
        return head.error();
    if (!riff(*head, static_cast<std::uint64_t>(reader.size())))
        return bad("riff_size");
    std::optional<Format> format;
    std::int64_t data_offset{}, data_size{};
    std::size_t chunks{};
    for (std::int64_t at = 12; at < reader.size();) {
        if (auto ok = active(context); !ok)
            return ok.error();
        if (++chunks > 16384 || reader.size() - at < 8)
            return bad("chunk_header");
        auto h = reader.read(at, 8);
        if (!h)
            return h.error();
        if (h->size() != 8 || !fourcc(h->substr(0, 4)))
            return bad("chunk_id");
        const auto size = static_cast<std::int64_t>(le(*h, 4, 4));
        const auto padded = size + (size & 1);
        if (padded > reader.size() - at - 8)
            return bad("chunk_size");
        if (h->substr(0, 4) == "fmt ") {
            if (format || size > 4096)
                return bad("format_duplicate_or_limit");
            auto bytes = reader.read(at + 8, static_cast<std::size_t>(size));
            if (!bytes)
                return bytes.error();
            format = parse_format(*bytes);
            if (!format)
                return bad("unsupported_format");
        } else if (h->substr(0, 4) == "data") {
            if (!format || data_offset || !size || size % format->align)
                return bad("data_layout");
            data_offset = at + 8;
            data_size = size;
        }
        at += 8 + padded;
    }
    if (!format || !data_offset)
        return bad("missing_audio");
    struct Hash {
        mbedtls_sha256_context ctx;
        Hash() { mbedtls_sha256_init(&ctx); }
        ~Hash() { mbedtls_sha256_free(&ctx); }
    } hash;
    if (mbedtls_sha256_starts(&hash.ctx, 0) != 0)
        return bad("hash");
    for (std::int64_t at = 0; at < data_size;) {
        if (auto ok = active(context); !ok)
            return ok.error();
        auto b = reader.read(data_offset + at, static_cast<std::size_t>(std::min<std::int64_t>(
                                                   ByteReader::window_bytes, data_size - at)));
        if (!b)
            return b.error();
        if (b->empty() ||
            mbedtls_sha256_update(&hash.ctx, reinterpret_cast<const unsigned char*>(b->data()),
                                  b->size()) != 0)
            return bad("hash");
        at += static_cast<std::int64_t>(b->size());
    }
    std::array<unsigned char, 32> digest{};
    if (mbedtls_sha256_finish(&hash.ctx, digest.data()) != 0)
        return bad("hash");
    WavInfo result;
    result.sample_rate_hz = static_cast<std::int32_t>(format->rate);
    result.channels = static_cast<std::int32_t>(format->channels);
    result.bits_per_sample = static_cast<std::int32_t>(format->valid_bits);
    result.bitrate_bps = static_cast<std::int64_t>(format->byte_rate) * 8;
    result.audio_bytes = data_size;
    result.samples = static_cast<std::uint64_t>(data_size) / format->align;
    result.duration_ms = static_cast<std::int64_t>(result.samples * 1000 / format->rate);
    result.codec = format->bits == 8 ? "pcm_u8" : "pcm_s" + std::to_string(format->bits) + "le";
    for (auto byte : digest) {
        result.payload_sha256 += "0123456789abcdef"[byte >> 4];
        result.payload_sha256 += "0123456789abcdef"[byte & 15];
    }
    return result;
}
} // namespace music_service::audio_processing::format
