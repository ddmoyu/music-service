#include "../../fixtures/audio/z3d_fixture.hpp"
#include "audio_processing/format/wav.hpp"
#include "audio_processing/migu_z3d.hpp"
#include "audio_processing/verifying_processor.hpp"
#include "infrastructure/filesystem/paths.hpp"
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <fstream>
#include <thread>
#include <tpropertymap.h>
#include <tvariant.h>
#include <wavfile.h>

using namespace music_service;
namespace fs = std::filesystem;
namespace ports = application::ports;
namespace z3d = test::z3d;
namespace {
std::string read(const fs::path& p) {
    std::ifstream in{p, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, {}};
}
void write(const fs::path& p, std::string_view bytes) {
    std::ofstream out{p, std::ios::binary};
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}
struct Temp {
    fs::path root;
    Temp() {
        static std::atomic<unsigned> seq{};
        root = fs::temp_directory_path() /
               ("music-z3d-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                std::to_string(seq++));
        fs::create_directories(root);
    }
    ~Temp() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    fs::path input(std::string_view b) {
        const auto p = root / "input.z3d";
        write(p, b);
        return p;
    }
};
ports::OperationContext context() {
    return {std::chrono::steady_clock::now() + std::chrono::seconds{30}, {}};
}
std::shared_ptr<const ports::SecretBytes> key(std::string_view k = z3d::file_key) {
    return std::make_shared<ports::SecretBytes>(k);
}
ports::ProcessingRequest request(const fs::path& p) {
    ports::ProcessingRequest r{p, {}, domain::MetadataPolicy::skip, {}};
    r.decryption_key = key();
    return r;
}
std::string env(const char* name) {
#ifdef _WIN32
    char* v = nullptr;
    std::size_t n{};
    if (_dupenv_s(&v, &n, name))
        return {};
    std::string s = v ? v : "";
    std::free(v);
    return s;
#else
    const auto* v = std::getenv(name);
    return v ? v : "";
#endif
}
} // namespace

TEST_CASE("Z3D native AES MD5 key derivation matches the independent published vector", "[z3d]") {
    for (const bool marker : {false, true}) {
        for (const unsigned fmt_size : {16U, 18U, 40U}) {
            CAPTURE(marker, fmt_size);
            Temp t;
            const auto plain = z3d::wav(40000, fmt_size); // spans several streaming windows
            const auto encrypted = z3d::encrypt(plain, marker);
            const auto input = t.input(encrypted), output = t.root / "decoded.wav";
            REQUIRE(audio_processing::MiguZ3dDecryptor{key()}.decrypt(input, output, context()));
            CHECK(read(output) == plain);
            CHECK(read(input) == encrypted);
        }
    }
}

TEST_CASE("legacy MG3D recovery starts each candidate from the original header", "[z3d]") {
    Temp t;
    auto plain = z3d::wav();
    // Expose a wrong all-hex candidate in a JUNK chunk, before the correct silence block.
    std::string junk(128, '\0');
    for (std::size_t i = 12; i < 44; ++i)
        junk[i] = static_cast<char>('F' - z3d::stream_key[(20 + i) % 32]);
    plain = z3d::riff(z3d::chunk("JUNK", junk) + plain.substr(12));
    const auto output = t.root / "decoded.wav";
    REQUIRE(audio_processing::MiguZ3dDecryptor{}.decrypt(t.input(z3d::encrypt(plain)), output,
                                                         context()));
    CHECK(read(output) == plain);
}

TEST_CASE("Z3D rejects invalid keys and non WAV data without publishing or changing input",
          "[z3d]") {
    for (const auto mode : {0, 1, 2, 3, 4, 5}) {
        CAPTURE(mode);
        Temp t;
        auto bytes = z3d::encrypt(z3d::wav());
        if (mode == 2)
            bytes = "<html>denied</html>";
        if (mode == 3)
            bytes.pop_back();
        if (mode == 4)
            bytes = z3d::wav(); // explicit encrypted plan never accepts plaintext
        if (mode == 5) {
            auto plain = z3d::wav();
            z3d::put(plain, 40, 0xfffffff0); // correct header, corrupt data chunk
            bytes = z3d::encrypt(plain);
        }
        const auto input = t.input(bytes), output = t.root / "decoded.wav";
        const auto k = mode == 0   ? key("bad")
                       : mode == 1 ? key("00000000000000000000000000000000")
                                   : key();
        const auto result = audio_processing::MiguZ3dDecryptor{k}.decrypt(input, output, context());
        REQUIRE_FALSE(result);
        CHECK(result.error().code == domain::ErrorCode::audio_invalid);
        CHECK_FALSE(fs::exists(output));
        CHECK(read(input) == bytes);
    }
}

TEST_CASE("Z3D keyless recovery fails explicitly when no zero block exposes a key", "[z3d]") {
    Temp t;
    auto plain = z3d::wav(10);
    for (std::size_t i = 44; i < plain.size(); ++i)
        plain[i] = '\x7f';
    const auto output = t.root / "decoded.wav";
    const auto result = audio_processing::MiguZ3dDecryptor{}.decrypt(t.input(z3d::encrypt(plain)),
                                                                     output, context());
    REQUIRE_FALSE(result);
    CHECK_FALSE(fs::exists(output));
}

TEST_CASE("WAV validation rejects inconsistent RIFF sizes formats and sample layouts",
          "[z3d][wav]") {
    for (const int mutation : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10}) {
        CAPTURE(mutation);
        Temp t;
        auto bytes = z3d::wav();
        if (mutation == 0)
            bytes.pop_back();
        if (mutation == 1)
            z3d::put(bytes, 20, 3, 2); // IEEE float unsupported
        if (mutation == 2)
            z3d::put(bytes, 22, 0, 2);
        if (mutation == 3)
            z3d::put(bytes, 28, 1);
        if (mutation == 4)
            z3d::put(bytes, 32, 3, 2);
        if (mutation == 5)
            z3d::put(bytes, 40, 64001);
        if (mutation == 6)
            bytes = z3d::riff(bytes.substr(12, 24) + bytes.substr(12));
        if (mutation == 7)
            bytes = z3d::riff(bytes.substr(12) + z3d::chunk("data", std::string(4, '\0')));
        if (mutation == 8) {
            bytes = z3d::wav(10, 40);
            z3d::put(bytes, 38, 17, 2);
        }
        if (mutation == 9) {
            bytes = z3d::wav(10, 40);
            z3d::put(bytes, 40, 1);
        }
        if (mutation == 10) {
            bytes = z3d::wav(10, 40);
            bytes[44] = 3;
        }
        const auto report =
            audio_processing::VerifyingProcessor{}.process(request(t.input(bytes)), context());
        REQUIRE_FALSE(report);
        CHECK(report.error().code == domain::ErrorCode::audio_invalid);
    }
}

TEST_CASE("WAV accepts word padded unknown chunks and measures PCM facts", "[z3d][wav]") {
    Temp t;
    const auto bytes = z3d::riff(z3d::chunk("JUNK", "odd") + z3d::wav().substr(12));
    const auto report =
        audio_processing::VerifyingProcessor{}.process(request(t.input(bytes)), context());
    REQUIRE(report);
    CHECK(report->facts.container == "wav");
    CHECK(report->facts.codec == "pcm_s16le");
    CHECK(report->facts.sample_rate_hz == 8000);
    CHECK(report->facts.bits_per_sample == 16);
    CHECK(report->facts.channels == 2);
    CHECK(report->facts.duration_ms == 2000);
    CHECK(report->facts.audio_bytes == 64000);
}

TEST_CASE("Z3D cancellation deadline and existing output never replace data", "[z3d]") {
    Temp t;
    const auto input = t.input(z3d::encrypt(z3d::wav())), output = t.root / "out.wav";
    std::stop_source stop;
    stop.request_stop();
    auto canceled = audio_processing::MiguZ3dDecryptor{key()}.decrypt(
        input, output, {context().deadline, stop.get_token()});
    REQUIRE_FALSE(canceled);
    CHECK(canceled.error().code == domain::ErrorCode::canceled);
    auto expired = audio_processing::MiguZ3dDecryptor{key()}.decrypt(
        input, output, {std::chrono::steady_clock::now(), {}});
    REQUIRE_FALSE(expired);
    CHECK(expired.error().code == domain::ErrorCode::deadline_exceeded);
    CHECK_FALSE(fs::exists(output));
    write(output, "keep");
    CHECK_FALSE(audio_processing::MiguZ3dDecryptor{key()}.decrypt(input, output, context()));
    CHECK(read(output) == "keep");
    CHECK_FALSE(audio_processing::MiguZ3dDecryptor{key()}.decrypt(input, input, context()));
}

TEST_CASE("Z3D processing writes and reads back WAV tags without changing PCM", "[z3d][wav]") {
    Temp t;
    const auto encrypted = z3d::encrypt(z3d::wav());
    auto r = request(t.input(encrypted));
    r.expected.size_bytes = static_cast<std::int64_t>(encrypted.size());
    r.expected.duration_ms = 2000;
    r.metadata_policy = domain::MetadataPolicy::required;
    r.metadata =
        ports::TrackMetadata{"合成 Z3D", {"歌手"}, "专辑", "[00:00.00]测试歌词", {}, 2, 10};
    r.metadata->cover = ports::Picture{
        "image/png", read(fs::path{MUSIC_SERVICE_TEST_FIXTURES} / "audio" / "cover.png")};
    const auto processor =
        audio_processing::ProcessorCatalog::builtin().find({"audio.migu-z3d", 1, "migu.z3d"});
    REQUIRE(processor);
    for (int retry = 0; retry < 2; ++retry) {
        const auto report = (*processor)->process(r, context());
        REQUIRE(report);
        CHECK(report->processor_id == "audio.migu-z3d");
        CHECK(report->completeness == domain::Completeness::full);
        CHECK(report->stages.front().outcome == ports::StageOutcome::done);
        TagLib::RIFF::WAV::File wav{report->output.c_str()};
        CHECK(wav.properties()["TITLE"].toString().to8Bit(true) == "合成 Z3D");
        CHECK(wav.properties()["LYRICS"].toString().to8Bit(true) == "[00:00.00]测试歌词");
        CHECK_FALSE(wav.complexProperties("PICTURE").isEmpty());
        CHECK(read(r.input) == encrypted);
    }
}

TEST_CASE("Z3D streaming cancellation discards partially written plaintext", "[z3d]") {
    Temp t;
    const auto input = t.input(z3d::encrypt(z3d::wav(4 * 1024 * 1024)));
    const auto output = t.root / "partial.wav";
    std::stop_source stop;
    std::atomic<bool> done{};
    std::jthread canceler([&] {
        while (!done.load()) {
            std::error_code ec;
            if (fs::file_size(output, ec) > 0 && !ec) {
                stop.request_stop();
                break;
            }
            std::this_thread::yield();
        }
    });
    const auto result = audio_processing::MiguZ3dDecryptor{key()}.decrypt(
        input, output, {context().deadline, stop.get_token()});
    done = true;
    canceler.join();
    REQUIRE_FALSE(result);
    CHECK(result.error().code == domain::ErrorCode::canceled);
    CHECK_FALSE(fs::exists(output));
    CHECK(fs::exists(input));
}

TEST_CASE("Z3D verification keeps previews and detects a shorter decrypted clip", "[z3d]") {
    Temp t;
    auto r = request(t.input(z3d::encrypt(z3d::wav())));
    SECTION("known preview stays preview") {
        r.expected.completeness = domain::Completeness::preview;
        r.expected.duration_ms = 2000;
    }
    SECTION("short file cannot retain a full length claim") {
        r.expected.completeness = domain::Completeness::full;
        r.expected.duration_ms = 200000;
    }
    const auto report = audio_processing::MiguZ3dProcessor{}.process(r, context());
    REQUIRE(report);
    CHECK(report->completeness == domain::Completeness::preview);
}

TEST_CASE("Z3D cancellation during tagging removes every plaintext working copy", "[z3d]") {
    Temp t;
    auto r = request(t.input(z3d::encrypt(z3d::wav(4 * 1024 * 1024))));
    r.metadata_policy = domain::MetadataPolicy::required;
    r.metadata = ports::TrackMetadata{"Canceled tagging", {}, {}, {}, {}};
    const auto tagged = t.root / "tagged.wav";
    std::stop_source stop;
    std::atomic<bool> done{};
    std::jthread canceler([&](std::stop_token lifetime) {
        while (!done.load() && !lifetime.stop_requested()) {
            std::error_code ec;
            if (fs::file_size(tagged, ec) > 0 && !ec) {
                stop.request_stop();
                break;
            }
            std::this_thread::yield();
        }
    });
    const auto result =
        audio_processing::MiguZ3dProcessor{}.process(r, {context().deadline, stop.get_token()});
    done = true;
    canceler.join();
    REQUIRE_FALSE(result);
    CHECK(result.error().code == domain::ErrorCode::canceled);
    CHECK_FALSE(fs::exists(tagged));
    CHECK_FALSE(fs::exists(t.root / "migu-z3d-decoded.part"));
    CHECK(fs::exists(r.input));
}

TEST_CASE("Z3D processing rejects stale aliases size mismatch and required metadata failure",
          "[z3d]") {
    Temp t;
    auto r = request(t.input(z3d::encrypt(z3d::wav())));
    const auto output = t.root / "migu-z3d-decoded.part";
    SECTION("transfer size") { r.expected.size_bytes = 1; }
    SECTION("required metadata") { r.metadata_policy = domain::MetadataPolicy::required; }
    SECTION("hard link alias") {
        fs::create_hard_link(r.input, output);
        REQUIRE_FALSE(audio_processing::MiguZ3dProcessor{}.process(r, context()));
        CHECK(read(output) == read(r.input));
        return;
    }
    SECTION("tagged input alias") {
        const auto tagged = t.root / "tagged.wav";
        const auto original = read(r.input);
        fs::create_hard_link(r.input, tagged);
        r.metadata_policy = domain::MetadataPolicy::required;
        r.metadata = ports::TrackMetadata{"Alias", {}, {}, {}, {}};
        REQUIRE_FALSE(audio_processing::MiguZ3dProcessor{}.process(r, context()));
        CHECK(read(r.input) == original);
        return;
    }
    REQUIRE_FALSE(audio_processing::MiguZ3dProcessor{}.process(r, context()));
    CHECK_FALSE(fs::exists(output));
}

TEST_CASE("native Z3D validates an opt in real local sample", "[.][z3d-real]") {
    const auto sample = env("MUSIC_SERVICE_Z3D_SAMPLE"),
               key_file = env("MUSIC_SERVICE_Z3D_KEY_FILE");
    if (sample.empty() || key_file.empty())
        SKIP("set MUSIC_SERVICE_Z3D_SAMPLE and MUSIC_SERVICE_Z3D_KEY_FILE");
    Temp t;
    auto r = request(t.input(read(infrastructure::filesystem::path_from_utf8(sample))));
    r.decryption_key = key(read(infrastructure::filesystem::path_from_utf8(key_file)));
    const auto report = audio_processing::MiguZ3dProcessor{}.process(r, context());
    REQUIRE(report);
    SUCCEED("native Z3D: " << report->facts.size_bytes << " bytes; "
                           << report->facts.duration_ms.value_or(0) << " ms; "
                           << report->facts.sample_rate_hz.value_or(0) << " Hz; "
                           << report->facts.bits_per_sample.value_or(0) << " bits; SHA256 "
                           << report->facts.sha256);
}
