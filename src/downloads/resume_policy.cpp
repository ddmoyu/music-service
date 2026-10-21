#include "downloads/resume_policy.hpp"

#include <utility>

namespace music_service::downloads {

namespace {

ResumeDecision restart(std::string reason) { return ResumeDecision{false, 0, std::nullopt, std::move(reason)}; }

bool strong(const std::optional<std::string>& etag) {
    return etag && !etag->empty() && etag->rfind("W/", 0) != 0;
}

bool same(const std::optional<std::string>& a, const std::optional<std::string>& b) {
    return a && b && !a->empty() && *a == *b;
}

} // namespace

ResumeDecision decide_resume(const std::optional<PartialFile>& partial, const ResourceIdentity& current) {
    if (!partial || partial->durable_bytes == 0) {
        return restart("no_partial");
    }
    if (partial->file_bytes < partial->durable_bytes) {
        return restart("shorter_than_checkpoint");
    }
    const auto& recorded = partial->recorded;
    if (recorded.track.empty() || recorded.track != current.track || recorded.quality != current.quality) {
        return restart("resource_changed");
    }
    if (!recorded.total_bytes || !current.total_bytes) {
        return restart("size_unknown");
    }
    if (*recorded.total_bytes != *current.total_bytes || partial->durable_bytes > *current.total_bytes) {
        return restart("size_changed");
    }
    if (strong(recorded.etag) && strong(current.etag) && *recorded.etag != *current.etag) {
        return restart("etag_changed"); // a changed tag outweighs any other validator
    }
    if (recorded.content_hash && current.content_hash && !recorded.content_hash->empty() &&
        !current.content_hash->empty() && *recorded.content_hash != *current.content_hash) {
        return restart("content_hash_changed"); // known other bytes, whatever the date says
    }
    std::optional<std::string> if_range;
    if (strong(recorded.etag) && same(recorded.etag, current.etag)) {
        if_range = current.etag;
    } else if (same(recorded.content_hash, current.content_hash)) {
        // Proves the resource; the server gets no If-Range, so the 206 is checked strictly.
    } else if (same(recorded.last_modified, current.last_modified)) {
        if_range = current.last_modified;
    } else {
        return restart("no_validator");
    }
    return ResumeDecision{true, partial->durable_bytes, std::move(if_range),
                          partial->durable_bytes == *current.total_bytes ? "complete" : "validated"};
}

} // namespace music_service::downloads
