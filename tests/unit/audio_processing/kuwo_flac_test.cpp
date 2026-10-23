#include "audio_processing/format/flac_frames.hpp"
#include "audio_processing/kuwo_flac.hpp"
#include "audio_processing/verifying_processor.hpp"
#include "downloads/metadata_support.hpp"
#include "infrastructure/filesystem/paths.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stop_token>

using namespace music_service;
namespace fs = std::filesystem;
namespace ports = application::ports;
namespace fmt = audio_processing::format;
namespace {
const fs::path fixtures = fs::path{MUSIC_SERVICE_TEST_FIXTURES} / "audio";
std::string read(const fs::path& path) {
    std::ifstream input{path, std::ios::binary};
    REQUIRE(input);
    return {std::istreambuf_iterator<char>{input}, {}};
}
struct Temp {
    fs::path root;
    explicit Temp(std::string_view name) : root(fs::temp_directory_path() / ("music-service-kuwo-flac-" + std::string{name})) {
        fs::remove_all(root); fs::create_directories(root);
    }
    ~Temp() { std::error_code ec; fs::remove_all(root, ec); }
    fs::path input(std::string_view bytes) {
        const auto path = root / infrastructure::filesystem::path_from_utf8("原始黑胶.part");
        std::ofstream out{path, std::ios::binary};
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return path;
    }
};
ports::OperationContext context() { return {std::chrono::steady_clock::now() + std::chrono::seconds{30}, {}}; }
ports::ProcessingRequest request(const fs::path& path) {
    ports::ProcessingRequest r;
    r.input = path; r.metadata_policy = domain::MetadataPolicy::skip;
    r.expected.container = "flac"; r.expected.size_bytes = static_cast<std::int64_t>(fs::file_size(path));
    return r;
}
std::string tail(std::size_t payload = 22) {
    return std::string{"\xf0\x00\xff\x0f", 4} + std::string(payload, 'k') + std::string{"\x0e\x55\xff\xf0", 4};
}
std::string hex(const std::array<unsigned char, 16>& data) {
    constexpr char chars[] = "0123456789abcdef";
    std::string out;
    for (auto b : data) { out += chars[b >> 4]; out += chars[b & 15]; }
    return out;
}
}

TEST_CASE("native FLAC frames reproduce independently encoded PCM at multiple depths", "[audio][kuwo-vinyl]") {
    const auto manifest = nlohmann::json::parse(read(fixtures / "flac-verification.json"));
    for (const auto& entry : manifest) {
        CAPTURE(entry["file"]);
        auto reader = fmt::ByteReader::open(fixtures / entry["file"].get<std::string>());
        REQUIRE(reader);
        auto info = fmt::scan_flac(*reader, context()); REQUIRE(info);
        auto frames = fmt::verify_flac_frames(*reader, *info, context());
        if (!frames) INFO(frames.error().details.front().reason);
        REQUIRE(frames);
        CHECK(frames->audio_end == reader->size());
        CHECK(frames->frames > 0);
        CHECK(hex(frames->pcm_md5) == entry["pcm_md5"].get<std::string>());
        CHECK(info->sample_rate_hz == entry["sample_rate_hz"].get<int>());
        CHECK(info->bits_per_sample == entry["bits_per_sample"].get<int>());
        CHECK(info->channels == entry["channels"].get<int>());
        CHECK(info->total_samples == entry["total_samples"].get<std::uint64_t>());
    }
}

TEST_CASE("Kuwo tail cleanup follows verified frame boundaries and keeps source bytes", "[audio][kuwo-vinyl]") {
    const auto audio = read(fixtures / "verify-24-96-lpc.flac");
    for (const auto extra : {std::string{}, tail(), tail(2), tail(100)}) {
        CAPTURE(extra.size());
        Temp temp{"tail"};
        const auto input = temp.input(audio + extra);
        auto processor = audio_processing::ProcessorCatalog::builtin().find({"audio.kuwo-flac", 1, {}});
        REQUIRE(processor);
        auto report = (*processor)->process(request(input), context());
        if (!report) INFO(report.error().details.front().reason);
        REQUIRE(report);
        CHECK(report->processor_id == "audio.kuwo-flac");
        CHECK(read(report->output) == audio);
        CHECK(read(input) == audio + extra);
        CHECK(report->facts.sample_rate_hz == 96000);
        CHECK(report->facts.bits_per_sample == 24);
        CHECK(report->facts.channels == 2);
        CHECK(report->facts.sha256 == "edae8a8dca6eaf3f0e12c3257c1c26d379878cab40b1bb6c10f3dd67e846b50a");
        CHECK(downloads::verified_quality(domain::Quality::hires, report->facts).value() == domain::Quality::hires);
        CHECK((std::find(report->warnings.begin(), report->warnings.end(), "KUWO_TRAILER_REMOVED") != report->warnings.end()) == !extra.empty());
        // 重试同一份原始输入仍得到相同成品，不重复修剪音频。
        auto again = (*processor)->process(request(input), context()); REQUIRE(again);
        CHECK(again->facts.sha256 == report->facts.sha256);
    }
}

TEST_CASE("Kuwo FLAC rejects corruption false sample counts and unrecognized trailers", "[audio][kuwo-vinyl]") {
    const auto original = read(fixtures / "verify-24-96-lpc.flac");
    auto source = fmt::ByteReader::open(fixtures / "verify-24-96-lpc.flac"); REQUIRE(source);
    const auto info = fmt::scan_flac(*source, context()); REQUIRE(info);
    for (int kind = 0; kind < 11; ++kind) {
        CAPTURE(kind);
        Temp temp{"invalid"}; auto bad = original;
        if (kind == 0) bad[static_cast<std::size_t>(info->audio_offset) + 5] ^= 1;
        if (kind == 1) bad[bad.size() - 8] ^= 2;
        if (kind == 2) bad.pop_back();
        if (kind == 3) bad[26] ^= 1; // PCM MD5，不依赖帧 CRC。
        if (kind == 4) bad[25] += 1; // STREAMINFO 总样本数。
        if (kind == 5) bad[25] = static_cast<char>(static_cast<unsigned char>(bad[25]) - 1);
        if (kind == 6) bad.replace(26, 16, 16, '\0');
        if (kind == 7) bad += "unknown trailer";
        if (kind == 8) bad += tail(4096);
        if (kind == 9) bad += original;
        if (kind == 10) bad += tail().substr(0, 29);
        const auto input = temp.input(bad);
        auto report = audio_processing::KuwoFlacProcessor{}.process(request(input), context());
        REQUIRE_FALSE(report);
        CHECK(report.error().code == domain::ErrorCode::audio_invalid);
        CHECK(read(input) == bad);
        CHECK_FALSE(fs::exists(temp.root / "kuwo-clean.flac"));
    }
}

TEST_CASE("Kuwo native FLAC tags retain PCM and measured quality rejects false hires", "[audio][kuwo-vinyl]") {
    for (const auto* file : {"verify-24-96-lpc.flac", "verify-16-44-fixed.flac"}) {
        CAPTURE(file); Temp temp{"tags"};
        auto r = request(temp.input(read(fixtures / file) + tail()));
        r.metadata_policy = domain::MetadataPolicy::required;
        r.metadata = ports::TrackMetadata{"原生黑胶测试", {"合成信号"}, "测试专辑", {}, {}, 3, {}};
        auto report = audio_processing::KuwoFlacProcessor{}.process(r, context());
        REQUIRE(report);
        CHECK(report->output.filename() == "tagged.flac");
        CHECK_FALSE(fs::exists(temp.root / "kuwo-clean.flac"));
        const auto quality = downloads::verified_quality(domain::Quality::hires, report->facts);
        REQUIRE(quality);
        const auto strict = domain::evaluate_resource({domain::Quality::hires}, {*quality, domain::Completeness::full});
        const bool hires = std::string_view{file}.starts_with("verify-24");
        CHECK(strict.has_value() == hires);
        if (!hires) {
            CHECK(strict.error().code == domain::ErrorCode::quality_unavailable);
            auto lower = domain::evaluate_resource({domain::Quality::hires, domain::QualityPolicy::allow_lower}, {*quality, domain::Completeness::full});
            REQUIRE(lower);
            CHECK(lower->warnings == std::vector{domain::ResourceWarning::quality_lowered});
        }
    }
}

TEST_CASE("Kuwo FLAC respects transfer size cancellation deadlines and input path", "[audio][kuwo-vinyl]") {
    Temp temp{"control"};
    const auto audio = read(fixtures / "verify-24-96-lpc.flac");
    auto r = request(temp.input(audio + tail()));
    r.expected.size_bytes = *r.expected.size_bytes + 1;
    CHECK_FALSE(audio_processing::KuwoFlacProcessor{}.process(r, context()));
    r.expected.size_bytes.reset();
    std::stop_source stop; stop.request_stop();
    auto canceled = audio_processing::KuwoFlacProcessor{}.process(r, {context().deadline, stop.get_token()});
    REQUIRE_FALSE(canceled); CHECK(canceled.error().code == domain::ErrorCode::canceled);
    auto expired = audio_processing::KuwoFlacProcessor{}.process(r, {std::chrono::steady_clock::now(), {}});
    REQUIRE_FALSE(expired); CHECK(expired.error().code == domain::ErrorCode::deadline_exceeded);
    CHECK_FALSE(fs::exists(temp.root / "kuwo-clean.flac"));
    const auto collision = temp.root / "kuwo-clean.flac";
    fs::copy_file(r.input, collision);
    CHECK_FALSE(audio_processing::KuwoFlacProcessor{}.process(request(collision), context()));
    CHECK(read(collision) == audio + tail());
}
