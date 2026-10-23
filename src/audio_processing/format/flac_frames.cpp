#include "audio_processing/format/flac_frames.hpp"

#include <mbedtls/md5.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace music_service::audio_processing::format {
namespace {
using domain::Error;
using domain::ErrorCode;
using application::ports::OperationContext;

// 内部解析失败以异常展开到唯一的 Result 边界，避免每次读取若干位都分配错误对象。
struct Failure { Error error; };
[[noreturn]] void bad(const char* reason) {
    throw Failure{{ErrorCode::audio_invalid, "the FLAC audio failed full verification", false, {{"audio", reason}}}};
}
void check_context(const OperationContext& context) {
    if (context.stop.stop_requested())
        throw Failure{{ErrorCode::canceled, "FLAC verification was canceled", false, {}}};
    if (context.expired(std::chrono::steady_clock::now()))
        throw Failure{{ErrorCode::deadline_exceeded, "FLAC verification exceeded its deadline", true, {}}};
}
constexpr auto crc16_table() {
    std::array<std::uint16_t, 256> table{};
    for (unsigned i = 0; i < 256; ++i) {
        unsigned v = i << 8;
        for (int j = 0; j < 8; ++j) v = ((v << 1) ^ ((v & 0x8000) ? 0x8005U : 0U)) & 0xffff;
        table[i] = static_cast<std::uint16_t>(v);
    }
    return table;
}
constexpr auto crc8_table() {
    std::array<unsigned char, 256> table{};
    for (unsigned i = 0; i < 256; ++i) {
        unsigned v = i;
        for (int j = 0; j < 8; ++j) v = ((v << 1) ^ ((v & 0x80) ? 7U : 0U)) & 0xff;
        table[i] = static_cast<unsigned char>(v);
    }
    return table;
}
constexpr auto crc16 = crc16_table();
constexpr auto crc8 = crc8_table();

class Bits {
public:
    Bits(ByteReader& reader, std::int64_t offset, const OperationContext& context)
        : reader_(reader), context_(context), offset_(offset) {}
    void frame() {
        if (left_ != 0) bad("unaligned_frame");
        crc16_ = 0; crc8_ = 0;
    }
    std::uint64_t get(unsigned count) {
        if (count > 36) bad("invalid_bit_width");
        std::uint64_t value = 0;
        while (count > 0) {
            if (left_ == 0) {
                if (index_ == window_.size()) {
                    check_context(context_);
                    auto next = reader_.read(offset_, ByteReader::window_bytes);
                    if (!next) throw Failure{std::move(next).error()};
                    window_ = std::move(*next); index_ = 0;
                    if (window_.empty()) bad("truncated_frame");
                }
                current_ = static_cast<unsigned char>(window_[index_++]);
                ++offset_;
                crc16_ = static_cast<std::uint16_t>((crc16_ << 8) ^ crc16[(crc16_ >> 8) ^ current_]);
                crc8_ = crc8[crc8_ ^ current_];
                left_ = 8;
            }
            const unsigned take = std::min(count, left_);
            value = (value << take) | ((current_ >> (left_ - take)) & ((1U << take) - 1));
            left_ -= take; count -= take;
        }
        return value;
    }
    std::int64_t signed_value(unsigned count) {
        if (count == 0 || count > 33) bad("invalid_bit_width");
        const auto value = get(count);
        return (value & (std::uint64_t{1} << (count - 1)))
            ? static_cast<std::int64_t>(value) - (std::int64_t{1} << count) : static_cast<std::int64_t>(value);
    }
    std::uint64_t unary(std::uint64_t limit) {
        std::uint64_t value = 0;
        while (get(1) == 0) if (++value > limit) bad("residual_limit");
        return value;
    }
    void padding() { if (left_ && get(left_) != 0) bad("nonzero_padding"); }
    void header_crc() { (void)get(8); if (crc8_ != 0) bad("header_crc"); }
    void frame_crc() { (void)get(16); if (crc16_ != 0) bad("frame_crc"); }
    std::int64_t offset() const { return offset_; }
private:
    ByteReader& reader_;
    const OperationContext& context_;
    std::int64_t offset_;
    std::string window_;
    std::size_t index_{0};
    unsigned current_{0}, left_{0};
    std::uint16_t crc16_{0};
    unsigned char crc8_{0};
};

std::uint64_t number(Bits& bits, bool variable) {
    const auto first = static_cast<unsigned>(bits.get(8));
    if (first < 128) return first;
    unsigned length = 0;
    for (unsigned mask = 128; first & mask; mask >>= 1) { ++length; if (mask == 1) break; }
    if (length < 2 || length > (variable ? 7U : 6U)) bad("frame_number");
    std::uint64_t value = first & ((1U << (7 - length)) - 1);
    for (unsigned i = 1; i < length; ++i) {
        const auto next = bits.get(8);
        if ((next & 0xc0) != 0x80) bad("frame_number");
        value = (value << 6) | (next & 0x3f);
    }
    constexpr std::array<std::uint64_t, 8> minimum{0, 0, 0x80, 0x800, 0x10000, 0x200000, 0x4000000, 0x80000000};
    if (value < minimum[length]) bad("overlong_frame_number");
    return value;
}

struct Header { unsigned samples, assignment; bool variable; std::uint64_t number; };
Header header(Bits& bits, const FlacStructure& info) {
    bits.frame();
    if (bits.get(15) != 0x7ffc) bad("frame_sync");
    const bool variable = bits.get(1) != 0;
    const auto block_code = static_cast<unsigned>(bits.get(4));
    const auto rate_code = static_cast<unsigned>(bits.get(4));
    const auto assignment = static_cast<unsigned>(bits.get(4));
    const auto depth_code = static_cast<unsigned>(bits.get(3));
    if (bits.get(1) != 0 || block_code == 0 || rate_code == 15 || assignment > 10 || depth_code == 3)
        bad("reserved_frame_header");
    const auto coded_number = number(bits, variable);
    unsigned samples = 0;
    if (block_code == 1) samples = 192;
    else if (block_code <= 5) samples = 144U << block_code;
    else if (block_code <= 7) samples = static_cast<unsigned>(bits.get(block_code == 6 ? 8 : 16)) + 1;
    else samples = 1U << block_code;
    constexpr std::array<unsigned, 12> rates{0, 88200, 176400, 192000, 8000, 16000, 22050, 24000, 32000, 44100, 48000, 96000};
    unsigned rate = static_cast<unsigned>(info.sample_rate_hz);
    if (rate_code && rate_code < 12) rate = rates[rate_code];
    else if (rate_code == 12) rate = static_cast<unsigned>(bits.get(8)) * 1000;
    else if (rate_code >= 13) rate = static_cast<unsigned>(bits.get(16)) * (rate_code == 14 ? 10U : 1U);
    constexpr std::array<unsigned, 8> depths{0, 8, 12, 0, 16, 20, 24, 32};
    const unsigned depth = depth_code ? depths[depth_code] : static_cast<unsigned>(info.bits_per_sample);
    const unsigned channels = assignment <= 7 ? assignment + 1 : 2;
    if (rate != static_cast<unsigned>(info.sample_rate_hz) || depth != static_cast<unsigned>(info.bits_per_sample) ||
        channels != static_cast<unsigned>(info.channels) || samples > info.max_block_samples || samples > 65535)
        bad("frame_properties_mismatch");
    bits.header_crc();
    return {samples, assignment, variable, coded_number};
}

void range(std::int64_t value, unsigned depth) {
    const auto limit = std::int64_t{1} << (depth - 1);
    if (value < -limit || value >= limit) bad("sample_out_of_range");
}

void residual(Bits& bits, std::vector<std::int64_t>& samples, unsigned order) {
    const auto method = bits.get(2);
    if (method > 1) bad("residual_method");
    const unsigned parameter_bits = method == 0 ? 4 : 5;
    const auto partitions = 1U << bits.get(4);
    if (samples.size() % partitions || samples.size() / partitions <= order) bad("residual_partition");
    const auto partition_samples = samples.size() / partitions;
    for (unsigned partition = 0; partition < partitions; ++partition) {
        const auto parameter = static_cast<unsigned>(bits.get(parameter_bits));
        const bool escape = parameter == (1U << parameter_bits) - 1;
        const auto width = escape ? static_cast<unsigned>(bits.get(5)) : 0U;
        const auto start = partition * partition_samples + (partition == 0 ? order : 0);
        const auto end = (partition + 1) * partition_samples;
        for (auto i = start; i < end; ++i) {
            if (escape) samples[i] = width == 0 ? 0 : bits.signed_value(width);
            else {
                // FLAC 残差限定到有符号 32 位（不含 INT32_MIN）；一元码还受读取截止时间约束。
                const auto quotient = bits.unary(0xfffffffeULL >> parameter);
                const auto folded = (quotient << parameter) | bits.get(parameter);
                if (folded > 0xfffffffeULL) bad("residual_overflow");
                samples[i] = (folded & 1) ? -static_cast<std::int64_t>((folded >> 1) + 1)
                                         : static_cast<std::int64_t>(folded >> 1);
            }
        }
    }
}

void subframe(Bits& bits, std::vector<std::int64_t>& samples, unsigned depth) {
    if (bits.get(1) != 0) bad("subframe_padding");
    const auto type = static_cast<unsigned>(bits.get(6));
    const bool wasted_flag = bits.get(1) != 0;
    const auto wasted = wasted_flag ? static_cast<unsigned>(bits.unary(depth - 1) + 1) : 0U;
    if (wasted >= depth) bad("wasted_bits");
    depth -= wasted;
    if (type == 0) std::fill(samples.begin(), samples.end(), bits.signed_value(depth));
    else if (type == 1) for (auto& sample : samples) sample = bits.signed_value(depth);
    else {
        const bool fixed = type >= 8 && type <= 12;
        if (!fixed && type < 32) bad("reserved_subframe");
        const auto order = fixed ? type - 8 : type - 31;
        if (order >= samples.size()) bad("predictor_order");
        for (unsigned i = 0; i < order; ++i) samples[i] = bits.signed_value(depth);
        std::array<std::int64_t, 32> coefficients{};
        unsigned shift = 0;
        if (fixed) {
            constexpr std::int64_t fixed_coefficients[5][4]{{0,0,0,0}, {1,0,0,0}, {2,-1,0,0}, {3,-3,1,0}, {4,-6,4,-1}};
            for (unsigned i = 0; i < order; ++i) coefficients[i] = fixed_coefficients[order][i];
        } else {
            const auto precision = static_cast<unsigned>(bits.get(4)) + 1;
            if (precision == 16) bad("lpc_precision");
            const auto signed_shift = bits.signed_value(5);
            if (signed_shift < 0) bad("negative_lpc_shift");
            shift = static_cast<unsigned>(signed_shift);
            for (unsigned i = 0; i < order; ++i) coefficients[i] = bits.signed_value(precision);
        }
        residual(bits, samples, order);
        for (std::size_t i = order; i < samples.size(); ++i) {
            // 已还原样本不超过 33 位，系数最多 15 位、32 项，累加在 int64 中有界。
            std::int64_t prediction = 0;
            for (unsigned j = 0; j < order; ++j) prediction += coefficients[j] * samples[i - j - 1];
            samples[i] += prediction >> shift;
            range(samples[i], depth);
        }
    }
    for (auto& sample : samples) { range(sample, depth); sample *= std::int64_t{1} << wasted; }
}

struct Md5 {
    mbedtls_md5_context value;
    Md5() { mbedtls_md5_init(&value); mbedtls_md5_starts(&value); }
    ~Md5() { mbedtls_md5_free(&value); }
    Md5(const Md5&) = delete;
    Md5& operator=(const Md5&) = delete;
};
} // namespace

domain::Result<FlacFrames> verify_flac_frames(ByteReader& reader, const FlacStructure& info,
    const OperationContext& context) {
    try {
        check_context(context);
        if (!info.total_samples || !*info.total_samples || info.min_block_samples < 16 ||
            info.max_block_samples < info.min_block_samples || info.max_block_samples > 65535 ||
            info.bits_per_sample < 4 || info.bits_per_sample > 32 || info.channels < 1 || info.channels > 8)
            bad("unsupported_streaminfo");
        if (std::all_of(info.pcm_md5.begin(), info.pcm_md5.end(), [](auto b) { return b == 0; }))
            bad("missing_pcm_md5");
        Bits bits{reader, info.audio_offset, context};
        Md5 md5;
        FlacFrames result;
        std::uint64_t count = 0;
        std::optional<bool> variable;
        unsigned fixed_samples = 0;
        std::vector<std::vector<std::int64_t>> channels(static_cast<std::size_t>(info.channels));
        std::vector<unsigned char> pcm;
        while (count < *info.total_samples) {
            check_context(context);
            const auto h = header(bits, info);
            if (!variable) { variable = h.variable; fixed_samples = h.samples; }
            if (*variable != h.variable || h.number != (h.variable ? count : result.frames)) bad("frame_sequence");
            if (h.samples > *info.total_samples - count) bad("sample_count_mismatch");
            const bool last = count + h.samples == *info.total_samples;
            if ((!last && h.samples < info.min_block_samples) ||
                (!h.variable && ((!last && h.samples != fixed_samples) || h.samples > fixed_samples)))
                bad("block_size_mismatch");
            for (std::size_t ch = 0; ch < channels.size(); ++ch) {
                channels[ch].resize(h.samples);
                const bool side = (h.assignment == 8 && ch == 1) || (h.assignment == 9 && ch == 0) ||
                                  (h.assignment == 10 && ch == 1);
                subframe(bits, channels[ch], static_cast<unsigned>(info.bits_per_sample) + (side ? 1U : 0U));
            }
            bits.padding(); bits.frame_crc();
            const auto bytes_per_sample = static_cast<unsigned>((info.bits_per_sample + 7) / 8);
            pcm.clear(); pcm.reserve(h.samples * channels.size() * bytes_per_sample);
            for (unsigned i = 0; i < h.samples; ++i) {
                if (h.assignment == 8) channels[1][i] = channels[0][i] - channels[1][i];
                else if (h.assignment == 9) channels[0][i] += channels[1][i];
                else if (h.assignment == 10) {
                    const auto mid = channels[0][i] * 2 + (channels[1][i] & 1);
                    const auto side = channels[1][i];
                    channels[0][i] = (mid + side) >> 1; channels[1][i] = (mid - side) >> 1;
                }
                for (const auto& channel : channels) {
                    range(channel[i], static_cast<unsigned>(info.bits_per_sample));
                    const auto value = static_cast<std::uint64_t>(channel[i]);
                    for (unsigned j = 0; j < bytes_per_sample; ++j) pcm.push_back(static_cast<unsigned char>(value >> (8 * j)));
                }
            }
            mbedtls_md5_update(&md5.value, pcm.data(), pcm.size());
            count += h.samples; ++result.frames;
        }
        mbedtls_md5_finish(&md5.value, result.pcm_md5.data());
        if (result.pcm_md5 != info.pcm_md5) bad("pcm_md5_mismatch");
        result.audio_end = bits.offset();
        return result;
    } catch (const Failure& failure) {
        return failure.error;
    }
}
} // namespace music_service::audio_processing::format
