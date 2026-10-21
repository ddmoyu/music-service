#include "domain/assets/container.hpp"

namespace music_service::domain {

namespace {

unsigned char byte_at(std::string_view head, std::size_t index) noexcept {
    return static_cast<unsigned char>(head[index]);
}

bool starts_with(std::string_view head, std::string_view magic) noexcept {
    return head.substr(0, magic.size()) == magic;
}

// An MPEG audio frame header: sync, a defined version, a defined layer, a usable bitrate
// and sample rate. Rejects the ADTS pattern, whose layer bits are zero.
bool mpeg_audio_header(std::string_view head) noexcept {
    if (head.size() < 4 || byte_at(head, 0) != 0xFF || (byte_at(head, 1) & 0xE0) != 0xE0) {
        return false;
    }
    const auto version = (byte_at(head, 1) >> 3) & 0x03;
    const auto layer = (byte_at(head, 1) >> 1) & 0x03;
    const auto bitrate = byte_at(head, 2) >> 4;
    const auto rate = (byte_at(head, 2) >> 2) & 0x03;
    return version != 1 && layer != 0 && bitrate != 0 && bitrate != 15 && rate != 3;
}

bool adts_header(std::string_view head) noexcept {
    return head.size() >= 2 && byte_at(head, 0) == 0xFF && (byte_at(head, 1) & 0xF6) == 0xF0;
}

bool printable_text(std::string_view text) noexcept {
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 && c != '\n' && c != '\r' && c != '\t') {
            return false;
        }
        if (u == 0x7F) {
            return false;
        }
    }
    return true;
}

} // namespace

ContentSniff sniff_content(std::string_view head) noexcept {
    if (head.empty()) {
        return {ContentKind::empty, std::nullopt};
    }
    const auto audio = [](Container container) { return ContentSniff{ContentKind::audio, container}; };
    if (starts_with(head, "ID3") || mpeg_audio_header(head)) {
        return audio(Container::mp3);
    }
    if (starts_with(head, "fLaC")) {
        return audio(Container::flac);
    }
    if (starts_with(head, "OggS")) {
        return audio(Container::ogg);
    }
    if (head.size() >= 8 && head.substr(4, 4) == "ftyp") {
        return audio(Container::mp4);
    }
    if (head.size() >= 12 && starts_with(head, "RIFF") && head.substr(8, 4) == "WAVE") {
        return audio(Container::wav);
    }
    if (adts_header(head)) {
        return audio(Container::aac_adts);
    }

    // Text bodies: skip a UTF-8 byte order mark and leading whitespace.
    std::string_view text = head;
    if (starts_with(text, "\xEF\xBB\xBF")) {
        text.remove_prefix(3);
    }
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {ContentKind::text, std::nullopt};
    }
    text.remove_prefix(first);
    if (text.front() == '<') {
        return {ContentKind::markup, std::nullopt};
    }
    if (text.front() == '{' || text.front() == '[') {
        return {ContentKind::json, std::nullopt};
    }
    // Multi-byte UTF-8 is not treated as text here; such bodies stay unknown.
    if (printable_text(text)) {
        return {ContentKind::text, std::nullopt};
    }
    return {ContentKind::unknown, std::nullopt};
}

std::string_view to_string(Container value) noexcept {
    switch (value) {
    case Container::mp3:
        return "mp3";
    case Container::aac_adts:
        return "aac";
    case Container::flac:
        return "flac";
    case Container::ogg:
        return "ogg";
    case Container::mp4:
        return "m4a";
    case Container::wav:
        return "wav";
    }
    return "mp3";
}

std::string_view to_string(ContentKind value) noexcept {
    switch (value) {
    case ContentKind::audio:
        return "audio";
    case ContentKind::markup:
        return "markup";
    case ContentKind::json:
        return "json";
    case ContentKind::text:
        return "text";
    case ContentKind::unknown:
        return "unknown";
    case ContentKind::empty:
        return "empty";
    }
    return "unknown";
}

} // namespace music_service::domain
