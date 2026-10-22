#include "downloads/sidecars.hpp"
#include "downloads/metadata_support.hpp"
#include <array>
#include <fstream>

namespace music_service::downloads {
domain::Result<void> prepare_sidecars(FinalizeRequest& request, ports::WorkFiles& files,
    const ports::TrackMetadata& metadata, const domain::OutputOptions& options,
    const ports::OperationContext& context) {
    if (!options.export_lyrics && !options.export_cover) return domain::ok();
    const auto fail = [](std::string reason) { return domain::Error{domain::ErrorCode::metadata_failed,
        "sidecar preparation failed", false, {{"sidecars", std::move(reason)}}}; };
    const auto slash = request.target_path.rfind('/');
    const auto filename = request.target_path.substr(slash == std::string::npos ? 0 : slash + 1);
    const auto stem = filename.substr(0, filename.rfind('.'));
    std::vector<std::pair<application::media::BundleMember, std::string>> sides;
    const auto add = [&](bool enabled, const char* kind, const std::string& extension, const std::string& data)
        -> domain::Result<void> {
        if (!enabled) return domain::ok();
        request.report.metadata_fields.emplace_back(std::string{kind} + "_file", data.empty() ? "unavailable" : "written");
        if (data.empty()) {
            if (options.sidecars_required) return fail(std::string{kind} + "_unavailable");
            request.report.warnings.push_back(std::string{"SIDECAR_"} + (std::string{kind} == "lyrics" ? "LYRICS" : "COVER") + "_UNAVAILABLE");
        } else sides.push_back({{stem + "." + extension, kind, extension, {}}, data});
        return domain::ok();
    };
    if (metadata.lyrics && metadata.lyrics->size() > 2 * 1024 * 1024) return fail("lyrics_limit");
    if (auto r = add(options.export_lyrics, "lyrics", "lrc", metadata.lyrics.value_or("")); !r) return r;
    std::string image;
    std::string ext = "jpg";
    if (metadata.cover && metadata.cover->data.size() <= options.cover_max_bytes) {
        const auto mime = picture_mime(metadata.cover->data);
        if (mime) { image = metadata.cover->data; ext = *mime == "image/png" ? "png" : "jpg"; }
    }
    if (auto r = add(options.export_cover, "cover", ext, image); !r) return r;
    auto audio = files.open(request.task.destination_id, request.task.task_id, "bundle/" + filename, 0);
    if (!audio) return audio.error();
    std::ifstream in{request.report.output, std::ios::binary};
    if (!in) return fail("audio_read");
    std::array<char, 65536> buffer{};
    while (in) {
        if (context.stop.stop_requested()) return domain::Error{domain::ErrorCode::canceled, "sidecars canceled", false, {}};
        if (context.expired(std::chrono::steady_clock::now()))
            return domain::Error{domain::ErrorCode::deadline_exceeded, "sidecars deadline exceeded", false, {}};
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        auto r = (*audio)->write({buffer.data(), static_cast<std::size_t>(in.gcount())});
        if (!r) return r;
    }
    if (in.bad()) return fail("audio_read");
    if (auto r = (*audio)->flush(); !r) return r;
    audio->reset();
    request.bundle = {{filename, "audio", request.report.facts.container,
        {static_cast<std::uint64_t>(request.report.facts.size_bytes), request.report.facts.sha256}}};
    for (auto& [member, data] : sides) {
        auto out = files.open(request.task.destination_id, request.task.task_id, "bundle/" + member.name, 0);
        if (!out) return out.error();
        if (auto r = (*out)->write({data.data(), data.size()}); !r) return r;
        if (auto r = (*out)->flush(); !r) return r;
        member.file.size_bytes = data.size();
        request.bundle.push_back(std::move(member));
    }
    request.workspace_file = files.relative_path(request.task.task_id, "bundle");
    request.target_path = request.target_path.substr(0, request.target_path.rfind('.')) + ".files";
    return domain::ok();
}
} // namespace music_service::downloads
