#pragma once

#include "application/ports/processing/audio_processor.hpp"
#include "application/ports/storage/destination_files.hpp"
#include "application/ports/storage/publication_repository.hpp"
#include "domain/result.hpp"
#include "application/media/file_bundle.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace music_service::downloads {

namespace ports = application::ports;

// The candidate name for the n-th try (1-based): the wanted path itself, then
// "stem (2).ext", "stem (3).ext", ... in the same directory. Deterministic, so a restart
// probes the same names in the same order.
std::string candidate_path(std::string_view wanted, std::uint32_t n);

// What the publication intent stores, enough to finish the publication without the
// runner: the size and hash of the bytes, the asset fields and the task result.
struct PublicationEvidence {
    ports::FileFingerprint file;
    std::string wanted_path; // first candidate name, to derive the others from
    std::string container;
    std::string properties_json{"{}"}; // object stored with the asset
    std::string result_json{"{}"};     // object stored as the task result
};

std::string encode_evidence(const PublicationEvidence& evidence);
domain::Result<PublicationEvidence> decode_evidence(std::string_view json);

// The strong ETag of published bytes, derived from their SHA-256.
std::string file_etag(const ports::FileFingerprint& file);

// Finishes a publication whose bytes are at the reserved path, as the caller has checked
// (the Finalizer after its rename, recovery by comparing with the evidence): asset, task
// result and completed state in one transaction, then the work directory goes. If the
// commit fails, the intent stays pending for the next recovery.
domain::Result<ports::CompletedPublication> commit_publication(ports::PublicationRepository& publications,
                                                               ports::DestinationFiles& files,
                                                               const ports::PendingPublication& pending,
                                                               const PublicationEvidence& evidence);

struct FinalizeRequest {
    ports::TaskRecord task;     // processing, held by the calling execution
    std::string workspace_file; // the processed file, relative to the destination root
    std::string target_path;    // wanted relative path including the extension
    ports::ProcessingReport report;
    std::string properties_json{"{}"}; // actual track, quality, ... stored with the asset
    std::vector<application::media::BundleMember> bundle{}; // audio first; workspace/target are directories
};

// Checks every member against the durable manifest; legacy products remain single files.
domain::Result<bool> publication_matches(ports::DestinationFiles& files, const std::string& destination,
    const std::string& path, const PublicationEvidence& evidence);

struct FinalizerOptions {
    std::uint32_t max_name_candidates{100};
};

// Publishes a verified work file (internal-contracts.md 7, runtime-and-tasks.md step 7-8):
//   1. refuses a report with a failed required stage, or without the SHA-256 of the
//      verified bytes, or a file whose size or SHA-256 no longer matches them (A19) -
//      nothing is reserved or moved then;
//   2. reserves the first free candidate name, skipping names an unknown file or another
//      reservation holds, and commits processing -> finalizing with the intent; a cancel
//      committed first wins and the task is left to the runner (A12);
//   3. renames without replacing; if an unknown file appeared at the name meanwhile, the
//      reservation moves to the next candidate, so nothing is overwritten;
//   4. commits the asset and completed state, then removes the work directory.
// A rename or directory failure fails the task with FINALIZATION_FAILED and keeps the work
// file for diagnosis (A17). If the final commit fails, or the move failed after the target
// appeared (its durability is unknown), the task stays finalizing and startup recovery
// completes it from the intent.
class Finalizer {
public:
    Finalizer(std::shared_ptr<ports::PublicationRepository> publications,
              std::shared_ptr<ports::DestinationFiles> files, FinalizerOptions options = {});

    domain::Result<ports::CompletedPublication> publish(const FinalizeRequest& request);

private:
    std::shared_ptr<ports::PublicationRepository> publications_;
    std::shared_ptr<ports::DestinationFiles> files_;
    FinalizerOptions options_;
};

// The task error recorded when a publication cannot complete.
std::string finalization_error_json(std::string_view reason, const domain::Error& cause);

// The "publication" detail of a move that failed after the target appeared (e.g. the
// directory flush after the rename failed): the intent is kept for recovery to check.
inline constexpr std::string_view move_unconfirmed = "move_unconfirmed";
bool is_move_unconfirmed(const domain::Error& error);

// Moves the work file to the reservation, relocating it past unknown files, as step 3
// above. Used by the Finalizer and by recovery.
// Returns the path the file ended up at.
domain::Result<std::string> move_to_reservation(ports::PublicationRepository& publications,
                                                ports::DestinationFiles& files,
                                                const ports::PublicationIntent& intent,
                                                std::string_view wanted_path,
                                                std::uint32_t max_name_candidates);

} // namespace music_service::downloads
