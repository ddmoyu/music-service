#pragma once

#include "application/ports/runtime/operation_context.hpp"
#include "audio_processing/format/byte_reader.hpp"
#include <optional>

namespace music_service::audio_processing::format {

struct WavInfo {
    std::int32_t sample_rate_hz{};
    std::int32_t channels{};
    std::int32_t bits_per_sample{};
    std::int64_t bitrate_bps{};
    std::int64_t duration_ms{};
    std::int64_t audio_bytes{};
    std::uint64_t samples{};
    std::string codec;
    std::string payload_sha256;
};

// 只接受有完整 RIFF 边界和 PCM 格式证据的 WAV；未知压缩格式、RF64 不猜测。
domain::Result<WavInfo> scan_wav(ByteReader& reader,
                                 const application::ports::OperationContext& context);
// 用于筛选 MG3D 的候选序列，最终输出仍须经过 scan_wav 的完整文件检查。
bool wav_header_matches(std::string_view prefix, std::uint64_t file_size);

} // namespace music_service::audio_processing::format
