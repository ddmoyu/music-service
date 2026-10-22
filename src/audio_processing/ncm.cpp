#include "audio_processing/ncm.hpp"
#include "audio_processing/format/byte_reader.hpp"
#include "audio_processing/verifying_processor.hpp"
#include "domain/assets/container.hpp"

#include <mbedtls/aes.h>
#include <mbedtls/base64.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <fstream>

namespace music_service::audio_processing {
namespace ports = application::ports;
namespace {
domain::Error bad(std::string reason) {
    return {domain::ErrorCode::audio_invalid, "invalid NCM file", false, {{"audio", "decrypt:" + reason}}};
}
domain::Result<void> active(const ports::OperationContext& context) {
    if (context.stop.stop_requested()) return domain::Error{domain::ErrorCode::canceled, "NCM import canceled", false, {}};
    if (context.expired(std::chrono::steady_clock::now()))
        return domain::Error{domain::ErrorCode::deadline_exceeded, "NCM import deadline exceeded", false, {}};
    return domain::ok();
}
domain::Result<std::string> aes(std::string data, std::string_view key) {
    if (data.empty() || data.size() % 16 != 0) return bad("aes_length");
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    int rc = mbedtls_aes_setkey_dec(&ctx, reinterpret_cast<const unsigned char*>(key.data()), 128);
    for (std::size_t at = 0; rc == 0 && at < data.size(); at += 16) {
        auto* block = reinterpret_cast<unsigned char*>(data.data() + at);
        rc = mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_DECRYPT, block, block);
    }
    mbedtls_aes_free(&ctx);
    if (rc != 0) return bad("aes");
    const auto pad = static_cast<unsigned char>(data.back());
    if (pad == 0 || pad > 16 || !std::all_of(data.end() - pad, data.end(),
            [pad](char c) { return static_cast<unsigned char>(c) == pad; })) return bad("padding");
    data.resize(data.size() - pad);
    return data;
}
struct Cursor {
    format::ByteReader& reader;
    std::int64_t offset{0};
    domain::Result<std::string> take(std::uint64_t count, std::uint64_t limit) {
        if (count > limit || count > static_cast<std::uint64_t>(reader.size() - offset)) return bad("length");
        // ByteReader is windowed; accumulate only the explicitly bounded header blocks.
        std::string out;
        while (count) {
            auto part = reader.read(offset, static_cast<std::size_t>(std::min<std::uint64_t>(count, 65536)));
            if (!part || part->empty()) return bad("truncated");
            offset += static_cast<std::int64_t>(part->size());
            count -= part->size();
            out += *part;
        }
        return out;
    }
    domain::Result<std::uint32_t> u32() {
        auto bytes = take(4, 4);
        if (!bytes) return bytes.error();
        std::uint32_t n = 0;
        for (unsigned i = 0; i < 4; ++i) n |= static_cast<std::uint32_t>(static_cast<unsigned char>((*bytes)[i])) << (8 * i);
        return n;
    }
};
}

domain::Result<ports::TrackMetadata> NcmDecryptor::decode(const std::filesystem::path& input,
    const std::filesystem::path& output, const ports::OperationContext& context, std::uint64_t max_bytes) {
    if (auto check = active(context); !check) return check.error();
    std::error_code ec;
    if (std::filesystem::exists(output, ec) || ec) return bad("output_exists");
    auto reader = format::ByteReader::open(input);
    if (!reader) return reader.error();
    if (reader->size() <= 0 || static_cast<std::uint64_t>(reader->size()) > max_bytes) return bad("size_limit");
    Cursor cursor{*reader};
    auto magic = cursor.take(10, 10);
    if (!magic || magic->substr(0, 8) != "CTENFDAM") return bad("header");
    auto key_len = cursor.u32();
    if (!key_len) return key_len.error();
    auto key_blob = cursor.take(*key_len, 4096);
    if (!key_blob) return key_blob.error();
    for (auto& c : *key_blob) c ^= 0x64;
    auto key = aes(std::move(*key_blob), "hzHRAmso5kInbaxW");
    if (!key || key->size() <= 17 || key->substr(0, 17) != "neteasecloudmusic") return bad("key");
    key->erase(0, 17);
    std::array<unsigned char, 256> box{};
    for (unsigned i = 0; i < 256; ++i) box[i] = static_cast<unsigned char>(i);
    unsigned last = 0;
    for (unsigned i = 0; i < 256; ++i) {
        last = (static_cast<unsigned>(box[i]) + last + static_cast<unsigned char>((*key)[i % key->size()])) & 255U;
        std::swap(box[i], box[last]);
    }
    ports::TrackMetadata metadata;
    auto meta_len = cursor.u32();
    if (!meta_len) return meta_len.error();
    auto meta_blob = cursor.take(*meta_len, 1024 * 1024);
    if (!meta_blob) return meta_blob.error();
    if (!meta_blob->empty()) {
        for (auto& c : *meta_blob) c ^= 0x63;
        constexpr std::string_view prefix = "163 key(Don't modify):";
        if (!meta_blob->starts_with(prefix)) return bad("metadata_prefix");
        const auto encoded = std::string_view{*meta_blob}.substr(prefix.size());
        std::string decoded(encoded.size(), '\0');
        std::size_t length = 0;
        if (mbedtls_base64_decode(reinterpret_cast<unsigned char*>(decoded.data()), decoded.size(), &length,
            reinterpret_cast<const unsigned char*>(encoded.data()), encoded.size()) != 0) return bad("metadata_base64");
        decoded.resize(length);
        auto clear = aes(std::move(decoded), "#14ljk_!\\]&0U<'(");
        if (!clear) return clear.error();
        const bool dj = clear->starts_with("dj:");
        if (!dj && !clear->starts_with("music:")) return bad("metadata_prefix");
        bool too_deep = false;
        auto object = nlohmann::json::parse(clear->substr(dj ? 3 : 6),
            [&](int depth, nlohmann::json::parse_event_t, nlohmann::json&) {
                if (depth > 32) too_deep = true;
                return !too_deep;
            }, false);
        if (too_deep) return bad("metadata_depth");
        if (dj && object.is_object() && object.contains("mainMusic")) object = object["mainMusic"];
        if (!object.is_object()) return bad("metadata_json");
        const auto text = [&](const char* field) -> std::optional<std::string> {
            const auto it = object.find(field);
            if (it == object.end() || !it->is_string() || it->get_ref<const std::string&>().size() > 65536) return {};
            return it->get<std::string>();
        };
        metadata.title = text("musicName").value_or("");
        metadata.album = text("album");
        metadata.lyrics = text("lyric");
        if (const auto it = object.find("artist"); it != object.end() && it->is_array()) {
            for (const auto& artist : *it) {
                if (metadata.artists.size() >= 100) return bad("artists_limit");
                if (artist.is_array() && !artist.empty() && artist[0].is_string())
                    metadata.artists.push_back(artist[0].get<std::string>());
            }
        }
    }
    if (!cursor.take(5, 5)) return bad("image_header"); // CRC32 (unspecified) and image version
    const auto frame = cursor.u32();
    const auto image_size = cursor.u32();
    if (!frame || !image_size || *frame < *image_size || *frame > 32 * 1024 * 1024) return bad("image_length");
    auto picture = cursor.take(*image_size, 8 * 1024 * 1024);
    if (!picture) return picture.error();
    if (!picture->empty()) {
        std::string mime;
        if (picture->starts_with("\x89PNG\r\n\x1a\n")) mime = "image/png";
        else if (picture->size() >= 4 && static_cast<unsigned char>((*picture)[0]) == 0xff &&
                 static_cast<unsigned char>((*picture)[1]) == 0xd8) mime = "image/jpeg";
        else return bad("image_format");
        metadata.cover = ports::Picture{std::move(mime), std::move(*picture)};
    }
    // NCM 3.x reserves a cover frame larger than its image. The stream starts AFTER padding.
    const auto padding = *frame - *image_size;
    if (padding >= static_cast<std::uint64_t>(reader->size() - cursor.offset)) return bad("audio_missing");
    cursor.offset += padding;
    struct Cleanup {
        std::filesystem::path path;
        bool keep{false};
        ~Cleanup() { if (!keep) { std::error_code ignored; std::filesystem::remove(path, ignored); } }
    } cleanup{output};
    std::ofstream out{output, std::ios::binary};
    if (!out) return bad("output_open");
    std::uint64_t position = 0;
    while (cursor.offset < reader->size()) {
        if (auto check = active(context); !check) return check.error();
        auto chunk = cursor.take(static_cast<std::uint64_t>(std::min<std::int64_t>(65536, reader->size() - cursor.offset)), 65536);
        if (!chunk) return chunk.error();
        for (auto& c : *chunk) {
            const auto j = static_cast<unsigned>((++position) & 255U);
            c ^= static_cast<char>(box[(static_cast<unsigned>(box[j]) + box[(static_cast<unsigned>(box[j]) + j) & 255U]) & 255U]);
        }
        if (position == chunk->size()) {
            const auto sniffed = domain::sniff_content(*chunk);
            if (sniffed.container != domain::Container::mp3 && sniffed.container != domain::Container::flac)
                return bad("audio_container");
        }
        out.write(chunk->data(), static_cast<std::streamsize>(chunk->size()));
        if (!out) return bad("output_write");
    }
    out.close();
    if (!out) return bad("output_flush");
    if (auto check = active(context); !check) return check.error();
    cleanup.keep = true;
    return metadata;
}

domain::Result<void> NcmDecryptor::decrypt(const std::filesystem::path& input, const std::filesystem::path& output,
                                          const ports::OperationContext& context) {
    auto decoded = decode(input, output, context);
    if (!decoded) return decoded.error();
    return domain::ok();
}
ports::ProcessingPlan NcmProcessor::plan() const { return {"audio.ncm", 1, "netease.ncm"}; }
domain::Result<ports::ProcessingReport> NcmProcessor::process(const ports::ProcessingRequest& request,
                                                            const ports::OperationContext& context) {
    const auto output = request.input.parent_path() / "ncm-decoded.part";
    auto metadata = NcmDecryptor{}.decode(request.input, output, context);
    if (!metadata) return metadata.error();
    auto plain = request;
    plain.input = output;
    plain.expected.size_bytes.reset(); // encrypted size is not audio size
    if (!plain.metadata) plain.metadata = *metadata;
    if (!request.embed_cover) plain.metadata->cover.reset();
    if (!request.embed_lyrics) plain.metadata->lyrics.reset();
    auto report = VerifyingProcessor{}.process(plain, context);
    if (!report) { std::error_code ec; std::filesystem::remove(output, ec); return report.error(); }
    report->processor_id = plan().processor_id;
    report->processor_version = plan().processor_version;
    for (auto& stage : report->stages) if (stage.stage == "decrypt") stage.outcome = ports::StageOutcome::done;
    report->source_metadata = std::move(*metadata);
    return report;
}
} // namespace music_service::audio_processing
