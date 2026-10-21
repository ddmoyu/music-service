#include "audio_processing/format/mp4.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <utility>
#include <vector>

namespace music_service::audio_processing::format {

namespace {

using domain::Error;
using domain::ErrorCode;

constexpr std::int64_t max_moov_bytes = 16 * 1024 * 1024;
constexpr int max_top_boxes = 4096;

Error invalid(std::string reason, std::string message, bool retryable) {
    Error error{ErrorCode::audio_invalid, std::move(message), retryable, {}};
    error.details.push_back({"audio", std::move(reason)});
    return error;
}

unsigned byte_at(std::string_view bytes, std::size_t index) { return static_cast<unsigned char>(bytes[index]); }

std::uint64_t be(std::string_view bytes, std::size_t at, std::size_t count) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < count; ++i) {
        value = (value << 8) | byte_at(bytes, at + i);
    }
    return value;
}

bool printable_type(std::string_view type) {
    for (const auto c : type) {
        if (c < 0x20 || c > 0x7E) {
            return false;
        }
    }
    return true;
}

std::optional<Error> interrupted(const application::ports::OperationContext& context) {
    if (context.stop.stop_requested()) {
        return Error{ErrorCode::canceled, "audio validation was canceled", false, {}};
    }
    if (context.expired(std::chrono::steady_clock::now())) {
        return Error{ErrorCode::deadline_exceeded, "audio validation exceeded its deadline", true, {}};
    }
    return std::nullopt;
}

struct Box {
    std::string type;
    std::size_t header{0}; // 8, or 16 with a 64-bit size
    std::size_t size{0};   // header included
};

// The child boxes of a container payload held in memory; nullopt when they do not tile it.
std::optional<std::vector<Box>> children(std::string_view payload) {
    std::vector<Box> boxes;
    std::size_t at = 0;
    while (at < payload.size()) {
        if (payload.size() - at < 8) {
            return std::nullopt;
        }
        Box box{std::string{payload.substr(at + 4, 4)}, 8, static_cast<std::size_t>(be(payload, at, 4))};
        if (box.size == 1) {
            if (payload.size() - at < 16) {
                return std::nullopt;
            }
            box.header = 16;
            box.size = static_cast<std::size_t>(be(payload, at + 8, 8));
        } else if (box.size == 0) {
            box.size = payload.size() - at;
        }
        if (box.size < box.header || box.size > payload.size() - at) {
            return std::nullopt;
        }
        boxes.push_back(box);
        at += box.size;
    }
    return boxes;
}

// The payload of the first child of this type, searching the given path of containers.
std::optional<std::string_view> find(std::string_view payload, std::initializer_list<std::string_view> path) {
    for (const auto type : path) {
        const auto boxes = children(payload);
        if (!boxes) {
            return std::nullopt;
        }
        std::size_t at = 0;
        bool found = false;
        for (const auto& box : *boxes) {
            if (box.type == type) {
                payload = payload.substr(at + box.header, box.size - box.header);
                found = true;
                break;
            }
            at += box.size;
        }
        if (!found) {
            return std::nullopt;
        }
    }
    return payload;
}

// moov/trak/mdia: the handler type and the first sample entry of each track.
void read_tracks(std::string_view moov, Mp4Structure& mp4, bool& sound) {
    const auto boxes = children(moov);
    if (!boxes) {
        return;
    }
    std::size_t at = 0;
    for (const auto& box : *boxes) {
        const auto payload = moov.substr(at + box.header, box.size - box.header);
        at += box.size;
        if (box.type != "trak") {
            continue;
        }
        const auto handler = find(payload, {"mdia", "hdlr"});
        if (!handler || handler->size() < 12 || handler->substr(8, 4) != "soun") {
            continue;
        }
        sound = true;
        // stsd: version/flags, entry count, then sample entries (size, format).
        const auto stsd = find(payload, {"mdia", "minf", "stbl", "stsd"});
        if (stsd && stsd->size() >= 16) {
            const auto format = stsd->substr(12, 4);
            if (format == "mp4a") {
                mp4.codec = "aac";
            } else if (format == "alac") {
                mp4.codec = "alac";
            } else if (format == "fLaC") {
                mp4.codec = "flac";
            } else if (format == "Opus") {
                mp4.codec = "opus";
            }
        }
        return;
    }
}

} // namespace

domain::Result<Mp4Structure> scan_mp4(ByteReader& reader, const application::ports::OperationContext& context) {
    Mp4Structure mp4;
    bool ftyp = false;
    bool mdat = false;
    std::optional<std::string> moov;
    const auto size = reader.size();
    std::int64_t offset = 0;
    for (int index = 0; offset < size; ++index) {
        if (index == max_top_boxes) {
            return invalid("damaged", "the MP4 file has too many top-level boxes", false);
        }
        if (auto stopped = interrupted(context)) {
            return std::move(*stopped);
        }
        auto head = reader.read(offset, 16);
        if (!head) {
            return std::move(head).error();
        }
        if (head->size() < 8) {
            return invalid("truncated", "an MP4 box header is cut off", true);
        }
        const auto type = head->substr(4, 4);
        if (!printable_type(type)) {
            return invalid("damaged", "an MP4 box has an invalid type", false);
        }
        std::int64_t header = 8;
        auto box_size = static_cast<std::int64_t>(be(*head, 0, 4));
        if (box_size == 1) {
            if (head->size() < 16) {
                return invalid("truncated", "an MP4 box header is cut off", true);
            }
            header = 16;
            const auto large = be(*head, 8, 8);
            if (large > static_cast<std::uint64_t>(INT64_MAX)) {
                return invalid("damaged", "an MP4 box is impossibly large", false);
            }
            box_size = static_cast<std::int64_t>(large);
        } else if (box_size == 0) {
            box_size = size - offset; // to the end of the file
        }
        if (box_size < header) {
            return invalid("damaged", "an MP4 box is smaller than its header", false);
        }
        if (box_size > size - offset) {
            return invalid("truncated", "an MP4 box runs past the end of the file", true);
        }
        if (index == 0) {
            if (type != "ftyp" || box_size < 16) {
                return invalid("damaged", "the MP4 file does not start with ftyp", false);
            }
            auto brand = reader.read(offset + header, 4);
            if (!brand) {
                return std::move(brand).error();
            }
            mp4.major_brand = *brand;
            ftyp = true;
        } else if (type == "moov") {
            if (moov) {
                return invalid("damaged", "the MP4 file has two moov boxes", false);
            }
            if (box_size - header > max_moov_bytes) {
                return invalid("damaged", "the MP4 moov box is too large", false);
            }
            std::string payload;
            for (auto at = offset + header; at < offset + box_size;) {
                auto bytes = reader.read(at, static_cast<std::size_t>(
                                                 std::min<std::int64_t>(ByteReader::window_bytes, offset + box_size - at)));
                if (!bytes) {
                    return std::move(bytes).error();
                }
                if (bytes->empty()) {
                    return invalid("truncated", "the MP4 moov box is cut off", true);
                }
                payload += *bytes;
                at += static_cast<std::int64_t>(bytes->size());
            }
            moov = std::move(payload);
        } else if (type == "mdat") {
            mdat = true;
            mp4.media_bytes += box_size - header;
        }
        offset += box_size;
    }
    if (!ftyp || !moov || !mdat) {
        return invalid("damaged", "the MP4 file lacks ftyp, moov or mdat", false);
    }
    const auto mvhd = find(*moov, {"mvhd"});
    if (!children(*moov) || !mvhd || mvhd->empty()) {
        return invalid("damaged", "the MP4 moov box is malformed", false);
    }
    // mvhd version 0: 32-bit times; version 1: 64-bit creation, modification and duration.
    const auto version = byte_at(*mvhd, 0);
    const std::size_t needed = version == 1 ? 32 : 20;
    if (mvhd->size() < needed) {
        return invalid("damaged", "the MP4 mvhd box is too short", false);
    }
    const auto timescale = be(*mvhd, version == 1 ? 20 : 12, 4);
    const auto duration = version == 1 ? be(*mvhd, 24, 8) : be(*mvhd, 16, 4);
    if (timescale > 0 && duration > 0 && duration != 0xFFFFFFFFU && duration != UINT64_MAX) {
        mp4.duration_ms = static_cast<std::int64_t>(duration * 1000 / timescale);
    }
    bool sound = false;
    read_tracks(*moov, mp4, sound);
    if (!sound) {
        return invalid("no_audio_track", "the MP4 file has no sound track", false);
    }
    return mp4;
}

} // namespace music_service::audio_processing::format
