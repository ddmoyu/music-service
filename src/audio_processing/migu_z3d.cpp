// 协议及参考版本见 docs/research/migu-z3d.md。
// MG3D 候选序列恢复参考 MIT 授权 Unlock Music；版权声明随发行包提供。
#include "audio_processing/migu_z3d.hpp"
#include "audio_processing/format/byte_reader.hpp"
#include "audio_processing/format/wav.hpp"
#include "audio_processing/verifying_processor.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <mbedtls/aes.h>
#include <mbedtls/md5.h>
#include <mbedtls/platform_util.h>

namespace music_service::audio_processing {
namespace {
namespace ports = application::ports;
domain::Error bad(std::string reason) {
    return {domain::ErrorCode::audio_invalid,
            "Migu Z3D decryption failed",
            false,
            {{"audio", "decrypt:z3d:" + std::move(reason)}}};
}
domain::Result<void> active(const ports::OperationContext& c) {
    if (c.stop.stop_requested())
        return domain::Error{domain::ErrorCode::canceled, "Z3D decryption canceled", false, {}};
    if (c.expired(std::chrono::steady_clock::now()))
        return domain::Error{
            domain::ErrorCode::deadline_exceeded, "Z3D decryption timed out", true, {}};
    return domain::ok();
}
struct Key {
    std::array<unsigned char, 32> bytes{};
    ~Key() { mbedtls_platform_zeroize(bytes.data(), bytes.size()); }
};
domain::Result<void> derive(std::string_view file_key, Key& key) {
    while (!file_key.empty() && (file_key.front() == ' ' || file_key.front() == '\t' ||
                                 file_key.front() == '\r' || file_key.front() == '\n'))
        file_key.remove_prefix(1);
    while (!file_key.empty() && (file_key.back() == ' ' || file_key.back() == '\t' ||
                                 file_key.back() == '\r' || file_key.back() == '\n'))
        file_key.remove_suffix(1);
    if (file_key.size() != 32 ||
        file_key.find_first_not_of("0123456789abcdefABCDEF") != file_key.npos)
        return bad("file_key");
    // 固定协议参数；逐文件密钥仍来自当前取流响应，不写入日志或持久化状态。
    constexpr unsigned char app_key[] = "308202333082019c";
    constexpr std::array<unsigned char, 32> code{0xae, 0xea, 0xc7, 0x17, 0x05, 0xa6, 0x89, 0x55,
                                                 0xa9, 0x31, 0xa6, 0xfe, 0x01, 0xe7, 0x28, 0xff,
                                                 0xf6, 0x49, 0x71, 0xe6, 0x1c, 0xea, 0xf0, 0x0a,
                                                 0xe3, 0xea, 0x43, 0xc3, 0xe0, 0x58, 0x33, 0x83};
    std::array<unsigned char, 16> iv{};
    for (unsigned char i = 0; i < iv.size(); ++i)
        iv[i] = i;
    Key decoded;
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    auto status = mbedtls_aes_setkey_dec(&aes, app_key, 128);
    if (!status)
        status = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, code.size(), iv.data(),
                                       code.data(), decoded.bytes.data());
    mbedtls_aes_free(&aes);
    if (status)
        return bad("key_derivation");
    mbedtls_md5_context md5;
    mbedtls_md5_init(&md5);
    std::array<unsigned char, 16> digest{};
    status = mbedtls_md5_starts(&md5);
    if (!status)
        status = mbedtls_md5_update(&md5, decoded.bytes.data(), decoded.bytes.size());
    if (!status)
        status = mbedtls_md5_update(&md5, reinterpret_cast<const unsigned char*>(file_key.data()),
                                    file_key.size());
    if (!status)
        status = mbedtls_md5_finish(&md5, digest.data());
    mbedtls_md5_free(&md5);
    if (status) {
        mbedtls_platform_zeroize(digest.data(), digest.size());
        return bad("key_derivation");
    }
    for (std::size_t i = 0; i < digest.size(); ++i) {
        key.bytes[2 * i] = static_cast<unsigned char>("0123456789ABCDEF"[digest[i] >> 4]);
        key.bytes[2 * i + 1] = static_cast<unsigned char>("0123456789ABCDEF"[digest[i] & 15]);
    }
    mbedtls_platform_zeroize(digest.data(), digest.size());
    return domain::ok();
}
void decode(std::string& bytes, const Key& key, std::uint64_t offset) {
    for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] =
            static_cast<char>(static_cast<unsigned char>(bytes[i]) - key.bytes[(offset + i) % 32]);
}
domain::Result<void> discover(std::string_view prefix, std::uint64_t size, Key& key,
                              const ports::OperationContext& c) {
    // 每次都从原始头部试解；失败候选不得污染下一次试解的数据。
    for (std::size_t at = 32; at + 32 <= prefix.size(); at += 32) {
        if (auto ok = active(c); !ok)
            return ok.error();
        const auto candidate = prefix.substr(at, 32);
        if (candidate.find_first_not_of("0123456789ABCDEF") != candidate.npos)
            continue;
        std::copy(candidate.begin(), candidate.end(), key.bytes.begin());
        std::string header{prefix.substr(0, 4096)};
        decode(header, key, 0);
        if (format::wav_header_matches(header, size))
            return domain::ok();
    }
    return bad("key_unavailable");
}
struct Cleanup {
    std::filesystem::path path;
    bool keep{};
    ~Cleanup() {
        if (!keep) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    }
};
} // namespace

domain::Result<void> MiguZ3dDecryptor::decrypt(const std::filesystem::path& input,
                                               const std::filesystem::path& output,
                                               const ports::OperationContext& context) {
    if (auto ok = active(context); !ok)
        return ok.error();
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(output, ec);
    if ((ec && ec != std::errc::no_such_file_or_directory) || std::filesystem::exists(status))
        return bad("output_exists");
    auto reader = format::ByteReader::open(input);
    if (!reader)
        return reader.error();
    if (reader->size() <= 0 || reader->size() > 2LL * 1024 * 1024 * 1024)
        return bad("size_limit");
    auto prefix = reader->read(0, format::ByteReader::window_bytes);
    if (!prefix)
        return prefix.error();
    const std::int64_t start = prefix->starts_with("Z3D") ? 3 : 0;
    const auto size = reader->size() - start;
    if (size < 44)
        return bad("audio_missing");
    if (start)
        prefix->erase(0, 3);
    Key key;
    if (key_) {
        if (auto result = derive(key_->reveal(), key); !result)
            return result.error();
    } else if (auto result = discover(*prefix, static_cast<std::uint64_t>(size), key, context);
               !result)
        return result.error();
    auto header = prefix->substr(0, 4096);
    decode(header, key, 0);
    if (!format::wav_header_matches(header, static_cast<std::uint64_t>(size)))
        return bad("header_or_key");
    Cleanup cleanup{output};
    std::ofstream out{output, std::ios::binary};
    if (!out)
        return bad("output_open");
    for (std::int64_t at = 0; at < size;) {
        if (auto ok = active(context); !ok)
            return ok.error();
        auto bytes = reader->read(start + at, static_cast<std::size_t>(std::min<std::int64_t>(
                                                  format::ByteReader::window_bytes, size - at)));
        if (!bytes)
            return bytes.error();
        if (bytes->empty())
            return bad("truncated");
        decode(*bytes, key, static_cast<std::uint64_t>(at));
        out.write(bytes->data(), static_cast<std::streamsize>(bytes->size()));
        if (!out)
            return bad("output_write");
        at += static_cast<std::int64_t>(bytes->size());
    }
    out.close();
    if (!out)
        return bad("output_flush");
    auto plain = format::ByteReader::open(output);
    if (!plain)
        return plain.error();
    auto wav = format::scan_wav(*plain, context);
    if (!wav)
        return wav.error();
    cleanup.keep = true;
    return domain::ok();
}

ports::ProcessingPlan MiguZ3dProcessor::plan() const { return {"audio.migu-z3d", 1, "migu.z3d"}; }
domain::Result<ports::ProcessingReport>
MiguZ3dProcessor::process(const ports::ProcessingRequest& request,
                          const ports::OperationContext& context) {
    if (auto ok = active(context); !ok)
        return ok.error();
    if (request.expected.size_bytes) {
        auto encrypted = format::ByteReader::open(request.input);
        if (!encrypted)
            return encrypted.error();
        if (encrypted->size() != *request.expected.size_bytes)
            return domain::Error{domain::ErrorCode::audio_invalid,
                                 "Z3D transfer size mismatch",
                                 true,
                                 {{"audio", "size_mismatch"}}};
    }
    const auto output = request.input.parent_path() / "migu-z3d-decoded.part";
    const auto tagged = request.input.parent_path() / "tagged.wav";
    // 两个明文副本都属于当前工作目录；拒绝链接和输入别名后才清理重试产物。
    const auto prepare = [&](const std::filesystem::path& path) -> domain::Result<void> {
        if (path == request.input)
            return bad("working_path_collision");
        std::error_code ec;
        const auto status = std::filesystem::symlink_status(path, ec);
        if (ec && ec != std::errc::no_such_file_or_directory)
            return bad("working_path_status");
        ec.clear();
        if (std::filesystem::exists(status)) {
            if (!std::filesystem::is_regular_file(status) ||
                std::filesystem::equivalent(path, request.input, ec) || ec)
                return bad("working_path_collision");
            std::filesystem::remove(path, ec);
            if (ec)
                return bad("working_path_remove");
        }
        return domain::ok();
    };
    if (auto ready = prepare(output); !ready)
        return ready.error();
    if (auto ready = prepare(tagged); !ready)
        return ready.error();
    Cleanup cleanup{output};
    Cleanup tagged_cleanup{tagged};
    auto decrypted =
        MiguZ3dDecryptor{request.decryption_key}.decrypt(request.input, output, context);
    if (!decrypted)
        return decrypted.error();
    auto plain = request;
    plain.input = output;
    plain.decryption_key.reset();
    plain.expected.size_bytes.reset();
    auto report = VerifyingProcessor{}.process(plain, context);
    if (!report)
        return report.error();
    report->processor_id = plan().processor_id;
    report->processor_version = plan().processor_version;
    for (auto& stage : report->stages)
        if (stage.stage == "decrypt")
            stage.outcome = ports::StageOutcome::done;
    cleanup.keep = true;
    tagged_cleanup.keep = report->output == tagged;
    return report;
}
} // namespace music_service::audio_processing
