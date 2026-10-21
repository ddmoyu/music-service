#include "audio_processing/verifying_processor.hpp"
#include "domain/assets/quality.hpp"
#include "infrastructure/config/config_loader.hpp"
#include "infrastructure/filesystem/paths.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <filesystem>
#include <fstream>
#include <stop_token>
#include <string>

using namespace music_service;
namespace ports = application::ports;
namespace fs = std::filesystem;
using audio_processing::ProcessorCatalog;
using audio_processing::VerifyingProcessor;

namespace {

class TempDir {
public:
    explicit TempDir(std::string_view name)
        : path_(fs::temp_directory_path() /
                infrastructure::filesystem::path_from_utf8("music-service-音频-" + std::string{name})) {
        fs::remove_all(path_);
        fs::create_directories(path_);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    [[nodiscard]] fs::path file(std::string_view name, const std::string& bytes) const {
        const auto path = path_ / infrastructure::filesystem::path_from_utf8(std::string{name});
        std::ofstream out(path, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return path;
    }

private:
    fs::path path_;
};

// MPEG-1 layer III, 44.1 kHz, joint stereo; 128 kbit/s frames are 417 bytes (418 padded).
std::string frame(unsigned bitrate_index = 9, bool padding = false) {
    static constexpr int kbps[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
    const auto length = static_cast<std::size_t>(144 * kbps[bitrate_index] * 1000 / 44100 + (padding ? 1 : 0));
    std::string bytes(length, '\x55');
    bytes[0] = '\xFF';
    bytes[1] = '\xFB';
    bytes[2] = static_cast<char>((bitrate_index << 4) | (padding ? 0x02U : 0U));
    bytes[3] = '\x40';
    return bytes;
}

std::string frames(std::size_t count) {
    std::string bytes;
    for (std::size_t i = 0; i < count; ++i) {
        bytes += frame(9, i % 3 == 0);
    }
    return bytes;
}

// 26.122 ms per frame: 1149 frames are 30.01 s.
constexpr std::size_t thirty_seconds = 1149;

std::string id3v2(std::size_t payload) {
    std::string tag{"ID3\x04\x00\x00", 6};
    for (int shift = 21; shift >= 0; shift -= 7) {
        tag += static_cast<char>((payload >> shift) & 0x7F);
    }
    return tag + std::string(payload, '\0');
}

// An ID3v2.4 tag with a footer, as allowed at the end of the file.
std::string id3v24_appended(std::size_t payload) {
    auto tag = id3v2(payload);
    tag[5] = '\x10'; // footer present
    return tag + "3DI" + tag.substr(3, 7);
}

std::string id3v1() { return "TAG" + std::string(125, ' '); }

// A first frame carrying a Xing header that declares frame_count frames.
std::string xing_frame(std::uint32_t frame_count) {
    auto bytes = frame();
    bytes.replace(36, 4, "Xing");
    const char flags[4] = {0, 0, 0, 1};
    bytes.replace(40, 4, std::string{flags, 4});
    for (int i = 0; i < 4; ++i) {
        bytes[static_cast<std::size_t>(44 + i)] = static_cast<char>((frame_count >> (24 - 8 * i)) & 0xFF);
    }
    return bytes;
}

ports::OperationContext context() { return {std::chrono::steady_clock::now() + std::chrono::minutes(1), {}}; }

ports::ProcessingRequest request(fs::path input) {
    ports::ProcessingRequest value;
    value.input = std::move(input);
    value.expected.container = "mp3";
    value.expected.bitrate_bps = 128000;
    return value;
}

bool has_warning(const ports::ProcessingReport& report, std::string_view code) {
    return std::find(report.warnings.begin(), report.warnings.end(), code) != report.warnings.end();
}

std::string reason(const domain::Error& error) { return error.details.empty() ? "" : error.details.front().reason; }

} // namespace

TEST_CASE("a tagged CBR MP3 is measured from its frames", "[audio][mp3]") {
    TempDir dir{"cbr"};
    VerifyingProcessor processor;
    auto input = request(dir.file("曲目.mp3", id3v2(2048) + frames(thirty_seconds) + id3v1()));
    input.expected.duration_ms = 30'000;
    input.expected.completeness = domain::Completeness::unknown;

    auto report = processor.process(input, context());
    REQUIRE(report);
    CHECK(report->processor_id == "audio.verify");
    CHECK(report->processor_version == 2);
    CHECK(report->output == input.input); // original encoding kept, nothing rewritten
    CHECK(report->facts.container == "mp3");
    CHECK(report->facts.codec == "mpeg1-layer3");
    CHECK(report->facts.sample_rate_hz == 44100);
    CHECK(report->facts.channels == 2);
    CHECK(report->facts.frames == thirty_seconds);
    CHECK(report->facts.duration_ms == 30'014);
    CHECK_FALSE(report->facts.variable_bitrate);
    CHECK(std::llabs(*report->facts.bitrate_bps - 128000) < 500);
    CHECK(report->facts.size_bytes == static_cast<std::int64_t>(fs::file_size(input.input)));
    // The checked bytes are named by their SHA-256: equal bytes, equal hash; others differ.
    CHECK(report->facts.sha256.size() == 64);
    CHECK(processor.process(input, context())->facts.sha256 == report->facts.sha256);
    auto other = request(dir.file("other.mp3", id3v2(2048) + frames(thirty_seconds + 1) + id3v1()));
    CHECK(processor.process(other, context())->facts.sha256 != report->facts.sha256);
    // A matching length is full-length evidence even when resolve could not tell.
    CHECK(report->completeness == domain::Completeness::full);
    CHECK(has_warning(*report, ports::processing_warning::metadata_not_written));
    CHECK_FALSE(has_warning(*report, ports::processing_warning::bitrate_mismatch));
    REQUIRE(report->stages.size() == 3);
    CHECK(report->stages[1].stage == "validate");
    CHECK(report->stages[1].outcome == ports::StageOutcome::done);
    CHECK(report->stages[2].outcome == ports::StageOutcome::skipped);
}

TEST_CASE("an unknown length stays unknown and a short clip is a preview", "[audio][mp3]") {
    TempDir dir{"length"};
    VerifyingProcessor processor;
    const auto path = dir.file("clip.mp3", frames(thirty_seconds));

    auto unknown = request(path);
    auto report = processor.process(unknown, context());
    REQUIRE(report);
    CHECK(report->completeness == domain::Completeness::unknown); // nothing to compare with
    CHECK(report->facts.duration_ms == 30'014);                    // measured, not estimated

    // Resolve claimed the whole 4-minute song; the bytes are a 30-second clip.
    auto claimed = request(path);
    claimed.expected.duration_ms = 240'000;
    claimed.expected.completeness = domain::Completeness::full;
    report = processor.process(claimed, context());
    REQUIRE(report);
    CHECK(report->completeness == domain::Completeness::preview);
    CHECK(has_warning(*report, ports::processing_warning::duration_shorter));
    // The download decision then rejects it unless previews are allowed.
    const auto verdict = domain::evaluate_resource({}, {domain::Quality::standard, report->completeness});
    REQUIRE_FALSE(verdict);
    CHECK(verdict.error().code == domain::ErrorCode::preview_rejected);

    // A known preview is never upgraded by a matching length.
    auto preview = request(path);
    preview.expected.duration_ms = 30'000;
    preview.expected.completeness = domain::Completeness::preview;
    report = processor.process(preview, context());
    REQUIRE(report);
    CHECK(report->completeness == domain::Completeness::preview);

    auto longer = request(path);
    longer.expected.duration_ms = 10'000;
    report = processor.process(longer, context());
    REQUIRE(report);
    CHECK(has_warning(*report, ports::processing_warning::duration_mismatch));
    CHECK(report->completeness == domain::Completeness::unknown);
}

TEST_CASE("HTML, JSON, text and empty bodies are not published as audio", "[audio][invalid]") {
    TempDir dir{"bodies"};
    VerifyingProcessor processor;
    const std::pair<std::string, std::string> cases[] = {
        {"<!DOCTYPE html><html><body>403 Forbidden</body></html>", "not_audio:markup"},
        {"{\"code\":-1,\"msg\":\"no permission\"}", "not_audio:json"},
        {"Service Unavailable", "not_audio:text"},
        {"", "empty"},
    };
    for (const auto& [body, expected] : cases) {
        auto result = processor.process(request(dir.file("body.mp3", body)), context());
        REQUIRE_FALSE(result);
        CHECK(result.error().code == domain::ErrorCode::audio_invalid);
        CHECK(reason(result.error()) == expected);
        CHECK(result.error().retryable); // an upstream error page may be transient
    }
}

TEST_CASE("unsupported or unrecognized containers fail", "[audio][invalid]") {
    TempDir dir{"containers"};
    VerifyingProcessor processor;
    auto ogg = processor.process(request(dir.file("a.mp3", "OggS" + std::string(64, '\0'))), context());
    REQUIRE_FALSE(ogg);
    CHECK(reason(ogg.error()) == "unsupported_container:ogg");
    CHECK_FALSE(ogg.error().retryable);
    // A FLAC marker without a STREAMINFO block is not a FLAC stream.
    auto flac = processor.process(request(dir.file("a.flac", "fLaC" + std::string(64, '\0'))), context());
    REQUIRE_FALSE(flac);
    CHECK(reason(flac.error()) == "damaged");

    auto binary = processor.process(request(dir.file("b.mp3", std::string(4096, '\x01'))), context());
    REQUIRE_FALSE(binary);
    CHECK(reason(binary.error()) == "unrecognized");

    // An ID3 tag with nothing but zeros behind it holds no audio.
    auto tag_only = processor.process(request(dir.file("c.mp3", id3v2(100) + std::string(5000, '\0'))), context());
    REQUIRE_FALSE(tag_only);
    CHECK(reason(tag_only.error()) == "no_audio_frames");
}

TEST_CASE("cut-off files are detected as truncated", "[audio][truncated]") {
    TempDir dir{"truncated"};
    VerifyingProcessor processor;
    const auto full = id3v2(512) + frames(400);

    SECTION("the last frame is cut") {
        auto result = processor.process(request(dir.file("cut.mp3", full.substr(0, full.size() - 100))), context());
        REQUIRE_FALSE(result);
        CHECK(reason(result.error()) == "truncated");
        CHECK(result.error().retryable);
    }
    SECTION("the first or second frame is cut") {
        for (const auto& cut : {frame().substr(0, 200), frame() + frame().substr(0, 200), frame() + frame().substr(0, 2),
                                id3v2(512) + frame().substr(0, 300)}) {
            auto result = processor.process(request(dir.file("start.mp3", cut)), context());
            REQUIRE_FALSE(result);
            CHECK(reason(result.error()) == "truncated");
            CHECK(result.error().retryable);
        }
    }
    SECTION("the ID3v2 tag is cut") {
        auto result = processor.process(request(dir.file("tag.mp3", full.substr(0, 300))), context());
        REQUIRE_FALSE(result);
        CHECK(reason(result.error()) == "truncated");
    }
    SECTION("fewer bytes than the transfer announced, even at a frame boundary") {
        const auto cut = id3v2(512) + frames(200);
        auto input = request(dir.file("short.mp3", cut));
        input.expected.size_bytes = static_cast<std::int64_t>(full.size());
        auto result = processor.process(input, context());
        REQUIRE_FALSE(result);
        CHECK(reason(result.error()) == "truncated");

        input.expected.size_bytes = static_cast<std::int64_t>(cut.size() - 1);
        result = processor.process(input, context());
        REQUIRE_FALSE(result);
        CHECK(reason(result.error()) == "size_mismatch");

        input.expected.size_bytes = static_cast<std::int64_t>(cut.size());
        CHECK(processor.process(input, context()));
    }
    SECTION("fewer frames than the Xing header declares") {
        auto result = processor.process(request(dir.file("xing.mp3", xing_frame(400) + frames(200))), context());
        REQUIRE_FALSE(result);
        CHECK(reason(result.error()) == "truncated");

        auto whole = processor.process(request(dir.file("xing-ok.mp3", xing_frame(400) + frames(400))), context());
        REQUIRE(whole);
        CHECK(whole->facts.frames == 400); // the header frame is not audio
    }
}

TEST_CASE("an ID3v2.4 tag at the end is not damage", "[audio][mp3]") {
    TempDir dir{"appended"};
    VerifyingProcessor processor;
    auto report = processor.process(request(dir.file("tail.mp3", frames(500) + id3v24_appended(8000))), context());
    REQUIRE(report);
    CHECK(report->facts.frames == 500);
    CHECK_FALSE(has_warning(*report, ports::processing_warning::damaged_data_skipped));

    // A footer whose header does not match is not a tag.
    auto forged = id3v24_appended(8000);
    forged[9] = '\x01';
    auto result = processor.process(request(dir.file("forged.mp3", frames(500) + forged)), context());
    REQUIRE_FALSE(result);
    CHECK(reason(result.error()) == "damaged");
}

TEST_CASE("damaged data is skipped only while it stays small", "[audio][damaged]") {
    TempDir dir{"damaged"};
    VerifyingProcessor processor;
    const auto small = frames(500) + std::string(300, '\x01') + frames(500);
    auto report = processor.process(request(dir.file("small.mp3", small)), context());
    REQUIRE(report);
    CHECK(report->facts.frames == 1000);
    CHECK(has_warning(*report, ports::processing_warning::damaged_data_skipped));

    const auto large = frames(300) + std::string(20'000, '\x01') + frames(300);
    auto result = processor.process(request(dir.file("large.mp3", large)), context());
    REQUIRE_FALSE(result);
    CHECK(reason(result.error()) == "damaged");

    // Garbage that never becomes frames again is trailing damage, judged the same way.
    const auto trailing = frames(300) + std::string(30'000, '\x01');
    result = processor.process(request(dir.file("trailing.mp3", trailing)), context());
    REQUIRE_FALSE(result);
    CHECK(reason(result.error()) == "damaged");
}

TEST_CASE("bitrate, VBR, MPEG-2 and container evidence are reported", "[audio][mp3]") {
    TempDir dir{"facts"};
    VerifyingProcessor processor;

    std::string vbr;
    for (int i = 0; i < 300; ++i) {
        vbr += frame(i % 2 == 0 ? 9U : 14U);
    }
    auto report = processor.process(request(dir.file("vbr.mp3", vbr)), context());
    REQUIRE(report);
    CHECK(report->facts.variable_bitrate);
    CHECK_FALSE(has_warning(*report, ports::processing_warning::bitrate_mismatch));

    std::string low;
    for (int i = 0; i < 300; ++i) {
        low += frame(5); // 64 kbit/s while 128 kbit/s was declared
    }
    auto input = request(dir.file("low.mp3", low));
    input.expected.container = "aac";
    report = processor.process(input, context());
    REQUIRE(report);
    CHECK(has_warning(*report, ports::processing_warning::bitrate_mismatch));
    CHECK(has_warning(*report, ports::processing_warning::container_mismatch));

    // MPEG-2 layer III, 22.05 kHz, 64 kbit/s: 208-byte frames of 576 samples.
    std::string mpeg2;
    for (int i = 0; i < 400; ++i) {
        std::string bytes(208, '\x33');
        bytes.replace(0, 4, std::string{"\xFF\xF3\x80\xC0", 4});
        mpeg2 += bytes;
    }
    input = request(dir.file("mpeg2.mp3", mpeg2));
    input.expected.bitrate_bps = std::nullopt;
    report = processor.process(input, context());
    REQUIRE(report);
    CHECK(report->facts.codec == "mpeg2-layer3");
    CHECK(report->facts.sample_rate_hz == 22050);
    CHECK(report->facts.channels == 1);
    CHECK(report->facts.duration_ms == 400 * 576 * 1000 / 22050);
}

TEST_CASE("validation stops on cancellation and the deadline", "[audio][cancel]") {
    TempDir dir{"cancel"};
    VerifyingProcessor processor;
    const auto path = dir.file("song.mp3", frames(2000));

    std::stop_source stop;
    stop.request_stop();
    auto canceled = processor.process(request(path), {std::chrono::steady_clock::now() + std::chrono::minutes(1), stop.get_token()});
    REQUIRE_FALSE(canceled);
    CHECK(canceled.error().code == domain::ErrorCode::canceled);

    auto late = processor.process(request(path), {std::chrono::steady_clock::now() - std::chrono::seconds(1), {}});
    REQUIRE_FALSE(late);
    CHECK(late.error().code == domain::ErrorCode::deadline_exceeded);

    // Long runs of tags are checked too: no frame loop is reached to notice it later.
    std::string tags;
    for (int i = 0; i < 100'000; ++i) {
        tags += id3v2(0);
    }
    const auto tagged = dir.file("tags.mp3", tags + std::string(5000, '\0'));
    canceled = processor.process(request(tagged), {std::chrono::steady_clock::now() + std::chrono::minutes(1), stop.get_token()});
    REQUIRE_FALSE(canceled);
    CHECK(canceled.error().code == domain::ErrorCode::canceled);
    late = processor.process(request(tagged), {std::chrono::steady_clock::now() - std::chrono::seconds(1), {}});
    REQUIRE_FALSE(late);
    CHECK(late.error().code == domain::ErrorCode::deadline_exceeded);
}

TEST_CASE("without trusted metadata nothing is written", "[audio][metadata]") {
    TempDir dir{"metadata"};
    VerifyingProcessor processor;
    auto input = request(dir.file("song.mp3", frames(100)));

    input.metadata_policy = domain::MetadataPolicy::skip;
    auto report = processor.process(input, context());
    REQUIRE(report);
    CHECK(report->warnings.empty());
    CHECK(report->output == input.input);

    input.metadata_policy = domain::MetadataPolicy::best_effort;
    report = processor.process(input, context());
    REQUIRE(report);
    CHECK(has_warning(*report, ports::processing_warning::metadata_not_written));
    CHECK(report->output == input.input);

    input.metadata_policy = domain::MetadataPolicy::required;
    auto required = processor.process(input, context());
    REQUIRE_FALSE(required);
    CHECK(required.error().code == domain::ErrorCode::metadata_failed);
    CHECK(reason(required.error()) == "no_metadata");
}

TEST_CASE("processors are found by id and version only", "[audio][catalog]") {
    const auto catalog = ProcessorCatalog::builtin();
    auto found = catalog.find(ports::ProcessingPlan{});
    REQUIRE(found);
    CHECK((*found)->plan().processor_id == "audio.verify");

    CHECK((*found)->plan().processor_version == 2);
    auto unknown = catalog.find(ports::ProcessingPlan{"audio.verify", 1});
    REQUIRE_FALSE(unknown);
    CHECK(unknown.error().code == domain::ErrorCode::capability_unsupported);
    // A required decryption no decryptor provides is never skipped.
    auto encrypted = catalog.find(ports::ProcessingPlan{"audio.verify", 2, "platform.cipher"});
    REQUIRE_FALSE(encrypted);
    CHECK(encrypted.error().code == domain::ErrorCode::capability_unsupported);

    auto duplicate = ProcessorCatalog::create({std::make_shared<VerifyingProcessor>(), std::make_shared<VerifyingProcessor>()});
    CHECK_FALSE(duplicate);
}

// Opt-in check of a real download: MUSIC_SERVICE_AUDIO_SAMPLE names an MP3 file and
// MUSIC_SERVICE_AUDIO_SAMPLE_MS optionally its declared length. Results go to the T15 record.
TEST_CASE("a real downloaded MP3 validates", "[audio][online-audio]") {
    const auto sample = infrastructure::config::process_environment("MUSIC_SERVICE_AUDIO_SAMPLE");
    if (!sample) {
        SKIP("set MUSIC_SERVICE_AUDIO_SAMPLE to an MP3 file to run");
    }
    auto input = request(infrastructure::filesystem::path_from_utf8(*sample));
    input.expected.bitrate_bps = std::nullopt;
    if (const auto declared = infrastructure::config::process_environment("MUSIC_SERVICE_AUDIO_SAMPLE_MS")) {
        input.expected.duration_ms = std::stoll(*declared);
    }
    VerifyingProcessor processor;
    auto report = processor.process(input, context());
    if (!report) {
        FAIL(domain::to_string(report.error().code) << " " << reason(report.error()));
    }
    WARN("codec " << *report->facts.codec << ", " << *report->facts.sample_rate_hz << " Hz, "
                  << *report->facts.bitrate_bps << " bit/s, " << *report->facts.duration_ms << " ms, "
                  << report->facts.frames << " frames, completeness " << domain::to_string(report->completeness)
                  << ", warnings " << report->warnings.size());
    for (const auto& warning : report->warnings) {
        WARN(warning);
    }
    CHECK(report->facts.frames > 0);
}
