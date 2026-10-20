#include "domain/catalog/refs.hpp"

#include <cstdint>

namespace music_dl::domain {

namespace {

bool is_lower(char c) noexcept { return c >= 'a' && c <= 'z'; }
bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }
bool is_alnum(char c) noexcept {
    return is_lower(c) || is_digit(c) || (c >= 'A' && c <= 'Z');
}

} // namespace

Result<SourceId> SourceId::parse(std::string_view text) {
    if (text.empty() || text.size() > 64 || !is_lower(text.front())) {
        return Error::validation("source", "must match [a-z][a-z0-9_]{0,63}");
    }
    for (const char c : text) {
        if (!is_lower(c) && !is_digit(c) && c != '_') {
            return Error::validation("source", "must match [a-z][a-z0-9_]{0,63}");
        }
    }
    return SourceId{std::string{text}};
}

Result<PlatformId> PlatformId::parse(std::string_view text) {
    if (text.empty() || text.size() > max_bytes) {
        return Error::validation("id", "must be 1 to 1024 bytes");
    }
    if (!is_valid_utf8(text)) {
        return Error::validation("id", "must be valid UTF-8");
    }
    return PlatformId{std::string{text}};
}

bool is_valid_service_id(std::string_view prefix, std::string_view id) noexcept {
    if (prefix.empty() || id.size() <= prefix.size() || id.size() > prefix.size() + 60 ||
        id.substr(0, prefix.size()) != prefix) {
        return false;
    }
    for (const char c : id.substr(prefix.size())) {
        if (!is_alnum(c)) {
            return false;
        }
    }
    return true;
}

bool is_valid_utf8(std::string_view text) noexcept {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<std::uint8_t>(text[i]);
        std::size_t length = 0;
        std::uint32_t code_point = 0;
        if (lead < 0x80) {
            ++i;
            continue;
        }
        if ((lead & 0xE0U) == 0xC0U) {
            length = 2;
            code_point = lead & 0x1FU;
        } else if ((lead & 0xF0U) == 0xE0U) {
            length = 3;
            code_point = lead & 0x0FU;
        } else if ((lead & 0xF8U) == 0xF0U) {
            length = 4;
            code_point = lead & 0x07U;
        } else {
            return false;
        }
        if (i + length > text.size()) {
            return false;
        }
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<std::uint8_t>(text[i + k]);
            if ((next & 0xC0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (next & 0x3FU);
        }
        // Reject overlong encodings, UTF-16 surrogates and values beyond U+10FFFF.
        const bool overlong = (length == 2 && code_point < 0x80) ||
                              (length == 3 && code_point < 0x800) ||
                              (length == 4 && code_point < 0x10000);
        if (overlong || (code_point >= 0xD800 && code_point <= 0xDFFF) || code_point > 0x10FFFF) {
            return false;
        }
        i += length;
    }
    return true;
}

} // namespace music_dl::domain
