#include "service_harness.hpp"

#include "infrastructure/clock.hpp"
#include "infrastructure/storage/sqlite/backup.hpp"
#include "infrastructure/storage/sqlite/connection.hpp"
#include "infrastructure/storage/sqlite/database.hpp"
#include "infrastructure/storage/sqlite/schema.hpp"

#include <fstream>

using namespace music_service;
using namespace music_service::test;
namespace fs = std::filesystem;
namespace sqlite = infrastructure::storage::sqlite;

namespace {

std::string utf8(const fs::path& path) { return infrastructure::filesystem::path_to_utf8(path); }

std::vector<fs::path> files_in(const fs::path& directory, std::string_view prefix) {
    std::vector<fs::path> found;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(directory, ec)) {
        if (utf8(entry.path().filename()).rfind(prefix, 0) == 0) {
            found.push_back(entry.path());
        }
    }
    return found;
}

// Runs one statement on a database file outside the service.
void exec_on(const fs::path& file, const std::string& sql) {
    auto db = sqlite::Connection::open(file, sqlite::Connection::Mode::read_write_create);
    REQUIRE(db);
    REQUIRE(db->exec(sql));
}

} // namespace

TEST_CASE("a backup taken while serving restores offline, keeping the replaced database", "[server][backup]") {
    Deployment d{"backup-restore"};
    REQUIRE(d.run({"init"}).code == server::exit_ok);
    REQUIRE(d.run({"principal", "create", "--name", "第一个"}).code == server::exit_ok);
    const auto backup = d.dir / "out" / "备份.sqlite3";

    {
        RunningService service{d, std::make_shared<infrastructure::observability::MemorySink>()};
        // The service holds the instance lock; the backup does not need it.
        const auto saved = d.run({"backup", "--output", utf8(backup)});
        INFO(saved.err);
        REQUIRE(saved.code == server::exit_ok);
        CHECK(saved.out.find("schema_version=" + std::to_string(sqlite::current_schema_version)) != std::string::npos);
        // Restoring needs the lock: refused while serving, nothing changed.
        const auto locked = d.run({"restore", "--input", utf8(backup)});
        CHECK(locked.code == server::exit_failed);
        CHECK(locked.err.find("INSTANCE_LOCKED") != std::string::npos);
        auto api = service.client(d.read(d.bootstrap()));
        CHECK(api.Get("/api/v1/info")->status == 200);
    }
    const auto info = sqlite::verify_backup(backup);
    REQUIRE(info);
    CHECK(info->schema_version == sqlite::current_schema_version);
    CHECK_FALSE(fs::exists(d.dir / "out" / "备份.sqlite3.partial"));
    // An existing file is never replaced.
    const auto again = d.run({"backup", "--output", utf8(backup)});
    CHECK(again.code == server::exit_failed);
    CHECK(again.err.find("CONFLICT") != std::string::npos);

    // A change after the backup, then the restore brings the old state back.
    REQUIRE(d.run({"principal", "create", "--name", "第二个"}).code == server::exit_ok);
    CHECK(d.run({"principal", "list"}).out.find("第二个") != std::string::npos);
    const auto restored = d.run({"restore", "--input", utf8(backup)});
    INFO(restored.err);
    REQUIRE(restored.code == server::exit_ok);
    CHECK(restored.out.find("the replaced database is kept at") != std::string::npos);
    const auto listed = d.run({"principal", "list"}).out;
    CHECK(listed.find("第一个") != std::string::npos);
    CHECK(listed.find("第二个") == std::string::npos);

    // The replaced database is a verified copy with the later change: restoring it undoes
    // the restore.
    const auto replaced = files_in(d.dir / "data" / "backups", "replaced-");
    REQUIRE(replaced.size() == 1);
    REQUIRE(sqlite::verify_backup(replaced[0]));
    REQUIRE(d.run({"restore", "--input", utf8(replaced[0])}).code == server::exit_ok);
    CHECK(d.run({"principal", "list"}).out.find("第二个") != std::string::npos);

    // The restored database serves again.
    RunningService service{d, std::make_shared<infrastructure::observability::MemorySink>()};
    CHECK(service.client(d.read(d.bootstrap())).Get("/api/v1/info")->status == 200);
}

TEST_CASE("unusable backups are refused and change nothing", "[server][backup]") {
    Deployment d{"backup-refused"};
    REQUIRE(d.run({"init"}).code == server::exit_ok);
    REQUIRE(d.run({"principal", "create", "--name", "留下"}).code == server::exit_ok);
    const auto before = d.run({"principal", "list"}).out;

    const auto garbage = d.dir / "out" / "garbage.sqlite3";
    std::ofstream{garbage, std::ios::binary} << std::string(8192, 'x');
    const auto missing = d.dir / "out" / "missing.sqlite3";
    const auto newer = d.dir / "out" / "newer.sqlite3";
    REQUIRE(d.run({"backup", "--output", utf8(newer)}).code == server::exit_ok);
    exec_on(newer, "UPDATE schema_metadata SET version = 99");
    const auto altered = d.dir / "out" / "altered.sqlite3";
    REQUIRE(d.run({"backup", "--output", utf8(altered)}).code == server::exit_ok);
    exec_on(altered, "UPDATE schema_metadata SET checksum = 'changed'");

    for (const auto& input : {garbage, missing, newer, altered}) {
        INFO(utf8(input));
        const auto refused = d.run({"restore", "--input", utf8(input)});
        CHECK(refused.code == server::exit_failed);
        CHECK(d.run({"principal", "list"}).out == before);
    }
    CHECK(files_in(d.dir / "data" / "backups", "replaced-").empty());
    CHECK(files_in(d.dir / "data", "service.sqlite3.restoring").empty());
    CHECK(d.run({"restore"}).code == server::exit_usage);
    CHECK(d.run({"backup", "--output", utf8(garbage), "--extra", "1"}).code == server::exit_usage);
}

TEST_CASE("startup refuses non-current databases without converting or replacing data", "[server][backup][schema]") {
    Deployment d{"non-current-schema"};
    REQUIRE(d.run({"init"}).code == server::exit_ok);
    REQUIRE(d.run({"principal", "create", "--name", "kept"}).code == server::exit_ok);
    const auto file = d.dir / "data" / "service.sqlite3";
    SECTION("older version") { exec_on(file, "UPDATE schema_metadata SET version = 1"); }
    SECTION("missing metadata") { exec_on(file, "DROP TABLE schema_metadata"); }
    SECTION("empty metadata") { exec_on(file, "DELETE FROM schema_metadata"); }
    const auto failed = d.run({"init"});
    CHECK(failed.code == server::exit_failed);
    CHECK((failed.err.find("current schema") != std::string::npos || failed.err.find("differs from this build") != std::string::npos));
    auto db = sqlite::Connection::open(file, sqlite::Connection::Mode::read_only);
    REQUIRE(db);
    auto kept = db->prepare("SELECT count(*) FROM principals WHERE display_name = 'kept'");
    REQUIRE(kept);
    REQUIRE(*kept->step());
    CHECK(kept->integer(0) == 1);
    CHECK_FALSE(sqlite::schema_version(*db));
    CHECK(files_in(d.dir / "data" / "backups", "pre-migration-").empty());
}
