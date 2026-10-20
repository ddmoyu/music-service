#pragma once

#include "music_dl/domain/result.hpp"

#include <compare>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace music_dl::domain {

// Stable platform identifier such as "kuwo": [a-z][a-z0-9_]{0,63}. Open set, not an enum.
class SourceId {
public:
    static Result<SourceId> parse(std::string_view text);

    [[nodiscard]] const std::string& value() const noexcept { return value_; }

    friend bool operator==(const SourceId&, const SourceId&) = default;
    friend auto operator<=>(const SourceId&, const SourceId&) = default;

private:
    explicit SourceId(std::string value) : value_(std::move(value)) {}
    std::string value_;
};

// Opaque platform-side identifier: valid UTF-8, 1..1024 bytes, never converted to a number.
class PlatformId {
public:
    static constexpr std::size_t max_bytes = 1024;

    static Result<PlatformId> parse(std::string_view text);

    [[nodiscard]] const std::string& value() const noexcept { return value_; }

    friend bool operator==(const PlatformId&, const PlatformId&) = default;
    friend auto operator<=>(const PlatformId&, const PlatformId&) = default;

private:
    explicit PlatformId(std::string value) : value_(std::move(value)) {}
    std::string value_;
};

// An entity on one platform. Not a task, file or asset identity.
struct PlatformRef {
    SourceId source;
    PlatformId id;

    friend bool operator==(const PlatformRef&, const PlatformRef&) = default;
    friend auto operator<=>(const PlatformRef&, const PlatformRef&) = default;
};

using TrackRef = PlatformRef;
using CollectionRef = PlatformRef;
using ArtistRef = PlatformRef;

// Service-generated identifier with a type prefix, e.g. "tsk_" + [A-Za-z0-9]{1,60}.
[[nodiscard]] bool is_valid_service_id(std::string_view prefix, std::string_view id) noexcept;

[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;

} // namespace music_dl::domain
