#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace music_service::downloads {

// What identifies the bytes of one resource (runtime-and-tasks.md, 重试、换源和续传).
// Same URL, file name or a similar duration are deliberately not part of it.
struct ResourceIdentity {
    std::string track;   // actual track, "source:id"
    std::string quality; // actual quality
    std::optional<std::uint64_t> total_bytes;
    std::optional<std::string> etag;          // as sent; a weak "W/" tag never validates
    std::optional<std::string> last_modified; // HTTP-date as sent
    std::optional<std::string> content_hash;  // platform-provided file hash
};

// A work file found again after a pause or restart, with the evidence its committed
// checkpoint recorded. durable_bytes is what was flushed before that checkpoint was
// committed; the file length alone is never trusted.
struct PartialFile {
    std::uint64_t file_bytes{0};
    std::uint64_t durable_bytes{0};
    ResourceIdentity recorded;
};

struct ResumeDecision {
    bool resume{false};
    std::uint64_t offset{0};             // truncate the work file here, then request bytes=offset-
    std::optional<std::string> if_range; // strong ETag or Last-Modified to send with the Range
    std::string reason;                  // why: for logs and the attempt's evidence
};

// Resume only when the partial bytes provably belong to the current resource: same track
// and quality, the same known total size, and one matching validator (strong ETag,
// platform hash or Last-Modified). Anything less restarts from zero instead of appending
// to bytes of another recording or quality. The offset is the durable checkpoint, so
// unflushed tail bytes are cut. An offset equal to the total goes to completion checks.
ResumeDecision decide_resume(const std::optional<PartialFile>& partial, const ResourceIdentity& current);

} // namespace music_service::downloads
