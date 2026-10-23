#pragma once

#include "audio_processing/format/flac.hpp"

namespace music_service::audio_processing::format {

struct FlacFrames {
    std::int64_t audio_end{0}; // 最后一帧 CRC16 之后的准确字节位置，不包含附加尾部。
    std::uint64_t frames{0};
    std::array<unsigned char, 16> pcm_md5{};
};

// 按 RFC 9639 原生解码所有子帧，只保留一个块的 PCM；验证序号、CRC、样本总数及 MD5。
// 需要已知总样本数和非零 STREAMINFO MD5，证据缺失时拒绝修剪，不搜索下一处同步字。
domain::Result<FlacFrames> verify_flac_frames(ByteReader& reader, const FlacStructure& info,
    const application::ports::OperationContext& context);

} // namespace music_service::audio_processing::format
