#include "audio_processing/ncm.hpp"
#include "audio_processing/verifying_processor.hpp"
#include "infrastructure/filesystem/paths.hpp"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <thread>
#include <mpegfile.h>
#include <tpropertymap.h>
#include <tvariant.h>

using namespace music_service;
namespace fs = std::filesystem;
namespace ports = application::ports;
namespace {
std::string read(const fs::path& path) {
    std::ifstream file{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{file}, {}};
}
struct NcmTemp {
    fs::path root;
    explicit NcmTemp(std::string_view label) : root(fs::temp_directory_path() / ("music-service-ncm-unit-" + std::string{label})) {
        fs::remove_all(root); fs::create_directories(root);
    }
    ~NcmTemp() { std::error_code ec; fs::remove_all(root, ec); }
    fs::path input(std::string_view bytes) {
        const auto path = root / "input.ncm";
        std::ofstream out{path, std::ios::binary}; out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return path;
    }
};
const fs::path fixtures = fs::path{MUSIC_SERVICE_TEST_FIXTURES} / "audio";
ports::OperationContext context() { return {std::chrono::steady_clock::now() + std::chrono::seconds{20}, {}}; }
std::size_t u32(const std::string& bytes, std::size_t at) {
    std::size_t n = 0;
    for (unsigned i = 0; i < 4; ++i) n |= std::size_t{static_cast<unsigned char>(bytes[at + i])} << (8 * i);
    return n;
}
}

TEST_CASE("native NCM imports independent MP3 and padded FLAC fixtures without changing payload", "[ncm]") {
    for (const auto* format : {"mp3", "flac"}) {
        CAPTURE(format);
        NcmTemp temp{"payload"};
        const auto input = temp.input(read(fixtures / (std::string{"synthetic-"} + format + ".ncm")));
        auto processor = audio_processing::ProcessorCatalog::builtin().find({"audio.ncm", 1, "netease.ncm"});
        REQUIRE(processor);
        auto report = (*processor)->process({input, {}, domain::MetadataPolicy::skip, {}}, context());
        REQUIRE(report);
        CHECK(report->facts.container == format);
        const auto original = read(fixtures / (std::string{format} == "mp3" ? "ncm-plain.mp3" : "sine-3s.flac"));
        CHECK(read(report->output) == original);
        CHECK(report->source_metadata->title == "NCM synthetic");
        REQUIRE(report->source_metadata->artists.size() == 1);
        CHECK(report->source_metadata->artists.front() == "Synthetic artist");
        CHECK(report->source_metadata->album == "Synthetic album");
        CHECK(report->source_metadata->lyrics == "[00:00.00]Synthetic lyric\n");
        REQUIRE(report->source_metadata->cover);
        CHECK(report->source_metadata->cover->data == read(fixtures / "cover.png"));
        CHECK(read(input) == read(fixtures / (std::string{"synthetic-"} + format + ".ncm")));
    }
}

TEST_CASE("NCM rejects bad headers lengths keys metadata and truncated audio", "[ncm]") {
    auto original = read(fixtures / "synthetic-mp3.ncm");
    const auto meta_size_at = 14 + u32(original, 10);
    const auto frame_size_at = meta_size_at + 4 + u32(original, meta_size_at) + 5;
    for (const auto kind : {0, 1, 2, 3, 4, 5, 6, 7}) {
        CAPTURE(kind);
        NcmTemp temp{"corruption"};
        auto bad = original;
        if (kind == 0) bad[0] = 'X';
        if (kind == 1) bad.replace(10, 4, 4, '\xff');
        if (kind == 2) bad[14] ^= 0x51;
        if (kind == 3) bad[meta_size_at + 4] ^= 0x44;
        if (kind == 4) bad.replace(frame_size_at, 4, 4, '\0');
        if (kind == 5) bad.replace(frame_size_at, 4, 4, '\xff');
        if (kind == 6) bad.resize(19);
        if (kind == 7) bad.pop_back();
        auto report = audio_processing::NcmProcessor{}.process({temp.input(bad), {}, domain::MetadataPolicy::skip, {}}, context());
        REQUIRE_FALSE(report);
        CHECK(report.error().code == domain::ErrorCode::audio_invalid);
        CHECK_FALSE(fs::exists(temp.root / "ncm-decoded.part"));
    }
}

TEST_CASE("NCM bounds inputs respects cancellation and never replaces an output", "[ncm]") {
    NcmTemp temp{"limits"};
    const auto input = temp.input(read(fixtures / "synthetic-mp3.ncm"));
    const auto output = temp.root / "decoded";
    auto limited = audio_processing::NcmDecryptor{}.decode(input, output, context(), 100);
    CHECK_FALSE(limited);
    CHECK_FALSE(fs::exists(output));
    std::stop_source stop; stop.request_stop();
    auto canceled = audio_processing::NcmDecryptor{}.decode(input, output, {context().deadline, stop.get_token()});
    REQUIRE_FALSE(canceled);
    CHECK(canceled.error().code == domain::ErrorCode::canceled);
    auto expired = audio_processing::NcmDecryptor{}.decode(input, output, {std::chrono::steady_clock::now(), {}});
    REQUIRE_FALSE(expired);
    CHECK(expired.error().code == domain::ErrorCode::deadline_exceeded);
    { std::ofstream existing{output}; existing << "preserve"; }
    CHECK_FALSE(audio_processing::NcmDecryptor{}.decode(input, output, context()));
    CHECK(read(output) == "preserve");
}

TEST_CASE("NCM streaming cancellation removes partial decoded output", "[ncm]") {
    NcmTemp temp{"cancellation"};
    const auto input = temp.input(read(fixtures / "synthetic-mp3.ncm") + std::string(16 * 1024 * 1024, '\0'));
    const auto output = temp.root / "decoded";
    std::stop_source stop;
    std::jthread watcher([&](std::stop_token token) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds{3};
        while (!token.stop_requested()) {
            std::error_code ec;
            const auto size = fs::file_size(output, ec);
            if ((!ec && size >= 65536) || std::chrono::steady_clock::now() >= end) { stop.request_stop(); break; }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    });
    auto result = audio_processing::NcmDecryptor{}.decode(input, output, {context().deadline, stop.get_token()});
    REQUIRE_FALSE(result);
    CHECK(result.error().code == domain::ErrorCode::canceled);
    CHECK_FALSE(fs::exists(output));
}

TEST_CASE("default processing detects NCM from bytes and checks the encrypted transfer size", "[ncm][processing]") {
    for (const auto* format : {"mp3", "flac"}) {
        for (const auto size_delta : {-1, 0, 1}) {
            CAPTURE(format, size_delta);
            NcmTemp temp{"auto-detect"};
            const auto encrypted = read(fixtures / (std::string{"synthetic-"} + format + ".ncm"));
            // A misleading extension and declaration must not prevent detection.
            const auto input = temp.root / "download.mp3";
            fs::copy_file(temp.input(encrypted), input);
            ports::ProcessingRequest request{input, {}, domain::MetadataPolicy::skip, {}};
            request.expected.container = "mp3";
            request.expected.size_bytes = static_cast<std::int64_t>(encrypted.size()) + size_delta;
            auto report = audio_processing::VerifyingProcessor{}.process(request, context());
            if (size_delta != 0) {
                REQUIRE_FALSE(report);
                CHECK(report.error().code == domain::ErrorCode::audio_invalid);
                CHECK(report.error().retryable);
                CHECK_FALSE(fs::exists(temp.root / "ncm-decoded.part"));
                continue;
            }
            REQUIRE(report);
            CHECK(report->processor_id == "audio.ncm");
            CHECK(report->stages.front().outcome == ports::StageOutcome::done);
            CHECK(read(report->output) == read(fixtures / (std::string{format} == "mp3" ? "ncm-plain.mp3" : "sine-3s.flac")));
            CHECK(report->facts.container == format);
            CHECK(read(input) == encrypted);
        }
    }
}

TEST_CASE("automatic NCM uses embedded metadata without overriding platform tags or embedding switches", "[ncm][processing]") {
    for (const bool supplied : {false, true}) {
        NcmTemp temp{"auto-tags"};
        ports::ProcessingRequest request{temp.input(read(fixtures / "synthetic-mp3.ncm")), {},
            domain::MetadataPolicy::required, {}};
        if (supplied) {
            request.metadata = ports::TrackMetadata{"Platform title", {"Platform artist"}, {}, {}, {}};
            request.embed_cover = false;
            request.embed_lyrics = false;
        }
        auto report = audio_processing::VerifyingProcessor{}.process(request, context());
        REQUIRE(report);
        REQUIRE(report->source_metadata);
        REQUIRE(report->source_metadata->cover); // still available for sidecar export
        TagLib::MPEG::File tagged{report->output.c_str()};
        const auto tags = tagged.properties();
        CHECK(tags["TITLE"].toString().to8Bit(true) == (supplied ? "Platform title" : "NCM synthetic"));
        CHECK(tags["ARTIST"].toString().to8Bit(true) == (supplied ? "Platform artist" : "Synthetic artist"));
        CHECK(tags["ALBUM"].toString().to8Bit(true) == "Synthetic album");
        CHECK(tags.contains("LYRICS") == !supplied);
        CHECK(tagged.complexProperties("PICTURE").isEmpty() == supplied);
    }
}

TEST_CASE("automatic NCM fails closed for a damaged envelope", "[ncm][processing]") {
    NcmTemp temp{"auto-corrupt"};
    auto encrypted = read(fixtures / "synthetic-mp3.ncm");
    encrypted[14] ^= 0x51;
    auto report = audio_processing::VerifyingProcessor{}.process(
        {temp.input(encrypted), {}, domain::MetadataPolicy::skip, {}}, context());
    REQUIRE_FALSE(report);
    CHECK(report.error().code == domain::ErrorCode::audio_invalid);
    CHECK_FALSE(fs::exists(temp.root / "ncm-decoded.part"));
}

TEST_CASE("automatic NCM reprocesses stale working copies after restart without replacing its input", "[ncm][processing]") {
    NcmTemp temp{"auto-restart"};
    const auto encrypted = read(fixtures / "synthetic-mp3.ncm");
    ports::ProcessingRequest request{temp.input(encrypted), {}, domain::MetadataPolicy::skip, {}};
    const auto leftover = temp.root / "ncm-decoded.part";
    for (const auto* bytes : {"partially decoded before crash", "untrusted previous output"}) {
        { std::ofstream file{leftover, std::ios::binary | std::ios::trunc}; file << bytes; }
        const auto report = audio_processing::VerifyingProcessor{}.process(request, context());
        REQUIRE(report);
        CHECK(read(report->output) == read(fixtures / "ncm-plain.mp3"));
        CHECK(read(request.input) == encrypted);
    }
    fs::copy_file(request.input, leftover, fs::copy_options::overwrite_existing);
    request.input = leftover;
    CHECK_FALSE(audio_processing::VerifyingProcessor{}.process(request, context()));
    CHECK(read(leftover) == encrypted);
}
