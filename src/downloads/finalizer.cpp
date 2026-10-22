#include "downloads/finalizer.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <utility>

namespace music_service::downloads {

namespace {

using domain::ErrorCode;
using nlohmann::json;

bool required_stage_failed(const ports::ProcessingReport& report) {
    return std::any_of(report.stages.begin(), report.stages.end(), [](const ports::StageReport& stage) {
        return stage.stage != "tags" && stage.outcome == ports::StageOutcome::failed;
    });
}

bool validated(const ports::ProcessingReport& report) {
    return std::any_of(report.stages.begin(), report.stages.end(), [](const ports::StageReport& stage) {
        return stage.stage == "validate" && stage.outcome == ports::StageOutcome::done;
    });
}

// complete | partial | failed | skipped (data-models.md, metadata_policy).
std::string metadata_status(const ports::ProcessingReport& report) {
    for (const auto& stage : report.stages) {
        if (stage.stage == "tags") {
            if (stage.outcome == ports::StageOutcome::done) {
                const auto partial = std::find(report.warnings.begin(), report.warnings.end(),
                                               ports::processing_warning::metadata_partial);
                return partial != report.warnings.end() ? "partial" : "complete";
            }
            if (stage.outcome == ports::StageOutcome::failed) {
                return "failed";
            }
        }
    }
    const auto not_written = std::find(report.warnings.begin(), report.warnings.end(),
                                       ports::processing_warning::metadata_not_written);
    return not_written != report.warnings.end() ? "failed" : "skipped";
}

std::string result_of(const ports::ProcessingReport& report) {
    json metadata = json::object();
    for (const auto& [field, status] : report.metadata_fields) metadata[field] = status;
    json processing{{"processor", report.processor_id},
                    {"version", report.processor_version},
                    {"completeness", std::string{domain::to_string(report.completeness)}}};
    if (report.facts.duration_ms) {
        processing["duration_ms"] = *report.facts.duration_ms;
    }
    return json{{"metadata_status", metadata_status(report)},
                {"metadata_fields", std::move(metadata)},
                {"warnings", report.warnings},
                {"processing", std::move(processing)}}
        .dump();
}

domain::Error audio_refused(std::string reason) {
    return domain::Error{ErrorCode::audio_invalid, "the processed file must not be published", false,
                         {{"audio", std::move(reason)}}};
}

} // namespace

std::string candidate_path(std::string_view wanted, std::uint32_t n) {
    if (n <= 1) {
        return std::string{wanted};
    }
    const auto slash = wanted.rfind('/');
    const auto name_start = slash == std::string_view::npos ? 0 : slash + 1;
    auto dot = wanted.rfind('.');
    if (dot == std::string_view::npos || dot <= name_start) {
        dot = wanted.size(); // no extension, or a name that starts with the dot
    }
    return std::string{wanted.substr(0, dot)} + " (" + std::to_string(n) + ")" + std::string{wanted.substr(dot)};
}

std::string encode_evidence(const PublicationEvidence& evidence) {
    return json{{"size", evidence.file.size_bytes},
                {"sha256", evidence.file.sha256},
                {"wanted_path", evidence.wanted_path},
                {"container", evidence.container},
                {"properties", json::parse(evidence.properties_json, nullptr, false)},
                {"result", json::parse(evidence.result_json, nullptr, false)}}
        .dump();
}

domain::Result<PublicationEvidence> decode_evidence(std::string_view text) {
    const auto parsed = json::parse(text, nullptr, false);
    const auto invalid = [] { return domain::Error::validation("evidence", "is not a publication record"); };
    if (!parsed.is_object()) {
        return invalid();
    }
    const auto size = parsed.find("size");
    const auto sha = parsed.find("sha256");
    const auto wanted = parsed.find("wanted_path");
    const auto container = parsed.find("container");
    const auto properties = parsed.find("properties");
    const auto result = parsed.find("result");
    if (size == parsed.end() || !size->is_number_unsigned() || sha == parsed.end() || !sha->is_string() ||
        wanted == parsed.end() || !wanted->is_string() || container == parsed.end() || !container->is_string() ||
        properties == parsed.end() || !properties->is_object() || result == parsed.end() || !result->is_object()) {
        return invalid();
    }
    return PublicationEvidence{{size->get<std::uint64_t>(), sha->get<std::string>()},
                               wanted->get<std::string>(),
                               container->get<std::string>(),
                               properties->dump(),
                               result->dump()};
}

domain::Result<bool> publication_matches(ports::DestinationFiles& files, const std::string& destination,
    const std::string& path, const PublicationEvidence& evidence) {
    auto members = application::media::bundle_members(evidence.properties_json);
    if (!members) return members.error();
    if (members->empty()) {
        auto actual = files.fingerprint(destination, path);
        if (!actual) return actual.error();
        return actual.value() && **actual == evidence.file;
    }
    if (members->front().file != evidence.file) return domain::Error::validation("bundle", "audio evidence differs");
    for (const auto& member : *members) {
        auto actual = files.fingerprint(destination, path + "/" + member.name);
        if (!actual) return actual.error();
        if (!actual.value() || **actual != member.file) return false;
    }
    return true;
}

std::string file_etag(const ports::FileFingerprint& file) {
    return "\"sha256-" + file.sha256.substr(0, 32) + "\"";
}

std::string finalization_error_json(std::string_view reason, const domain::Error& cause) {
    return json{{"code", std::string{domain::to_string(ErrorCode::finalization_failed)}},
                {"message", "the file could not be published"},
                {"retryable", false},
                {"details", json::array({json{{"field", "publication"}, {"reason", std::string{reason}}},
                                         json{{"field", "cause"},
                                              {"reason", std::string{domain::to_string(cause.code)}}}})}}
        .dump();
}

domain::Result<std::string> move_to_reservation(ports::PublicationRepository& publications,
                                                ports::DestinationFiles& files,
                                                const ports::PublicationIntent& intent, std::string_view wanted_path,
                                                std::uint32_t max_name_candidates) {
    std::string reserved = intent.reserved_path;
    for (std::uint32_t round = 0; round < max_name_candidates; ++round) {
        auto moved = files.move(intent.destination_id, intent.workspace_file, reserved);
        if (moved) {
            return reserved;
        }
        if (moved.error().code != ErrorCode::conflict) {
            // The rename may have happened before flushing (or unlinking the source) failed:
            // then the intent must survive, so recovery can compare the target with it.
            auto arrived = files.exists(intent.destination_id, reserved);
            if (arrived && arrived.value()) {
                auto cause = std::move(moved).error();
                cause.details.push_back({"publication", std::string{move_unconfirmed}});
                return cause;
            }
            return std::move(moved).error();
        }
        // An unknown file holds the name: keep it, and move the reservation instead.
        bool relocated = false;
        for (std::uint32_t n = 1; n <= max_name_candidates && !relocated; ++n) {
            auto candidate = candidate_path(wanted_path, n);
            if (candidate == reserved) {
                continue;
            }
            auto present = files.exists(intent.destination_id, candidate);
            if (!present) {
                return std::move(present).error();
            }
            if (present.value()) {
                continue;
            }
            auto changed = publications.relocate(intent.task_id, intent.execution_id, candidate);
            if (!changed) {
                if (changed.error().code == ErrorCode::conflict) {
                    continue;
                }
                return std::move(changed).error();
            }
            reserved = std::move(candidate);
            relocated = true;
        }
        if (!relocated) {
            break;
        }
    }
    return domain::Error{ErrorCode::conflict, "no free target name", false, {}};
}

bool is_move_unconfirmed(const domain::Error& error) {
    return std::any_of(error.details.begin(), error.details.end(), [](const domain::ErrorDetail& detail) {
        return detail.field == "publication" && detail.reason == move_unconfirmed;
    });
}

domain::Result<ports::CompletedPublication> commit_publication(ports::PublicationRepository& publications,
                                                               ports::DestinationFiles& files,
                                                               const ports::PendingPublication& pending,
                                                               const PublicationEvidence& evidence) {
    const auto& intent = pending.intent;
    auto members = application::media::bundle_members(evidence.properties_json);
    if (!members) return members.error();
    const auto audio_path = intent.reserved_path + (members->empty() ? "" : "/" + members->front().name);
    const ports::NewFileAsset asset{pending.task.owner_id,
                                    intent.destination_id,
                                    audio_path,
                                    evidence.file.size_bytes,
                                    evidence.container.empty() ? std::nullopt
                                                               : std::optional<std::string>{evidence.container},
                                    file_etag(evidence.file),
                                    evidence.properties_json};
    auto completed = publications.complete(intent.task_id, intent.execution_id, pending.task.version, asset,
                                           evidence.result_json);
    if (!completed) {
        return completed;
    }
    // Published files never live in the work directory; what is left there is waste.
    (void)files.remove_workspace(intent.destination_id, intent.task_id);
    return completed;
}

Finalizer::Finalizer(std::shared_ptr<ports::PublicationRepository> publications,
                     std::shared_ptr<ports::DestinationFiles> files, FinalizerOptions options)
    : publications_(std::move(publications)), files_(std::move(files)), options_(options) {}

domain::Result<ports::CompletedPublication> Finalizer::publish(const FinalizeRequest& request) {
    const auto& task = request.task;
    if (!task.execution_id) {
        return domain::Error{ErrorCode::version_conflict, "task has no running execution", true, {}};
    }
    if (required_stage_failed(request.report) || !validated(request.report) || request.report.facts.sha256.empty()) {
        return audio_refused("not_validated");
    }
    const auto audio_work = request.workspace_file + (request.bundle.empty() ? "" : "/" + request.bundle.front().name);
    auto bytes = files_->fingerprint(task.destination_id, audio_work);
    if (!bytes) {
        return std::move(bytes).error();
    }
    if (!bytes.value()) {
        return domain::Error{ErrorCode::file_system, "the processed file is missing", false,
                             {{"workspace_file", "missing"}}};
    }
    const auto file = *bytes.value();
    if (file.size_bytes != static_cast<std::uint64_t>(request.report.facts.size_bytes) ||
        file.sha256 != request.report.facts.sha256) {
        return audio_refused("changed_after_validation");
    }
    if (request.target_path.empty()) {
        return domain::Error::validation("target_path", "must not be empty");
    }
    auto properties = json::parse(request.properties_json, nullptr, false);
    if (!properties.is_object()) {
        return domain::Error::validation("properties", "must be a JSON object");
    }
    // The file's own record says whether its tags were written (api-contract.md 10.1).
    if (!properties.contains("metadata_status")) {
        properties["metadata_status"] = metadata_status(request.report);
    }

    if (!request.bundle.empty()) {
        auto members = request.bundle;
        for (auto& member : members) {
            auto part = files_->fingerprint(task.destination_id, request.workspace_file + "/" + member.name);
            if (!part || !part.value() || part.value()->size_bytes != member.file.size_bytes)
                return audio_refused("bundle_member_changed");
            if (!member.file.sha256.empty() && member.file.sha256 != part.value()->sha256)
                return audio_refused("bundle_member_changed");
            member.file = **part;
        }
        properties = json::parse(application::media::with_bundle(properties.dump(), members));
        if (!application::media::bundle_members(properties.dump())) return audio_refused("bundle_invalid");
    }
    const PublicationEvidence evidence{file, request.target_path, request.report.facts.container, properties.dump(),
                                       result_of(request.report)};
    ports::PublicationIntent intent{task.task_id, *task.execution_id, task.destination_id, request.workspace_file,
                                    {}, encode_evidence(evidence), "reserved"};
    std::optional<ports::TaskRecord> finalizing;
    for (std::uint32_t n = 1; n <= options_.max_name_candidates && !finalizing; ++n) {
        intent.reserved_path = candidate_path(request.target_path, n);
        auto present = files_->exists(task.destination_id, intent.reserved_path);
        if (!present) {
            return std::move(present).error();
        }
        if (present.value()) {
            continue; // an unknown file is never a candidate
        }
        auto begun = publications_->begin(intent, task.version);
        if (begun) {
            finalizing = std::move(begun).value();
        } else if (begun.error().code != ErrorCode::conflict) {
            return std::move(begun).error(); // a cancel committed first, storage, ...
        }
    }
    if (!finalizing) {
        return domain::Error{ErrorCode::conflict, "no free target name", true, {}};
    }

    auto placed = move_to_reservation(*publications_, *files_, intent, request.target_path,
                                      options_.max_name_candidates);
    if (!placed) {
        auto cause = std::move(placed).error();
        if (is_move_unconfirmed(cause)) {
            return cause; // still finalizing with the intent; recovery checks the target
        }
        const auto reason = cause.code == ErrorCode::conflict ? "target_conflict" : "move_failed";
        auto failed = publications_->abandon(task.task_id, *task.execution_id, finalizing->version,
                                             domain::TaskStatus::failed, finalization_error_json(reason, cause));
        if (!failed) {
            return std::move(failed).error(); // still finalizing; recovery decides at restart
        }
        return domain::Error{ErrorCode::finalization_failed, "the file could not be published", false,
                             {{"publication", reason}}};
    }
    intent.reserved_path = std::move(placed).value();
    // Only a hint for diagnosis: recovery decides from the files and the evidence.
    (void)publications_->set_stage(task.task_id, *task.execution_id, "moved");
    return commit_publication(*publications_, *files_, ports::PendingPublication{intent, *finalizing}, evidence);
}

} // namespace music_service::downloads
