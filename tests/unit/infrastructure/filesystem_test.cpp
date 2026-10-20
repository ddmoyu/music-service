#include "infrastructure/filesystem/data_layout.hpp"
#include "infrastructure/filesystem/files.hpp"
#include "infrastructure/filesystem/instance_lock.hpp"
#include "infrastructure/filesystem/paths.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <iterator>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#endif

using namespace music_service;
using namespace music_service::infrastructure::filesystem;
namespace fs = std::filesystem;

namespace {

// A fresh directory with a non-ASCII name, removed afterwards.
class TempDir {
public:
    explicit TempDir(std::string_view name)
        : path_(fs::temp_directory_path() / path_from_utf8("music-service-测试-" + std::string{name})) {
        fs::remove_all(path_);
        fs::create_directories(path_);
        path_ = fs::canonical(path_);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

} // namespace

TEST_CASE("UTF-8 paths round-trip", "[filesystem][paths]") {
    const std::string text = "音乐/示例歌手/歌曲 (Live).flac";
    CHECK(path_to_utf8(path_from_utf8(text)) == text);
}

TEST_CASE("relative paths are checked before joining", "[filesystem][paths]") {
    for (const auto* ok : {"a.mp3", "Artist/Album/01 歌曲.flac", "x/.hidden", "a..b/c"}) {
        CHECK(check_relative_path(ok));
    }
    for (const auto* bad : {"", "/etc/passwd", "../x", "a/../../b", "a//b", "./a", "a/.",
                            "C:/x", "a\\b", "a:b", "con", "CON.txt", "lpt1.mp3", "a/nul/b",
                            "name.", "name ", " name", "a\tb", "a?b"}) {
        INFO(bad);
        CHECK_FALSE(check_relative_path(bad));
    }
    CHECK_FALSE(check_relative_path("\xC3\x28"));
    CHECK(check_relative_path("com10.mp3")); // only COM1-COM9 are reserved
}

TEST_CASE("resolve_within keeps results inside the root", "[filesystem][paths]") {
    TempDir dir{"resolve"};
    const auto root = dir.path();
    const auto resolved = resolve_within(root, "歌手/专辑/歌.mp3");
    REQUIRE(resolved);
    CHECK(is_within(root, *resolved));
    CHECK(*resolved == root / path_from_utf8("歌手/专辑/歌.mp3"));
    CHECK_FALSE(resolve_within(root, "../outside.mp3"));
}

TEST_CASE("symlinks that leave the root are rejected", "[filesystem][paths]") {
    TempDir root{"link-root"};
    TempDir outside{"link-outside"};
    std::error_code ec;
    fs::create_directory_symlink(outside.path(), root.path() / "escape", ec);
    if (ec) {
        SKIP("creating directory symlinks needs extra privileges on this system");
    }
    const auto escaped = resolve_within(root.path(), "escape/song.mp3");
    REQUIRE_FALSE(escaped);
    CHECK(escaped.error().code == domain::ErrorCode::validation_failed);
}

TEST_CASE("is_within compares whole components", "[filesystem][paths]") {
    CHECK(is_within(fs::path{"/music"}, fs::path{"/music/a"}));
    CHECK(is_within(fs::path{"/music"}, fs::path{"/music"}));
    CHECK_FALSE(is_within(fs::path{"/music"}, fs::path{"/music2/a"}));
    CHECK_FALSE(is_within(fs::path{"/music/a"}, fs::path{"/music"}));
}

TEST_CASE("file names are sanitized for every system", "[filesystem][paths]") {
    CHECK(sanitize_file_name("AC/DC: Back?") == "AC_DC_ Back_");
    CHECK(sanitize_file_name("  title. . ") == "title");
    CHECK(sanitize_file_name("CON") == "_CON");
    CHECK(sanitize_file_name("nul.mp3") == "_nul.mp3");
    CHECK(sanitize_file_name("...") == "_");
    CHECK(sanitize_file_name("") == "_");
    CHECK(sanitize_file_name("a\x01" "b") == "a_b");
    CHECK(sanitize_file_name("示例歌曲") == "示例歌曲");

    // Truncation never splits a multi-byte character.
    const auto cut = sanitize_file_name("歌歌歌", 7);
    CHECK(cut == "歌歌");
    CHECK(check_relative_path(sanitize_file_name("../../etc")));
}

TEST_CASE("new files are created exclusively and never overwritten", "[filesystem][files]") {
    TempDir dir{"files"};
    const auto file = dir.path() / path_from_utf8("密钥.secret");

    REQUIRE(write_new_file(file, "first", Privacy::owner_only));
    const auto second = write_new_file(file, "second", Privacy::owner_only);
    REQUIRE_FALSE(second);
    CHECK(second.error().code == domain::ErrorCode::file_system);

    const auto content = read_file(file, 1024);
    REQUIRE(content);
    CHECK(*content == "first");
    CHECK_FALSE(read_file(file, 2)); // over the size limit
    CHECK(read_file(dir.path() / "missing", 10).error().code == domain::ErrorCode::not_found);

#ifndef _WIN32
    const auto perms = fs::status(file).permissions();
    CHECK((perms & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
#endif

    REQUIRE(remove_file(file));
    CHECK(remove_file(file)); // already gone is fine
}

TEST_CASE("rename refuses to replace an existing target", "[filesystem][files]") {
    TempDir dir{"rename"};
    const auto from = dir.path() / "work.part";
    const auto to = dir.path() / path_from_utf8("歌曲.mp3");
    REQUIRE(write_new_file(from, "new", Privacy::normal));
    REQUIRE(write_new_file(to, "existing", Privacy::normal));

    REQUIRE_FALSE(rename_no_replace(from, to));
    CHECK(*read_file(to, 64) == "existing");
    CHECK(fs::exists(from));

    REQUIRE(remove_file(to));
    REQUIRE(rename_no_replace(from, to));
    CHECK(*read_file(to, 64) == "new");
    CHECK_FALSE(fs::exists(from));
}

TEST_CASE("private directories are created with parents", "[filesystem][files]") {
    TempDir dir{"dirs"};
    const auto nested = dir.path() / "a" / path_from_utf8("秘密") / "c";
    REQUIRE(create_directories(nested, Privacy::owner_only));
    CHECK(fs::is_directory(nested));
    CHECK(create_directories(nested, Privacy::owner_only));
#ifndef _WIN32
    const auto perms = fs::status(nested).permissions();
    CHECK((perms & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
#endif
}

TEST_CASE("existing secret directories are made private", "[filesystem][layout]") {
    TempDir dir{"restored"};
    const auto data = dir.path() / "data";
    // A restored backup or a hand-made directory inherits the parent's permissions.
    fs::create_directories(data / "secrets" / "credentials");
#ifndef _WIN32
    fs::permissions(data / "secrets", fs::perms::owner_all | fs::perms::group_all |
                                          fs::perms::others_read | fs::perms::others_exec);
#endif
    const auto layout = prepare_data_dir(data);
    REQUIRE(layout);
    for (const auto& path : {layout->secrets, layout->credentials}) {
        INFO(path_to_utf8(path));
#ifdef _WIN32
        PACL dacl = nullptr;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        std::wstring name = path.native();
        REQUIRE(::GetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                        nullptr, nullptr, &dacl, nullptr,
                                        &descriptor) == ERROR_SUCCESS);
        SECURITY_DESCRIPTOR_CONTROL control = 0;
        DWORD revision = 0;
        ::GetSecurityDescriptorControl(descriptor, &control, &revision);
        CHECK((control & SE_DACL_PROTECTED) != 0); // nothing inherited from the parent
        REQUIRE(dacl != nullptr);
        CHECK(dacl->AceCount == 2); // owner and SYSTEM
        ::LocalFree(descriptor);
#else
        const auto perms = fs::status(path).permissions();
        CHECK((perms & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
#endif
    }
}

TEST_CASE("only one instance can hold the data directory", "[filesystem][lock]") {
    TempDir dir{"lock"};
    const auto lock_file = dir.path() / "instance.lock";

    auto first = InstanceLock::acquire(lock_file);
    REQUIRE(first);
    CHECK(first->held());

    const auto second = InstanceLock::acquire(lock_file);
    REQUIRE_FALSE(second);
    CHECK(second.error().code == domain::ErrorCode::instance_locked);

    first->release();
    CHECK_FALSE(first->held());
    auto third = InstanceLock::acquire(lock_file);
    CHECK(third);
}

TEST_CASE("data_dir is prepared with the documented layout", "[filesystem][layout]") {
    TempDir dir{"data"};
    const auto layout = prepare_data_dir(dir.path() / path_from_utf8("数据"));
    REQUIRE(layout);
    CHECK(fs::is_directory(layout->secrets));
    CHECK(fs::is_directory(layout->credentials));
    CHECK(fs::is_directory(layout->cache));
    CHECK(fs::is_directory(layout->logs));
    CHECK(layout->lock_file.filename() == "instance.lock");
    CHECK(layout->database.filename() == "service.sqlite3");
    CHECK(layout->bootstrap_token.parent_path() == layout->secrets);
    // The writability probe leaves nothing behind.
    CHECK(std::distance(fs::directory_iterator{layout->root}, fs::directory_iterator{}) == 3);
}

TEST_CASE("destination roots must exist and stay apart from data_dir", "[filesystem][layout]") {
    TempDir dir{"destination"};
    const auto data = prepare_data_dir(dir.path() / "data");
    REQUIRE(data);

    const auto missing = check_destination_root(dir.path() / "offline-share", data->root);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == domain::ErrorCode::destination_unavailable);
    CHECK_FALSE(fs::exists(dir.path() / "offline-share")); // never created as a fallback

    const auto music = dir.path() / path_from_utf8("音乐");
    fs::create_directories(music);
    const auto ok = check_destination_root(music, data->root);
    REQUIRE(ok);
    CHECK(*ok == fs::canonical(music));

    CHECK_FALSE(check_destination_root(data->root, data->root));
    CHECK_FALSE(check_destination_root(dir.path(), data->root)); // contains data_dir
}

TEST_CASE("task workspaces live under the destination root", "[filesystem][layout]") {
    TempDir dir{"workspace"};
    const auto workspace = task_workspace(dir.path(), "tsk_abc123");
    REQUIRE(workspace);
    CHECK(fs::is_directory(*workspace));
    CHECK(*workspace == dir.path() / ".music-service-work" / "tsk_abc123");
    CHECK_FALSE(task_workspace(dir.path(), "../tsk_x"));
}

TEST_CASE("task workspaces cannot be redirected outside the root", "[filesystem][layout]") {
    TempDir root{"workspace-root"};
    TempDir outside{"workspace-outside"};
    std::error_code ec;
    const auto link = root.path() / ".music-service-work";
    fs::create_directory_symlink(outside.path(), link, ec);
#ifdef _WIN32
    if (ec) { // junctions need no privilege
        const auto command = L"cmd /c mklink /J \"" + link.native() + L"\" \"" +
                             outside.path().native() + L"\" >NUL";
        if (::_wsystem(command.c_str()) == 0) {
            ec.clear();
        }
    }
#endif
    if (ec) {
        SKIP("creating directory symlinks needs extra privileges on this system");
    }
    const auto workspace = task_workspace(root.path(), "tsk_abc123");
    REQUIRE_FALSE(workspace);
    CHECK(workspace.error().code == domain::ErrorCode::validation_failed);
    CHECK(fs::is_empty(outside.path()));
}
