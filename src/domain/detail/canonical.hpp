#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace music_service::domain::detail {

// Internal to the domain library. Length-prefixed field encoding: "<tag>=<bytes>:<value>;". Any byte sequence,
// including separators, encodes unambiguously. Absent optionals encode as "<tag>~;".
class CanonicalWriter {
public:
    CanonicalWriter& field(std::string_view tag, std::string_view value) {
        out_.append(tag).push_back('=');
        out_.append(std::to_string(value.size())).push_back(':');
        out_.append(value).push_back(';');
        return *this;
    }
    CanonicalWriter& number(std::string_view tag, std::uint64_t value) {
        return field(tag, std::string_view{std::to_string(value)});
    }
    CanonicalWriter& flag(std::string_view tag, bool value) {
        return field(tag, std::string_view{value ? "1" : "0"});
    }
    CanonicalWriter& optional(std::string_view tag, const std::optional<std::string>& value) {
        if (value) {
            return field(tag, std::string_view{*value});
        }
        out_.append(tag).append("~;");
        return *this;
    }
    CanonicalWriter& optional(std::string_view tag, const std::optional<std::uint64_t>& value) {
        if (value) {
            return number(tag, *value);
        }
        out_.append(tag).append("~;");
        return *this;
    }

    [[nodiscard]] const std::string& str() const noexcept { return out_; }

private:
    std::string out_;
};

} // namespace music_service::domain::detail
