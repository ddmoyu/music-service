#include "server/ncm_import.hpp"
#include "audio_processing/ncm.hpp"
#include "downloads/finalizer.hpp"
#include "downloads/sidecars.hpp"
#include "downloads/recovery.hpp"
#include "downloads/metadata_support.hpp"
#include "infrastructure/filesystem/destination_files.hpp"
#include "infrastructure/filesystem/paths.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <algorithm>
#include <fstream>

namespace music_service::server {
namespace ports = application::ports;
namespace local = infrastructure::filesystem;
domain::Result<ports::CompletedPublication> import_ncm(const application::config::ServiceConfig& config,
    const std::filesystem::path& input, const std::string& owner, const std::string& destination,
    domain::MetadataPolicy policy, const ports::OperationContext& context) {
    if (context.stop.stop_requested()) return domain::Error{domain::ErrorCode::canceled, "local import canceled", false, {}};
    auto instance = open_instance(config);
    if (!instance) return instance.error();
    auto& inst = **instance;
    auto principal = inst.identities->get_principal(owner);
    if (!principal || !principal->enabled) return domain::Error::validation("principal", "requires an enabled principal");
    auto destinations = register_destinations(inst, config);
    if (!destinations) return destinations.error();
    std::map<std::string, std::filesystem::path> roots;
    const DestinationState* chosen = nullptr;
    for (const auto& item : *destinations) {
        roots.emplace(item.config.id, item.root);
        if (item.config.id == destination && item.available && item.config.accept_new_tasks &&
            std::find(item.config.principals.begin(), item.config.principals.end(), owner) != item.config.principals.end()) chosen = &item;
    }
    if (!chosen) return domain::Error::validation("destination", "requires an available destination permitted to the principal");
    auto files = std::make_shared<local::LocalDestinationFiles>(std::move(roots));
    auto recovered = downloads::Recovery{inst.publications, inst.tasks, files}.run();
    if (!recovered) return recovered.error();
    auto interrupted = inst.tasks->requeue_interrupted(inst.clock->now());
    if (!interrupted) return interrupted.error();
    std::error_code ec;
    const auto source_path = std::filesystem::canonical(input, ec);
    if (ec || !std::filesystem::is_regular_file(source_path, ec)) return domain::Error::validation("input", "requires a local regular NCM file");
    const auto size = std::filesystem::file_size(source_path, ec);
    if (ec || size == 0 || size > config.downloads.max_file_bytes || size > 2ULL * 1024 * 1024 * 1024)
        return domain::Error::validation("input", "exceeds the configured file limit or NCM 2 GiB limit");
    local::LocalDestinationFiles source_files{{{"input", source_path.parent_path()}}};
    auto identity = source_files.fingerprint("input", local::path_to_utf8(source_path.filename()));
    if (!identity || !identity.value()) return domain::Error::validation("input", "cannot fingerprint input");
    auto source = domain::SourceId::parse("local_ncm").value();
    auto binding = domain::AccountBinding::make(owner, source, domain::AccountMode::anonymous, "anonymous", {}, {});
    if (!binding) return binding.error();
    domain::DownloadIntentDraft draft{owner, {source, domain::PlatformId::parse(identity.value()->sha256).value()},
        domain::SameSource{}, {*binding}, {domain::Quality::standard, domain::QualityPolicy::allow_lower, false}, policy,
        {destination, "local-ncm-v1", "naming-v2", "metadata-v3", config.downloads.output}};
    auto intent = domain::NormalizedDownloadIntent::normalize(std::move(draft));
    if (!intent) return intent.error();
    auto completed = inst.tasks->completed_results(owner, domain::fingerprint(*intent));
    if (!completed) return completed.error();
    for (const auto& old : *completed) {
        if (!old.file_available) continue;
        auto asset = inst.publications->get_asset(owner, old.candidate.file_id);
        if (!asset) continue;
        auto members = application::media::bundle_members(asset->properties_json);
        if (!members) continue;
        bool intact = true;
        if (members->empty()) {
            auto bytes = files->fingerprint(destination, old.relative_path);
            intact = bytes && bytes.value() && downloads::file_etag(**bytes) == asset->etag && (**bytes).size_bytes == old.size_bytes;
        } else for (const auto& member : *members) {
            auto checked = files->matches(destination, application::media::sibling_path(old.relative_path, member.name), member.file);
            if (!checked || !*checked) { intact = false; break; }
        }
        if (intact) {
            auto task = inst.tasks->get(owner, old.candidate.task_id);
            if (task) return ports::CompletedPublication{*task, *asset};
        }
    }
    auto created = inst.tasks->create({*intent, inst.ids->next("import_"), identity.value()->sha256, false, "local:import-ncm"});
    if (!created) return created.error();
    auto claimed = inst.tasks->claim(created->task.task_id, inst.clock->now());
    if (!claimed || !claimed.value()) return domain::Error::validation("import", "could not claim local task");
    auto task = **claimed;
    const auto fail = [&](const domain::Error& cause) -> domain::Result<ports::CompletedPublication> {
        const auto error = nlohmann::json{{"code", domain::to_string(cause.code)}, {"message", cause.message}, {"retryable", false}}.dump();
        if (cause.code == domain::ErrorCode::canceled) {
            auto canceled = inst.tasks->request_cancel(owner, task.task_id);
            if (canceled && canceled->status == domain::TaskStatus::canceling)
                (void)inst.tasks->transition({task.task_id, *task.execution_id, canceled->version, domain::TaskStatus::canceled, {}, {}, {}, {}, {}, error});
        } else (void)inst.tasks->transition({task.task_id, *task.execution_id, task.version, domain::TaskStatus::failed, {}, {}, {}, {}, {}, error});
        (void)files->remove_workspace(destination, task.task_id);
        return cause;
    };
    auto next = inst.tasks->transition({task.task_id, *task.execution_id, task.version, domain::TaskStatus::downloading, {}, {}, {}});
    if (!next) return next.error();
    task = *next;
    auto work = files->open(destination, task.task_id, "input.ncm", 0);
    if (!work) return fail(work.error());
    std::ifstream stream{source_path, std::ios::binary};
    std::array<char, 65536> buffer{};
    std::uint64_t copied = 0;
    while (stream) {
        if (context.stop.stop_requested() || context.expired(std::chrono::steady_clock::now())) {
            work->reset();
            return fail({context.stop.stop_requested() ? domain::ErrorCode::canceled : domain::ErrorCode::deadline_exceeded,
                "local import interrupted", false, {}});
        }
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        copied += static_cast<std::uint64_t>(stream.gcount());
        if (copied > size) { work->reset(); return fail(domain::Error::validation("input", "changed while copying")); }
        auto written = (*work)->write({buffer.data(), static_cast<std::size_t>(stream.gcount())});
        if (!written) { work->reset(); return fail(written.error()); }
    }
    if (stream.bad() || copied != size) { work->reset(); return fail(domain::Error::validation("input", "changed while copying")); }
    auto flushed = (*work)->flush();
    work->reset();
    if (!flushed) return fail(flushed.error());
    auto copied_hash = files->fingerprint(destination, files->relative_path(task.task_id, "input.ncm"));
    if (!copied_hash || !copied_hash.value() || **copied_hash != **identity)
        return fail(domain::Error::validation("input", "changed while copying"));
    next = inst.tasks->transition({task.task_id, *task.execution_id, task.version, domain::TaskStatus::processing, {}, {}, {}});
    if (!next) return next.error();
    task = *next;
    auto input_path = files->local_path(destination, files->relative_path(task.task_id, "input.ncm"));
    if (!input_path) return fail(input_path.error());
    ports::ProcessingRequest request{*input_path, {}, policy, {}};
    request.embed_cover = config.downloads.output.embed_cover;
    request.embed_lyrics = config.downloads.output.embed_lyrics;
    auto report = audio_processing::NcmProcessor{}.process(request, context);
    if (!report) return fail(report.error());
    const auto metadata = report->source_metadata.value_or(ports::TrackMetadata{});
    domain::Track track{intent->original_track(), metadata.title.empty() ? local::path_to_utf8(source_path.stem()) : metadata.title,
                       {}, {}, {}, {}};
    for (const auto& artist : metadata.artists) track.artists.push_back({artist, {}});
    if (metadata.album) track.album = domain::AlbumSummary{*metadata.album, {}};
    const auto name = downloads::output_name(track, config.downloads.output, [](std::string_view text) { return local::sanitize_file_name(text); });
    downloads::FinalizeRequest publish{task, files->relative_path(task.task_id, local::path_to_utf8(report->output.filename())),
        name + "." + report->facts.container, *report, R"({"origin":"local_ncm"})"};
    auto sides = downloads::prepare_sidecars(publish, *files, metadata, config.downloads.output, context);
    if (!sides) return fail(sides.error());
    if (context.stop.stop_requested()) return fail({domain::ErrorCode::canceled, "local import canceled", false, {}});
    auto result = downloads::Finalizer{inst.publications, files}.publish(publish);
    if (!result) {
        auto current = inst.tasks->get(owner, task.task_id);
        // A durable intent survives any ambiguous rename/commit. Recovery owns it now.
        if (current && current->status == domain::TaskStatus::processing) return fail(result.error());
    }
    return result;
}
}
