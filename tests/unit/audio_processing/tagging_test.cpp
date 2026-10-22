// Tags written by audio.verify v2 (T30), read back with TagLib: MP3 (ID3v2.4), FLAC
// (Vorbis comments and a picture block) and MP4/M4A (iTunes atoms) from recorded or
// generated samples. The received file stays untouched, the audio payload is the same,
// existing tags survive, writing again is stable, and a failed write follows the
// metadata policy. FLAC and M4A structure validation runs on the same samples.

#include "audio_processing/verifying_processor.hpp"
#include "infrastructure/filesystem/paths.hpp"

#include <catch2/catch_test_macros.hpp>

#include <flacfile.h>
#include <id3v1tag.h>
#include <id3v2tag.h>
#include <mp4file.h>
#include <mpegfile.h>
#include <tpropertymap.h>
#include <tvariant.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace music_service;
namespace ports = application::ports;
namespace fs = std::filesystem;
using audio_processing::VerifyingProcessor;

namespace {

class TempDir {
public:
    explicit TempDir(std::string_view name)
        : path_(fs::temp_directory_path() /
                infrastructure::filesystem::path_from_utf8("music-service-标签-" + std::string{name})) {
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
    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

std::string read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

std::string fixture(const char* name) {
    return read(fs::path{MUSIC_SERVICE_TEST_FIXTURES} / "audio" / name);
}

// MPEG-1 layer III, 44.1 kHz, 128 kbit/s frames.
std::string mp3_frames(std::size_t count) {
    std::string bytes;
    for (std::size_t i = 0; i < count; ++i) {
        const auto padding = i % 3 == 0;
        std::string frame(static_cast<std::size_t>(144 * 128 * 1000 / 44100 + (padding ? 1 : 0)), '\x55');
        frame[0] = '\xFF';
        frame[1] = '\xFB';
        frame[2] = static_cast<char>((9U << 4) | (padding ? 0x02U : 0U));
        frame[3] = '\x40';
        bytes += frame;
    }
    return bytes;
}

// A minimal 1x1 PNG.
std::string png() {
    static const unsigned char bytes[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
        0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00,
        0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x00, 0x01, 0x00, 0x00, 0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D,
        0xB4, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
    return {reinterpret_cast<const char*>(bytes), sizeof(bytes)};
}

ports::TrackMetadata metadata() {
    ports::TrackMetadata m;
    m.title = "晴天";
    m.artists = {"周杰伦", "Guest"};
    m.album = "叶惠美";
    m.lyrics = "故事的小黄花";
    m.cover = ports::Picture{"image/png", png()};
    m.track_number = 3;
    m.track_total = 12;
    return m;
}

ports::OperationContext context() { return {std::chrono::steady_clock::now() + std::chrono::minutes(1), {}}; }

ports::ProcessingRequest request(fs::path input, domain::MetadataPolicy policy = domain::MetadataPolicy::best_effort) {
    ports::ProcessingRequest value;
    value.input = std::move(input);
    value.metadata_policy = policy;
    value.metadata = metadata();
    return value;
}

TagLib::String utf8(const char* text) { return TagLib::String(text, TagLib::String::UTF8); }

bool stage(const ports::ProcessingReport& report, std::string_view name, ports::StageOutcome outcome) {
    for (const auto& s : report.stages) {
        if (s.stage == name) {
            return s.outcome == outcome;
        }
    }
    return false;
}

bool has_warning(const ports::ProcessingReport& report, std::string_view code) {
    return std::find(report.warnings.begin(), report.warnings.end(), code) != report.warnings.end();
}

// The unified properties and pictures every format reads back the same way.
void check_common(TagLib::File& file) {
    REQUIRE(file.isValid());
    const auto properties = file.properties();
    CHECK(properties["TITLE"] == TagLib::StringList(utf8("晴天")));
    TagLib::StringList artists;
    artists.append(utf8("周杰伦"));
    artists.append(utf8("Guest"));
    CHECK(properties["ARTIST"] == artists);
    CHECK(properties["ALBUM"] == TagLib::StringList(utf8("叶惠美")));
    CHECK(properties["LYRICS"] == TagLib::StringList(utf8("故事的小黄花")));
    CHECK(properties["TRACKNUMBER"] == TagLib::StringList("3/12"));
    const auto pictures = file.complexProperties("PICTURE");
    REQUIRE(pictures.size() == 1);
    const auto cover = png();
    CHECK(pictures.front().value("data").toByteVector() ==
          TagLib::ByteVector(cover.data(), static_cast<unsigned int>(cover.size())));
    CHECK(pictures.front().value("mimeType").toString() == "image/png");
}

} // namespace

TEST_CASE("unknown or zero track numbers never create zero track tags", "[audio][tags][output]") {
    TempDir dir{"unknown-track"};
    auto req = request(dir.file("input.part", mp3_frames(200)));
    req.metadata->track_number.reset();
    SECTION("missing number") {}
    SECTION("zero number") { req.metadata->track_number = 0; }
    VerifyingProcessor processor;
    const auto result = processor.process(req, context());
    REQUIRE(result);
    TagLib::MPEG::File file(result->output.c_str());
    CHECK(file.properties()["TRACKNUMBER"].isEmpty());
}

TEST_CASE("MP3 tags go into a copy and read back as ID3v2.4", "[audio][tags][mp3]") {
    TempDir dir{"mp3"};
    const auto audio = mp3_frames(200);
    const auto input = dir.file("input.part", audio);
    VerifyingProcessor processor;
    auto report = processor.process(request(input), context());
    REQUIRE(report);
    CHECK(stage(*report, "tags", ports::StageOutcome::done));
    CHECK(report->warnings.empty());
    CHECK(report->output == dir.path() / "tagged.mp3");
    CHECK(read(input) == audio); // the received file is untouched
    const auto tagged = read(report->output);
    CHECK(tagged.size() == static_cast<std::size_t>(report->facts.size_bytes));
    CHECK(tagged.compare(tagged.size() - audio.size(), audio.size(), audio) == 0); // the frames as received
    CHECK(report->facts.frames == 200);

    TagLib::MPEG::File file(report->output.c_str());
    check_common(file);
    REQUIRE(file.hasID3v2Tag());
    CHECK(file.ID3v2Tag()->header()->majorVersion() == 4);
    CHECK_FALSE(file.hasID3v1Tag()); // nothing added that was not asked for
}

TEST_CASE("tags already in an MP3 stay, and writing again is stable", "[audio][tags][mp3]") {
    TempDir dir{"mp3-existing"};
    // An earlier tagger's ID3v2.3 tag with a custom frame, and an ID3v1 tag at the end.
    const auto source = dir.file("source.mp3", mp3_frames(150));
    {
        TagLib::MPEG::File seed(source.c_str());
        seed.ID3v2Tag(true)->setTitle("old title");
        TagLib::PropertyMap extra = seed.properties();
        extra.insert("RELEASECOUNTRY", TagLib::StringList("TW"));
        seed.setProperties(extra);
        seed.ID3v1Tag(true)->setTitle("v1 title");
        REQUIRE(seed.save(TagLib::MPEG::File::AllTags, TagLib::File::StripNone, TagLib::ID3v2::v3));
    }
    const auto input = dir.file("input.part", read(source));
    VerifyingProcessor processor;
    auto first = processor.process(request(input), context());
    REQUIRE(first);
    REQUIRE(stage(*first, "tags", ports::StageOutcome::done));
    {
        TagLib::MPEG::File file(first->output.c_str());
        check_common(file);
        CHECK(file.properties()["RELEASECOUNTRY"] == TagLib::StringList("TW")); // kept
        REQUIRE(file.hasID3v1Tag());
        CHECK(file.ID3v1Tag()->title() == "v1 title"); // the ID3v1 tag is not rewritten
    }
    // Processing the tagged file again gives the same file: no duplicated frames.
    const auto again_input = dir.file("again.part", read(first->output));
    auto second = processor.process(request(again_input), context());
    REQUIRE(second);
    CHECK(read(second->output) == read(first->output));
    CHECK(second->facts.sha256 == first->facts.sha256);
}

TEST_CASE("FLAC is validated and tagged with Vorbis comments and a picture", "[audio][tags][flac]") {
    TempDir dir{"flac"};
    const auto audio = fixture("sine-3s.flac");
    const auto input = dir.file("input.part", audio);
    auto plain = request(input, domain::MetadataPolicy::skip);
    plain.expected.duration_ms = 3000;
    VerifyingProcessor processor;
    auto validated = processor.process(plain, context());
    REQUIRE(validated);
    CHECK(validated->facts.container == "flac");
    CHECK(validated->facts.codec == "flac");
    CHECK(validated->facts.sample_rate_hz == 8000);
    CHECK(validated->facts.channels == 1);
    CHECK(validated->facts.duration_ms == 3000);
    CHECK(validated->facts.frames == 24000); // samples
    CHECK(validated->completeness == domain::Completeness::full);
    CHECK(validated->output == input);

    auto report = processor.process(request(input), context());
    REQUIRE(report);
    CHECK(stage(*report, "tags", ports::StageOutcome::done));
    CHECK(report->output == dir.path() / "tagged.flac");
    CHECK(read(input) == audio);
    CHECK(report->facts.frames == 24000);
    TagLib::FLAC::File file(report->output.c_str());
    check_common(file);
    CHECK(file.pictureList().size() == 1);
}

TEST_CASE("M4A is validated and tagged with iTunes atoms", "[audio][tags][mp4]") {
    TempDir dir{"m4a"};
    const auto audio = fixture("sine-3s.m4a");
    const auto input = dir.file("input.part", audio);
    auto plain = request(input, domain::MetadataPolicy::skip);
    plain.expected.duration_ms = 3000;
    plain.expected.container = "m4a";
    VerifyingProcessor processor;
    auto validated = processor.process(plain, context());
    REQUIRE(validated);
    CHECK(validated->facts.container == "m4a");
    CHECK(validated->facts.codec == "aac");
    REQUIRE(validated->facts.duration_ms);
    CHECK(*validated->facts.duration_ms >= 2990);
    CHECK(*validated->facts.duration_ms <= 3100);
    CHECK(validated->completeness == domain::Completeness::full);
    CHECK(validated->warnings.empty());

    auto report = processor.process(request(input), context());
    REQUIRE(report);
    CHECK(stage(*report, "tags", ports::StageOutcome::done));
    CHECK(report->output == dir.path() / "tagged.m4a");
    CHECK(read(input) == audio);
    TagLib::MP4::File file(report->output.c_str());
    check_common(file);
}

TEST_CASE("cut-off FLAC and M4A files are not audio to publish", "[audio][truncated]") {
    TempDir dir{"cut"};
    VerifyingProcessor processor;
    const auto m4a = fixture("sine-3s.m4a");
    auto cut_m4a = processor.process(request(dir.file("a.part", m4a.substr(0, m4a.size() - 100)),
                                             domain::MetadataPolicy::skip),
                                     context());
    REQUIRE_FALSE(cut_m4a);
    CHECK(cut_m4a.error().code == domain::ErrorCode::audio_invalid);
    CHECK(cut_m4a.error().details.front().reason == "truncated");
    CHECK(cut_m4a.error().retryable);

    const auto flac = fixture("sine-3s.flac");
    auto cut_flac = processor.process(request(dir.file("b.part", flac.substr(0, 30)), domain::MetadataPolicy::skip),
                                      context());
    REQUIRE_FALSE(cut_flac);
    CHECK(cut_flac.error().details.front().reason == "truncated");
    // Most of the audio missing: fewer bytes than STREAMINFO's frames need.
    auto short_flac = processor.process(
        request(dir.file("c.part", flac.substr(0, 2000)), domain::MetadataPolicy::skip), context());
    REQUIRE_FALSE(short_flac);
    CHECK(short_flac.error().details.front().reason == "truncated");
}

TEST_CASE("a tag write that fails follows the metadata policy", "[audio][tags][metadata]") {
    TempDir dir{"failing"};
    const auto audio = mp3_frames(100);
    const auto input = dir.file("input.part", audio);
    fs::create_directories(dir.path() / "tagged.mp3"); // the copy cannot be created
    VerifyingProcessor processor;

    auto best_effort = processor.process(request(input), context());
    REQUIRE(best_effort);
    CHECK(stage(*best_effort, "tags", ports::StageOutcome::failed));
    CHECK(has_warning(*best_effort, ports::processing_warning::metadata_not_written));
    CHECK(best_effort->output == input); // the validated file, untagged
    CHECK(read(input) == audio);

    auto required = processor.process(request(input, domain::MetadataPolicy::required), context());
    REQUIRE_FALSE(required);
    CHECK(required.error().code == domain::ErrorCode::metadata_failed);
    CHECK(required.error().details.front().reason == "copy");
}
