#include "infrastructure/filesystem/paths.hpp"

#include "domain/catalog/refs.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>

namespace music_service::infrastructure::filesystem {

namespace fs = std::filesystem;

namespace {

bool is_forbidden_char(unsigned char c) noexcept {
    if (c < 0x20 || c == 0x7F) {
        return true;
    }
    switch (c) {
    case '<':
    case '>':
    case ':':
    case '"':
    case '/':
    case '\\':
    case '|':
    case '?':
    case '*':
        return true;
    default:
        return false;
    }
}

char ascii_upper(char c) noexcept {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

// CON, PRN, AUX, NUL, COM1-9, LPT1-9, with or without an extension.
bool is_reserved_name(std::string_view component) noexcept {
    const auto stem = component.substr(0, component.find('.'));
    std::string upper;
    for (const char c : stem) {
        upper.push_back(ascii_upper(c));
    }
    while (!upper.empty() && upper.back() == ' ') {
        upper.pop_back();
    }
    static constexpr std::array<std::string_view, 4> fixed{"CON", "PRN", "AUX", "NUL"};
    if (std::find(fixed.begin(), fixed.end(), upper) != fixed.end()) {
        return true;
    }
    return upper.size() == 4 && (upper.rfind("COM", 0) == 0 || upper.rfind("LPT", 0) == 0) &&
           upper[3] >= '1' && upper[3] <= '9';
}

// Length of the longest prefix of text, at most max_bytes, that ends on a UTF-8 boundary.
std::size_t utf8_prefix(std::string_view text, std::size_t max_bytes) noexcept {
    if (text.size() <= max_bytes) {
        return text.size();
    }
    std::size_t end = max_bytes;
    while (end > 0 && (static_cast<std::uint8_t>(text[end]) & 0xC0U) == 0x80U) {
        --end;
    }
    return end;
}

} // namespace

fs::path path_from_utf8(std::string_view text) {
    return fs::path{std::u8string{text.begin(), text.end()}};
}

std::string path_to_utf8(const fs::path& path) {
    const auto u8 = path.u8string();
    return std::string{u8.begin(), u8.end()};
}

domain::Error file_error(std::string_view operation, const fs::path& path,
                         const std::error_code& ec, domain::ErrorCode code) {
    std::string message{operation};
    message.append(" failed for '").append(path_to_utf8(path)).append("'");
    if (ec) {
        message.append(": ")
            .append(ec.category().name())
            .append(" error ")
            .append(std::to_string(ec.value()));
    }
    return domain::Error{code, std::move(message), false, {}};
}

domain::Result<fs::path> canonical_directory(const fs::path& path) {
    std::error_code ec;
    const auto absolute = fs::absolute(path, ec);
    if (ec) {
        return file_error("resolve directory", path, ec);
    }
    const auto status = fs::status(absolute, ec);
    if (ec || !fs::is_directory(status)) {
        return file_error("open directory", absolute,
                          ec ? ec : std::make_error_code(std::errc::not_a_directory),
                          domain::ErrorCode::destination_unavailable);
    }
    auto canonical = fs::canonical(absolute, ec);
    if (ec) {
        return file_error("resolve directory", absolute, ec);
    }
    return canonical;
}

bool is_within(const fs::path& root, const fs::path& candidate) {
    const auto normal_root = root.lexically_normal();
    const auto normal_candidate = candidate.lexically_normal();
    auto r = normal_root.begin();
    auto c = normal_candidate.begin();
    for (; r != normal_root.end(); ++r, ++c) {
        if (r->empty() && std::next(r) == normal_root.end()) {
            break; // trailing separator on the root
        }
        if (c == normal_candidate.end() || *r != *c) {
            return false;
        }
    }
    return true;
}

domain::Result<void> check_relative_path(std::string_view relative) {
    if (relative.empty()) {
        return domain::Error::validation("path", "must not be empty");
    }
    if (!domain::is_valid_utf8(relative)) {
        return domain::Error::validation("path", "must be valid UTF-8");
    }
    if (relative.front() == '/') {
        return domain::Error::validation("path", "must be relative");
    }
    std::size_t start = 0;
    while (start <= relative.size()) {
        const auto end = relative.find('/', start);
        const auto component =
            relative.substr(start, end == std::string_view::npos ? std::string_view::npos
                                                                  : end - start);
        if (component.empty() || component == "." || component == "..") {
            return domain::Error::validation("path", "contains an empty, '.' or '..' component");
        }
        for (const char c : component) {
            if (is_forbidden_char(static_cast<unsigned char>(c))) {
                return domain::Error::validation("path", "contains a forbidden character");
            }
        }
        if (component.back() == '.' || component.back() == ' ' || component.front() == ' ') {
            return domain::Error::validation("path", "component starts with a space or ends "
                                                     "with a dot or space");
        }
        if (is_reserved_name(component)) {
            return domain::Error::validation("path", "uses a reserved device name");
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return domain::ok();
}

domain::Result<fs::path> resolve_within(const fs::path& canonical_root, std::string_view relative) {
    if (auto checked = check_relative_path(relative); !checked) {
        return checked.error();
    }
    const auto target = (canonical_root / path_from_utf8(relative)).lexically_normal();
    if (!is_within(canonical_root, target)) {
        return domain::Error::validation("path", "escapes the root directory");
    }

    // Resolve the deepest existing ancestor: a symlink or junction inside the root may
    // point anywhere.
    std::error_code ec;
    auto existing = target;
    while (!existing.empty() && !fs::exists(fs::symlink_status(existing, ec)) &&
           existing != canonical_root) {
        existing = existing.parent_path();
    }
    const auto resolved = fs::canonical(existing, ec);
    if (ec) {
        return file_error("resolve path", existing, ec);
    }
    if (!is_within(canonical_root, resolved)) {
        return domain::Error::validation("path", "resolves outside the root directory");
    }
    return target;
}

std::string sanitize_file_name(std::string_view text, std::size_t max_bytes) {
    std::string result;
    result.reserve(text.size());
    for (const char c : text) {
        result.push_back(is_forbidden_char(static_cast<unsigned char>(c)) ? '_' : c);
    }
    if (!domain::is_valid_utf8(result)) {
        // Replace the whole name rather than writing a broken byte sequence to disk.
        result.assign("_");
    }

    const auto first = result.find_first_not_of(' ');
    result.erase(0, first == std::string::npos ? result.size() : first);
    result.resize(utf8_prefix(result, max_bytes == 0 ? 1 : max_bytes));
    while (!result.empty() && (result.back() == ' ' || result.back() == '.')) {
        result.pop_back();
    }
    if (result.empty()) {
        return "_";
    }
    if (is_reserved_name(result)) {
        result.insert(result.begin(), '_');
        result.resize(utf8_prefix(result, std::max<std::size_t>(max_bytes, 2)));
    }
    return result;
}

} // namespace music_service::infrastructure::filesystem
