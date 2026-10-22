#pragma once
#include "downloads/finalizer.hpp"
#include "application/ports/storage/work_files.hpp"

namespace music_service::downloads {
// Writes all members durably in a private directory. Nothing is published until the
// finalizer moves that directory; a crash during preparation leaves only workspace files.
domain::Result<void> prepare_sidecars(FinalizeRequest& request, ports::WorkFiles& files,
    const ports::TrackMetadata& metadata, const domain::OutputOptions& options,
    const ports::OperationContext& context);
} // namespace music_service::downloads
