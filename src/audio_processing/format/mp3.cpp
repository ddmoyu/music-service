#include "audio_processing/format/mp3.hpp"

#include <array>
#include <chrono>
#include <utility>

namespace music_service::audio_processing::format {

namespace {

using domain::Error;
using domain::ErrorCode;

constexpr std::int64_t search_window = 64 * 1024; // how far to look for the next frame
constexpr std::uint64_t check_every = 256;         // frames between cancellation checks

Error invalid(std::string reason, std::string message, bool retryable) {
    Error error{ErrorCode::audio_invalid, std::move(message), retryable, {}};
    error.details.push_back({"audio", std::move(reason)});
    return error;
}

unsigned byte_at(const std::string& bytes, std::size_t index) {
    return static_cast<unsigned char>(bytes[index]);
}

std::uint32_t be32(const std::string& bytes, std::size_t at) {
    return (std::uint32_t{byte_at(bytes, at)} << 24) | (std::uint32_t{byte_at(bytes, at + 1)} << 16) |
           (std::uint32_t{byte_at(bytes, at + 2)} << 8) | std::uint32_t{byte_at(bytes, at + 3)};
}

std::uint32_t le32(const std::string& bytes, std::size_t at) {
    return std::uint32_t{byte_at(bytes, at)} | (std::uint32_t{byte_at(bytes, at + 1)} << 8) |
           (std::uint32_t{byte_at(bytes, at + 2)} << 16) | (std::uint32_t{byte_at(bytes, at + 3)} << 24);
}

enum class Version { mpeg1, mpeg2, mpeg25 };

struct FrameHeader {
    Version version{Version::mpeg1};
    int layer{3};
    std::int32_t bitrate_bps{0};
    std::int32_t sample_rate_hz{0};
    std::int32_t channels{2};
    std::int64_t length{0};
    std::int32_t samples{0};

    [[nodiscard]] bool same_stream(const FrameHeader& other) const noexcept {
        return version == other.version && layer == other.layer && sample_rate_hz == other.sample_rate_hz;
    }
};

// kbit/s by [mpeg1 ? 0 : 1][layer - 1][index]; index 0 (free format) and 15 are rejected.
constexpr std::array<std::array<std::array<std::int32_t, 15>, 3>, 2> bitrates{{
    {{{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
      {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},
      {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}}},
    {{{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},
      {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
      {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}}},
}};

constexpr std::array<std::int32_t, 3> mpeg1_rates{44100, 48000, 32000};

std::optional<FrameHeader> parse_header(const std::string& bytes) {
    if (bytes.size() < 4 || byte_at(bytes, 0) != 0xFF || (byte_at(bytes, 1) & 0xE0) != 0xE0) {
        return std::nullopt;
    }
    const auto version_bits = (byte_at(bytes, 1) >> 3) & 0x03;
    const auto layer_bits = (byte_at(bytes, 1) >> 1) & 0x03;
    const auto bitrate_index = byte_at(bytes, 2) >> 4;
    const auto rate_index = (byte_at(bytes, 2) >> 2) & 0x03;
    const auto padding = (byte_at(bytes, 2) >> 1) & 0x01;
    const auto mode = byte_at(bytes, 3) >> 6;
    const auto emphasis = byte_at(bytes, 3) & 0x03;
    if (version_bits == 1 || layer_bits == 0 || bitrate_index == 0 || bitrate_index == 15 || rate_index == 3 ||
        emphasis == 2) {
        return std::nullopt;
    }

    FrameHeader header;
    header.version = version_bits == 3 ? Version::mpeg1 : (version_bits == 2 ? Version::mpeg2 : Version::mpeg25);
    header.layer = 4 - static_cast<int>(layer_bits);
    const bool mpeg1 = header.version == Version::mpeg1;
    header.bitrate_bps =
        bitrates[mpeg1 ? 0 : 1][static_cast<std::size_t>(header.layer - 1)][bitrate_index] * 1000;
    header.sample_rate_hz = mpeg1_rates[rate_index] / (mpeg1 ? 1 : (header.version == Version::mpeg2 ? 2 : 4));
    header.channels = mode == 3 ? 1 : 2;

    const std::int64_t bitrate = header.bitrate_bps;
    const std::int64_t rate = header.sample_rate_hz;
    if (header.layer == 1) {
        header.samples = 384;
        header.length = (12 * bitrate / rate + padding) * 4;
    } else if (header.layer == 2 || mpeg1) {
        header.samples = 1152;
        header.length = 144 * bitrate / rate + padding;
    } else {
        header.samples = 576;
        header.length = 72 * bitrate / rate + padding;
    }
    return header;
}

std::string codec_name(const FrameHeader& header) {
    const char* version = header.version == Version::mpeg1 ? "mpeg1" : (header.version == Version::mpeg2 ? "mpeg2" : "mpeg2.5");
    return std::string{version} + "-layer" + std::to_string(header.layer);
}

class Scanner {
public:
    Scanner(ByteReader& reader, const application::ports::OperationContext& context)
        : reader_(reader), context_(context) {}

    domain::Result<Mp3Structure> run() {
        auto start = skip_leading_tags();
        if (!start) {
            return std::move(start).error();
        }
        auto end = audio_end(*start);
        if (!end) {
            return std::move(end).error();
        }
        end_ = *end;

        auto first = find_frame(*start, nullptr);
        if (!first) {
            return std::move(first).error();
        }
        if (!*first) {
            return invalid("no_audio_frames", "the file contains no MPEG audio frames", false);
        }
        auto [position, header] = **first;
        Mp3Structure result;
        result.skipped_bytes = position - *start;
        result.codec = codec_name(header);
        result.sample_rate_hz = header.sample_rate_hz;
        result.channels = header.channels;

        auto info = read_info_frame(position, header);
        if (!info) {
            return std::move(info).error();
        }
        if (info->present) {
            // A Xing/Info/VBRI header frame carries no audio.
            result.declared_frames = info->frames;
            position += header.length;
        }

        const FrameHeader reference = header;
        std::optional<std::int32_t> first_bitrate;
        std::uint64_t samples = 0;
        while (position < end_) {
            if (result.frames % check_every == 0) {
                if (auto stopped = interrupted()) {
                    return std::move(*stopped);
                }
            }
            auto bytes = reader_.read(position, 4);
            if (!bytes) {
                return std::move(bytes).error();
            }
            const auto parsed = parse_header(*bytes);
            if (parsed && parsed->same_stream(reference)) {
                if (position + parsed->length > end_) {
                    return invalid("truncated", "the last audio frame is cut off", true);
                }
                ++result.frames;
                result.audio_bytes += parsed->length;
                samples += static_cast<std::uint64_t>(parsed->samples);
                if (!first_bitrate) {
                    first_bitrate = parsed->bitrate_bps;
                } else if (*first_bitrate != parsed->bitrate_bps) {
                    result.variable_bitrate = true;
                }
                position += parsed->length;
                continue;
            }
            if (bytes->size() < 4 && parsed_prefix(*bytes)) {
                return invalid("truncated", "the last audio frame is cut off", true);
            }
            auto next = find_frame(position + 1, &reference);
            if (!next) {
                return std::move(next).error();
            }
            if (!*next) {
                result.skipped_bytes += end_ - position; // trailing bytes that are not frames
                break;
            }
            result.skipped_bytes += (*next)->first - position;
            position = (*next)->first;
        }

        if (result.frames == 0) {
            return invalid("no_audio_frames", "the file contains no MPEG audio frames", false);
        }
        // More than 1% (and over 4 KiB) of the audio region is damaged: not a usable file.
        const auto region = result.audio_bytes + result.skipped_bytes;
        if (result.skipped_bytes > 4096 && result.skipped_bytes * 100 > region) {
            return invalid("damaged", "too much of the file is not audio frames", true);
        }
        if (result.declared_frames && result.frames + 2 < *result.declared_frames) {
            return invalid("truncated", "fewer audio frames than the file header declares", true);
        }
        result.duration_ms =
            static_cast<std::int64_t>(samples * 1000 / static_cast<std::uint64_t>(reference.sample_rate_hz));
        if (result.duration_ms > 0) {
            result.bitrate_bps = result.audio_bytes * 8 * 1000 / result.duration_ms;
        }
        return result;
    }

private:
    // A cut-off frame header at the very end still starts with the sync bits.
    static bool parsed_prefix(const std::string& bytes) {
        return !bytes.empty() && byte_at(bytes, 0) == 0xFF;
    }

    std::optional<Error> interrupted() const {
        if (context_.stop.stop_requested()) {
            return Error{ErrorCode::canceled, "audio validation was canceled", false, {}};
        }
        if (context_.expired(std::chrono::steady_clock::now())) {
            return Error{ErrorCode::deadline_exceeded, "audio validation exceeded its deadline", true, {}};
        }
        return std::nullopt;
    }

    // ID3v2 tags in front, possibly several; returns where the audio region starts.
    domain::Result<std::int64_t> skip_leading_tags() {
        std::int64_t position = 0;
        for (;;) {
            auto bytes = reader_.read(position, 10);
            if (!bytes) {
                return std::move(bytes).error();
            }
            if (bytes->substr(0, 3) != "ID3") {
                return position;
            }
            if (bytes->size() < 10) {
                return invalid("truncated", "the ID3v2 tag is cut off", true);
            }
            std::int64_t size = 0;
            for (std::size_t i = 6; i < 10; ++i) {
                if (byte_at(*bytes, i) >= 0x80) {
                    return invalid("damaged", "the ID3v2 tag size is malformed", true);
                }
                size = (size << 7) | byte_at(*bytes, i);
            }
            const bool footer = (byte_at(*bytes, 5) & 0x10) != 0;
            const auto next = position + 10 + size + (footer ? 10 : 0);
            if (next > reader_.size()) {
                return invalid("truncated", "the ID3v2 tag is cut off", true);
            }
            position = next;
        }
    }

    // Removes ID3v1, APEv2 and Lyrics3v2 tags from the end; returns where audio ends.
    domain::Result<std::int64_t> audio_end(std::int64_t start) {
        std::int64_t end = reader_.size();
        for (bool changed = true; changed;) {
            changed = false;
            if (end - start >= 128) {
                auto tag = reader_.read(end - 128, 3);
                if (!tag) {
                    return std::move(tag).error();
                }
                if (*tag == "TAG") {
                    end -= 128;
                    changed = true;
                    continue;
                }
            }
            if (end - start >= 32) {
                auto footer = reader_.read(end - 32, 32);
                if (!footer) {
                    return std::move(footer).error();
                }
                if (footer->substr(0, 8) == "APETAGEX") {
                    const std::int64_t size = le32(*footer, 12);
                    const bool has_header = (le32(*footer, 20) & 0x80000000U) != 0;
                    const auto total = size + (has_header ? 32 : 0);
                    if (size >= 32 && total <= end - start) {
                        end -= total;
                        changed = true;
                        continue;
                    }
                }
            }
            if (end - start >= 15) {
                auto lyrics = reader_.read(end - 15, 15);
                if (!lyrics) {
                    return std::move(lyrics).error();
                }
                if (lyrics->substr(6) == "LYRICS200") {
                    std::int64_t size = 0;
                    bool digits = true;
                    for (std::size_t i = 0; i < 6; ++i) {
                        const char c = (*lyrics)[i];
                        digits = digits && c >= '0' && c <= '9';
                        size = size * 10 + (c - '0');
                    }
                    if (digits && size + 15 <= end - start) {
                        end -= size + 15;
                        changed = true;
                    }
                }
            }
        }
        return end;
    }

    // The first frame at or after from (within the search window) that starts a chain:
    // its successor is also a frame of the same stream, or it ends exactly at the end.
    // Without a reference the first frame needs two consistent successors.
    using Found = std::optional<std::pair<std::int64_t, FrameHeader>>;
    domain::Result<Found> find_frame(std::int64_t from, const FrameHeader* reference) {
        const auto limit = std::min(end_, from + search_window);
        for (auto position = from; position + 4 <= limit; ++position) {
            auto bytes = reader_.read(position, 4);
            if (!bytes) {
                return std::move(bytes).error();
            }
            if (byte_at(*bytes, 0) != 0xFF) {
                continue;
            }
            const auto header = parse_header(*bytes);
            if (!header || (reference && !header->same_stream(*reference))) {
                continue;
            }
            auto chained = chain(position, *header, reference ? 1 : 2);
            if (!chained) {
                return std::move(chained).error();
            }
            if (*chained) {
                return Found{std::pair{position, *header}};
            }
        }
        return Found{};
    }

    domain::Result<bool> chain(std::int64_t position, FrameHeader header, int successors) {
        for (int i = 0; i < successors; ++i) {
            position += header.length;
            if (position == end_) {
                return true;
            }
            auto bytes = reader_.read(position, 4);
            if (!bytes) {
                return std::move(bytes).error();
            }
            const auto next = parse_header(*bytes);
            if (!next || !next->same_stream(header)) {
                return false;
            }
            header = *next;
        }
        return true;
    }

    struct InfoFrame {
        bool present{false};
        std::optional<std::uint64_t> frames;
    };

    // A Xing/Info or VBRI header in the first frame and the frame count it declares.
    domain::Result<InfoFrame> read_info_frame(std::int64_t position, const FrameHeader& header) {
        auto bytes = reader_.read(position, static_cast<std::size_t>(std::min<std::int64_t>(header.length, 64)));
        if (!bytes) {
            return std::move(bytes).error();
        }
        const bool mpeg1 = header.version == Version::mpeg1;
        const std::size_t side_info = mpeg1 ? (header.channels == 1 ? 17 : 32) : (header.channels == 1 ? 9 : 17);
        const std::size_t xing = 4 + side_info;
        if (header.layer == 3 && bytes->size() >= xing + 8) {
            const auto tag = bytes->substr(xing, 4);
            if (tag == "Xing" || tag == "Info") {
                const auto flags = be32(*bytes, xing + 4);
                if ((flags & 0x01U) != 0 && bytes->size() >= xing + 12) {
                    return InfoFrame{true, be32(*bytes, xing + 8)};
                }
                return InfoFrame{true, std::nullopt};
            }
        }
        constexpr std::size_t vbri = 36;
        if (bytes->size() >= vbri + 18 && bytes->substr(vbri, 4) == "VBRI") {
            return InfoFrame{true, be32(*bytes, vbri + 14)};
        }
        return InfoFrame{};
    }

    ByteReader& reader_;
    const application::ports::OperationContext& context_;
    std::int64_t end_{0};
};

} // namespace

domain::Result<Mp3Structure> scan_mp3(ByteReader& reader, const application::ports::OperationContext& context) {
    return Scanner{reader, context}.run();
}

} // namespace music_service::audio_processing::format
