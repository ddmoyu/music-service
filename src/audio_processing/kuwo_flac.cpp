#include "audio_processing/kuwo_flac.hpp"
#include "audio_processing/format/flac_frames.hpp"
#include "audio_processing/verifying_processor.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <system_error>

namespace music_service::audio_processing {
namespace ports = application::ports;
namespace {
domain::Error invalid(const char* reason) {
    return {domain::ErrorCode::audio_invalid, "the Kuwo FLAC file could not be verified", false, {{"audio", reason}}};
}
std::optional<domain::Error> interrupted(const ports::OperationContext& context) {
    if (context.stop.stop_requested()) return domain::Error{domain::ErrorCode::canceled, "FLAC processing was canceled", false, {}};
    if (context.expired(std::chrono::steady_clock::now()))
        return domain::Error{domain::ErrorCode::deadline_exceeded, "FLAC processing exceeded its deadline", true, {}};
    return {};
}
struct WorkingCopy {
    std::filesystem::path path;
    bool owned{false};
    bool keep{false};
    ~WorkingCopy() { if (owned && !keep) { std::error_code ec; std::filesystem::remove(path, ec); } }
};
domain::Result<void> copy_prefix(format::ByteReader& reader, const std::filesystem::path& path,
    std::int64_t end, const ports::OperationContext& context) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output) return domain::Error{domain::ErrorCode::file_system, "cannot create the FLAC working copy", true, {}};
    for (std::int64_t offset = 0; offset < end;) {
        if (auto stop = interrupted(context)) return *stop;
        auto bytes = reader.read(offset, static_cast<std::size_t>(std::min<std::int64_t>(format::ByteReader::window_bytes, end - offset)));
        if (!bytes) return bytes.error();
        if (bytes->empty()) return invalid("changed_during_copy");
        output.write(bytes->data(), static_cast<std::streamsize>(bytes->size()));
        if (!output) return domain::Error{domain::ErrorCode::file_system, "cannot write the FLAC working copy", true, {}};
        offset += static_cast<std::int64_t>(bytes->size());
    }
    output.close();
    if (!output) return domain::Error{domain::ErrorCode::file_system, "cannot finish the FLAC working copy", true, {}};
    return domain::ok();
}
} // namespace

ports::ProcessingPlan KuwoFlacProcessor::plan() const { return {"audio.kuwo-flac", 1, {}}; }

domain::Result<ports::ProcessingReport> KuwoFlacProcessor::process(const ports::ProcessingRequest& request,
    const ports::OperationContext& context) {
    if (auto stop = interrupted(context)) return *stop;
    auto reader = format::ByteReader::open(request.input);
    if (!reader) return reader.error();
    // 先校验网络传输的完整大小，不能以清理尾部掩盖截断下载。
    if (request.expected.size_bytes && reader->size() != *request.expected.size_bytes) return invalid("transfer_size_mismatch");
    auto info = format::scan_flac(*reader, context);
    if (!info) return info.error();
    auto frames = format::verify_flac_frames(*reader, *info, context);
    if (!frames) return frames.error();
    const auto removed = reader->size() - frames->audio_end;
    if (removed < 0 || removed > 4096) return invalid("unsupported_kuwo_trailer");
    if (removed > 0) {
        auto trailer = reader->read(frames->audio_end, static_cast<std::size_t>(removed));
        if (!trailer) return trailer.error();
        // 识别实测附加数据的首尾标记，长度由已验证的末帧决定，不固定删除 30 字节。
        if (trailer->size() < 8 || !trailer->starts_with(std::string_view{"\xf0\x00\xff\x0f", 4}) ||
            !trailer->ends_with(std::string_view{"\x0e\x55\xff\xf0", 4})) return invalid("unsupported_kuwo_trailer");
    }
    auto plain = request;
    WorkingCopy working{request.input.parent_path() / "kuwo-clean.flac"};
    if (removed > 0) {
        if (working.path == request.input) return invalid("working_path_collision");
        working.owned = true;
        auto copied = copy_prefix(*reader, working.path, frames->audio_end, context);
        if (!copied) return copied.error();
        plain.input = working.path;
        plain.expected.size_bytes = frames->audio_end;
    }
    auto report = VerifyingProcessor{}.process(plain, context);
    if (!report) return report.error();
    WorkingCopy tagged{report->output, report->output != request.input && report->output != working.path};
    // 标签写入或复制后再次完整验证；发布依据最终文件，不能只检查输入的头信息。
    auto output = format::ByteReader::open(report->output);
    if (!output) return output.error();
    auto final_info = format::scan_flac(*output, context);
    if (!final_info) return final_info.error();
    auto final_frames = format::verify_flac_frames(*output, *final_info, context);
    if (!final_frames) return final_frames.error();
    if (final_frames->audio_end != output->size() || final_frames->pcm_md5 != frames->pcm_md5 ||
        final_info->sample_rate_hz != info->sample_rate_hz || final_info->bits_per_sample != info->bits_per_sample ||
        final_info->channels != info->channels || final_info->total_samples != info->total_samples)
        return invalid("payload_changed");
    report->processor_id = plan().processor_id;
    report->processor_version = plan().processor_version;
    report->facts.frames = final_frames->frames;
    if (removed > 0) report->warnings.emplace_back("KUWO_TRAILER_REMOVED");
    working.keep = report->output == working.path;
    tagged.keep = true;
    return report;
}
} // namespace music_service::audio_processing
