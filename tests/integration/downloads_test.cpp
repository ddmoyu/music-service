// Download intents, commands, queries and batches on real SQLite: idempotency,
// duplicate handling, completed-result reuse with re-checks, retry, cancel, paging and
// all-or-nothing batches (A01-A03, A35-A37, A46), and the cleanup of work directories
// and expired idempotency records.

#include "../unit/providers/mock_providers.hpp"

#include "application/accounts/credential_service.hpp"
#include "application/tasks/download_service.hpp"
#include "downloads/maintenance.hpp"
#include "infrastructure/clock.hpp"
#include "infrastructure/filesystem/destination_files.hpp"
#include "infrastructure/filesystem/paths.hpp"
#include "infrastructure/observability/correlation.hpp"
#include "infrastructure/secrets/file_secret_store.hpp"
#include "infrastructure/storage/sqlite/account_repositories.hpp"
#include "infrastructure/storage/sqlite/migrations.hpp"
#include "infrastructure/storage/sqlite/task_repositories.hpp"
#include "providers/registry/provider_registry.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

using namespace music_service;
using namespace music_service::test;
using namespace music_service::application::tasks;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace sqlite = infrastructure::storage::sqlite;
using domain::ErrorCode;
using domain::TaskStatus;

namespace {

struct Downloads {
    explicit Downloads(std::string_view name, sqlite::TaskRepositoryOptions options = {}, std::uint32_t batch_limit = 3)
        : dir(fs::temp_directory_path() / infrastructure::filesystem::path_from_utf8("music-service-下载-" +
                                                                                     std::string{name})) {
        fs::remove_all(dir);
        fs::create_directories(dir / "secrets");
        database = sqlite::Database::open(dir / "service.sqlite3", {}).value();
        REQUIRE(sqlite::migrate(*database, clock->now()).has_value());
        const sqlite::RepositoryContext ctx{database.get(), clock, ids};
        identities = std::make_shared<sqlite::SqliteIdentityRepository>(ctx);
        destinations = std::make_shared<sqlite::SqliteDestinationRepository>(ctx);
        tasks = std::make_shared<sqlite::SqliteTaskRepository>(ctx, options);
        publications = std::make_shared<sqlite::SqlitePublicationRepository>(ctx);
        for (const auto* id : {"dst_main", "dst_closed", "dst_private", "dst_offline"}) {
            REQUIRE(destinations->bind(id, std::string{"/music/"} + id));
        }
        owner = unwrap(identities->create_principal("owner", ports::PrincipalRole::user, std::nullopt)).id;
        other = unwrap(identities->create_principal("other", ports::PrincipalRole::user, std::nullopt)).id;

        std::map<std::string, application::config::ProviderConfig> config;
        config["off"].enabled = false;
        registry = unwrap(providers::ProviderRegistry::build(
            {mock_combo("both"), mock_combo("off"), mock_catalog("cat"), mock_audio("aud")}, config));
        credentials = std::make_shared<application::accounts::CredentialService>(
            std::make_shared<sqlite::SqliteCredentialRepository>(ctx),
            std::make_shared<infrastructure::secrets::FileSecretStore>(dir / "secrets"), registry, clock);
        std::vector<DestinationRule> rules{{"dst_main", "/music/dst_main", {owner, other}, true},
                                           {"dst_closed", "/music/dst_closed", {owner}, false},
                                           {"dst_private", "/music/dst_private", {other}, true},
                                           {"dst_offline", "/music/dst_offline", {owner}, true}};
        service = std::make_shared<DownloadService>(
            tasks, registry, credentials, rules,
            [](const DestinationRule& rule) { return rule.id != "dst_offline"; },
            [this](const std::string& destination, const std::string& path, std::uint64_t size) {
                const std::lock_guard lock{files_mutex};
                const auto found = files.find(destination + "|" + path);
                return found != files.end() && found->second == size;
            },
            batch_limit);
    }
    ~Downloads() {
        service.reset();
        credentials.reset();
        database.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    DownloadRequest request(std::string_view source_name = "both", std::string_view id = "1") const {
        return DownloadRequest{track(source_name, id), {}, false, false, std::nullopt, std::nullopt,
                               domain::MetadataPolicy::best_effort, false};
    }
    std::string key(int n) const { return "idempotency-key-" + std::to_string(n); }

    CreateResult create(const DownloadRequest& r, int key_number, std::string hash = "h") {
        return unwrap(service->create(owner, r, key(key_number), std::move(hash)));
    }
    domain::Error create_error(const DownloadRequest& r, int key_number, std::string hash = "h") {
        auto result = service->create(owner, r, key(key_number), std::move(hash));
        REQUIRE_FALSE(result.has_value());
        return std::move(result).error();
    }

    ports::TaskRecord step(const ports::TaskRecord& task, TaskStatus to) {
        return unwrap(tasks->transition({task.task_id, *task.execution_id, task.version, to, std::nullopt, std::nullopt,
                                         std::nullopt}));
    }
    // Drives the oldest queued task to processing.
    ports::TaskRecord running() {
        auto task = *unwrap(tasks->claim_next(clock->now()));
        task = step(task, TaskStatus::downloading);
        return step(task, TaskStatus::processing);
    }
    // Completes the oldest queued task with a file at path.
    ports::FileAssetRecord complete(const std::string& path) {
        auto task = running();
        auto finalizing = unwrap(publications->begin(
            {task.task_id, *task.execution_id, "dst_main", ".work/" + task.task_id, path, "{}", "reserved"},
            task.version));
        auto done = unwrap(publications->complete(task.task_id, *task.execution_id, finalizing.version,
                                                  {owner, "dst_main", path, 100, "mp3", "etag", "{}"}, "{}"));
        const std::lock_guard lock{files_mutex};
        files["dst_main|" + path] = 100;
        return done.asset;
    }

    fs::path dir;
    std::shared_ptr<infrastructure::ManualClock> clock = std::make_shared<infrastructure::ManualClock>();
    std::shared_ptr<infrastructure::observability::IdGenerator> ids =
        std::make_shared<infrastructure::observability::IdGenerator>(11);
    std::unique_ptr<sqlite::Database> database;
    std::shared_ptr<sqlite::SqliteIdentityRepository> identities;
    std::shared_ptr<sqlite::SqliteDestinationRepository> destinations;
    std::shared_ptr<sqlite::SqliteTaskRepository> tasks;
    std::shared_ptr<sqlite::SqlitePublicationRepository> publications;
    std::shared_ptr<const providers::ProviderRegistry> registry;
    std::shared_ptr<application::accounts::CredentialService> credentials;
    std::shared_ptr<DownloadService> service;
    std::string owner;
    std::string other;
    std::mutex files_mutex;
    std::map<std::string, std::uint64_t> files; // "destination|path" -> size on disk
};

std::size_t total_tasks(Downloads& d, const std::string& owner) {
    return unwrap(d.tasks->list({owner, std::nullopt, std::nullopt, std::nullopt, 1000, std::nullopt})).size();
}

} // namespace

TEST_CASE("a 230 item collection submits as 100 100 30 and keeps batch atomicity", "[downloads][collections-b]") {
    Downloads d{"collection-230", {}, 100};
    std::ifstream file(fs::path{MUSIC_SERVICE_TEST_FIXTURES} / "collections/230-tracks.json");
    const auto fixture = nlohmann::json::parse(file);
    std::vector<DownloadRequest> items;
    for (const auto& item : fixture["items"]) items.push_back(d.request(item["source"].get<std::string>(), item["id"].get<std::string>()));
    REQUIRE(items.size() == 230);
    std::size_t offset = 0;
    for (const std::size_t count : {100U, 100U, 30U}) {
        const std::vector<DownloadRequest> page(items.begin() + static_cast<std::ptrdiff_t>(offset),
                                              items.begin() + static_cast<std::ptrdiff_t>(offset + count));
        const auto key = "collection-batch-" + std::to_string(offset);
        const auto result = unwrap(d.service->create_batch(d.owner, page, key, key));
        CHECK(result.created_count == count);
        REQUIRE(result.items.size() == count);
        for (std::size_t i = 0; i < count; ++i) CHECK(result.items[i].intent.original_track() == items[offset + i].track);
        const auto replay = unwrap(d.service->create_batch(d.owner, page, key, key));
        CHECK(replay.replayed);
        CHECK(replay.batch_id == result.batch_id);
        CHECK(d.service->create_batch(d.owner, page, key, "changed").error().code == ErrorCode::idempotency_conflict);
        offset += count;
        CHECK(total_tasks(d, d.owner) == offset);
    }
    CHECK(d.service->create_batch(d.owner, std::vector<DownloadRequest>(items.begin(), items.begin() + 101),
                                  "collection-too-large", "101").error().code == ErrorCode::validation_failed);
    const auto invalid = d.service->create_batch(d.owner, {d.request("both", "new"), d.request("missing", "1")},
                                                  "collection-bad-item", "bad");
    CHECK_FALSE(invalid);
    CHECK(total_tasks(d, d.owner) == 230);
}

TEST_CASE("idempotency keys replay the first outcome without redoing the checks", "[downloads]") {
    Downloads d{"idempotency"};
    const auto cookie = unwrap(d.credentials->create({d.owner, "aud", "", ports::CredentialKind::cookie, "web_cookie",
                                                      ports::SecretBytes{"a=1"}, std::nullopt}))
                            .id;
    auto request = d.request("aud");
    request.credential_id = cookie;

    // A01: the same key and request give the same task.
    const auto first = d.create(request, 1);
    CHECK(first.status_code == 202);
    CHECK_FALSE(first.reused);
    CHECK_FALSE(first.replayed);
    CHECK(first.view.task.status == TaskStatus::queued);
    CHECK(first.view.intent.account_bindings().front().credential_id() == cookie);

    // A37: the replay does not re-read state that changed meanwhile (the credential is
    // gone now and a fresh check would refuse).
    REQUIRE(d.credentials->revoke(d.owner, cookie));
    const auto replay = d.create(request, 1);
    CHECK(replay.replayed);
    CHECK(replay.status_code == 202);
    CHECK(replay.view.task.task_id == first.view.task.task_id);
    CHECK(total_tasks(d, d.owner) == 1);
    // The replay is the first response: queued, though the task is canceled by now.
    REQUIRE(d.service->cancel(d.owner, first.view.task.task_id));
    const auto after_cancel = d.create(request, 1);
    CHECK(after_cancel.replayed);
    CHECK(after_cancel.status_code == 202);
    CHECK(after_cancel.view.task.status == TaskStatus::queued);
    CHECK(unwrap(d.service->get(d.owner, first.view.task.task_id)).task.status == TaskStatus::canceled);

    // A02: the same key with another request is a conflict with no side effect.
    CHECK(d.create_error(request, 1, "other").code == ErrorCode::idempotency_conflict);
    CHECK(total_tasks(d, d.owner) == 1);
    // Keys are per principal.
    CHECK(d.service->create(d.other, d.request(), d.key(1), "h").has_value());

    // Malformed keys.
    CHECK(d.service->create(d.owner, d.request(), "short", "h").error().code == ErrorCode::validation_failed);
    CHECK(d.service->create(d.owner, d.request(), std::string(129, 'k'), "h").error().code ==
          ErrorCode::validation_failed);
    CHECK(d.service->create(d.owner, d.request(), "has spaces in the key!", "h").error().code ==
          ErrorCode::validation_failed);

    // Records expire with idempotency.retention; the key can then be used anew.
    d.clock->advance(25h);
    const auto fresh = d.create(d.request("both", "2"), 1, "after-expiry");
    CHECK_FALSE(fresh.replayed);
}

TEST_CASE("equal active intents are shared, force is refused", "[downloads]") {
    Downloads d{"active"};
    const auto first = d.create(d.request(), 1);

    // A03: another key for the same intent reuses the active task.
    const auto again = d.create(d.request(), 2);
    CHECK(again.reused);
    CHECK(again.status_code == 200);
    CHECK(again.view.task.task_id == first.view.task.task_id);

    auto forced = d.request();
    forced.force = true;
    CHECK(d.create_error(forced, 3).code == ErrorCode::active_duplicate);

    // Concurrent submissions of one new intent: one task, the rest reuse it.
    std::atomic<int> created{0};
    std::atomic<int> reused{0};
    std::vector<std::jthread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&, i] {
            auto result = d.service->create(d.owner, d.request("both", "race"), d.key(100 + i), "h");
            if (result) {
                (result->reused ? reused : created)++;
            }
        });
    }
    threads.clear();
    CHECK(created == 1);
    CHECK(reused == 7);
}

TEST_CASE("different product or content rules are different downloads", "[downloads]") {
    Downloads d{"intents"};
    // A35: metadata and preview rules are part of the intent.
    auto skip = d.request();
    skip.metadata_policy = domain::MetadataPolicy::skip;
    auto required = d.request();
    required.metadata_policy = domain::MetadataPolicy::required;
    auto preview = d.request();
    preview.requirement.allow_preview = true;
    const auto a = d.create(skip, 1);
    const auto b = d.create(required, 2);
    const auto c = d.create(preview, 3);
    const auto plain = d.create(d.request(), 4);
    const std::set<std::string> ids{a.view.task.task_id, b.view.task.task_id, c.view.task.task_id,
                                    plain.view.task.task_id};
    CHECK(ids.size() == 4);
    CHECK_FALSE(b.reused);

    // Omitted defaults and explicit defaults are one intent (the request hash differs).
    auto explicit_defaults = d.request();
    explicit_defaults.requirement = {domain::Quality::standard, domain::QualityPolicy::strict, false};
    explicit_defaults.destination_id = "dst_main";
    CHECK(d.create(explicit_defaults, 5, "explicit").view.task.task_id == plain.view.task.task_id);

    // A36: manual targets are stable, verified refs (match handles never enter the intent).
    const auto binding = anonymous(d.owner, "both");
    const auto target_binding = anonymous(d.owner, "aud");
    const auto intent_for = [&](std::string_view target_id) {
        return unwrap(domain::NormalizedDownloadIntent::normalize(
            {d.owner, track("both", "x"), domain::ManualTarget{track("aud", target_id)}, {binding, target_binding},
             {}, domain::MetadataPolicy::best_effort, {"dst_main", "v", "n", "m"}}));
    };
    CHECK(domain::fingerprint(intent_for("t1")) == domain::fingerprint(intent_for("t1")));
    CHECK_FALSE(domain::fingerprint(intent_for("t1")) == domain::fingerprint(intent_for("t2")));
    const auto first = unwrap(d.tasks->create({intent_for("t1"), d.key(10), "h"}));
    const auto second = unwrap(d.tasks->create({intent_for("t2"), d.key(11), "h"}));
    const auto same = unwrap(d.tasks->create({intent_for("t1"), d.key(12), "h"}));
    CHECK(first.task.task_id != second.task.task_id);
    CHECK(same.reused);
    CHECK(same.task.task_id == first.task.task_id);
    const auto stored = unwrap(d.tasks->get_intent(first.task.task_id));
    CHECK(std::get<domain::ManualTarget>(stored.selection()).target == track("aud", "t1"));
}

TEST_CASE("new quality and lyric semantics do not reuse older completed products", "[downloads][stage-e]") {
    Downloads d{"old-product-version"};
    const auto current = d.create(d.request(), 1);
    REQUIRE(d.service->cancel(d.owner, current.view.task.task_id));
    const auto& i = current.view.intent;
    auto old_product = i.product();
    old_product.metadata_config_version = "metadata-v1";
    auto old_intent = unwrap(domain::NormalizedDownloadIntent::normalize({i.owner_id(), i.original_track(),
        i.selection(), i.account_bindings(), i.requirement(), i.metadata_policy(), old_product}));
    const auto old = unwrap(d.tasks->create({std::move(old_intent), d.key(2), "old", false}));
    (void)d.complete("old.mp3");
    const auto fresh = d.create(d.request(), 3);
    CHECK_FALSE(fresh.reused);
    CHECK(fresh.view.task.task_id != old.task.task_id);
    CHECK(fresh.view.task.status == TaskStatus::queued);
    const auto replay = d.create(d.request(), 1);
    CHECK(replay.replayed);
    CHECK(replay.view.task.task_id == current.view.task.task_id);
}

TEST_CASE("completed results are reused only while they still hold", "[downloads]") {
    Downloads d{"completed"};
    const auto first = d.create(d.request(), 1);
    const auto asset = d.complete("歌手/Song.mp3");
    CHECK(unwrap(d.service->get(d.owner, first.view.task.task_id)).task.status == TaskStatus::completed);

    // The file exists and nothing changed: reused.
    const auto reused = d.create(d.request(), 2);
    CHECK(reused.reused);
    CHECK(reused.status_code == 200);
    CHECK(reused.view.task.task_id == first.view.task.task_id);

    // force skips completed results.
    auto forced = d.request();
    forced.force = true;
    const auto forced_task = d.create(forced, 3);
    CHECK_FALSE(forced_task.reused);
    REQUIRE(d.service->cancel(d.owner, forced_task.view.task.task_id));

    // A46: the file was rewritten to another size: not the published file, a new task.
    {
        const std::lock_guard lock{d.files_mutex};
        d.files["dst_main|歌手/Song.mp3"] = 99;
    }
    const auto rewritten = d.create(d.request(), 4);
    CHECK_FALSE(rewritten.reused);
    CHECK(rewritten.view.task.task_id != first.view.task.task_id);
    REQUIRE(d.service->cancel(d.owner, rewritten.view.task.task_id));

    // A46: the file is gone: no reused=true, a new task instead.
    {
        const std::lock_guard lock{d.files_mutex};
        d.files.clear();
    }
    const auto replaced = d.create(d.request(), 40);
    CHECK_FALSE(replaced.reused);
    CHECK(replaced.view.task.task_id != first.view.task.task_id);
    REQUIRE(d.service->cancel(d.owner, replaced.view.task.task_id));

    // A46: the asset changed after the outside check: the transaction refuses the
    // stale candidate and creates a new task.
    const auto results = unwrap(d.tasks->completed_results(d.owner, first.view.task.fingerprint));
    REQUIRE(results.size() == 1);
    const auto candidate = results.front().candidate;
    REQUIRE(d.publications->mark_missing(asset.id));
    const auto intent = unwrap(d.tasks->get_intent(first.view.task.task_id));
    const auto stale = unwrap(d.tasks->create({intent, d.key(5), "h", false, "/api/v1/downloads", std::nullopt, candidate}));
    CHECK_FALSE(stale.reused);
    CHECK(stale.task.task_id != first.view.task.task_id);
}

TEST_CASE("requests are checked before anything is stored", "[downloads]") {
    Downloads d{"checks"};
    CHECK(d.create_error(d.request("ghost"), 1).code == ErrorCode::unknown_provider);
    CHECK(d.create_error(d.request("cat"), 2).code == ErrorCode::capability_unsupported); // no audio
    CHECK(d.create_error(d.request("off"), 3).code == ErrorCode::provider_disabled);
    CHECK(d.create_error(d.request("aud"), 4).code == ErrorCode::credentials_required);
    auto fallback = d.request();
    fallback.automatic_fallback = true;
    CHECK(d.create_error(fallback, 5).code == ErrorCode::capability_unsupported);
    auto matched = d.request();
    matched.match_selection = true;
    CHECK(d.create_error(matched, 6).code == ErrorCode::capability_unsupported);

    auto destination = d.request();
    destination.destination_id = "dst_private"; // not this principal's
    CHECK(d.create_error(destination, 7).code == ErrorCode::not_found);
    destination.destination_id = "dst_unknown";
    CHECK(d.create_error(destination, 8).code == ErrorCode::not_found);
    destination.destination_id = "dst_closed";
    CHECK(d.create_error(destination, 9).code == ErrorCode::destination_unavailable);
    destination.destination_id = "dst_offline";
    CHECK(d.create_error(destination, 10).code == ErrorCode::destination_unavailable);
    CHECK(total_tasks(d, d.owner) == 0);
}

TEST_CASE("active task quotas apply per principal", "[downloads]") {
    Downloads d{"quota", {2, 100, std::chrono::hours{24}}};
    (void)d.create(d.request("both", "1"), 1);
    (void)d.create(d.request("both", "2"), 2);
    CHECK(d.create_error(d.request("both", "3"), 3).code == ErrorCode::queue_limit_exceeded);
    CHECK(d.service->create(d.other, d.request("both", "3"), d.key(3), "h").has_value());
}

TEST_CASE("cancel follows the task state", "[downloads]") {
    Downloads d{"cancel"};
    const auto running = d.create(d.request("both", "run"), 1);
    const auto task = d.running();
    REQUIRE(task.task_id == running.view.task.task_id);
    const auto queued = d.create(d.request("both", "wait"), 2);

    const auto now = unwrap(d.service->cancel(d.owner, queued.view.task.task_id));
    CHECK(now.view.task.status == TaskStatus::canceled);
    CHECK_FALSE(now.asynchronous);
    const auto later = unwrap(d.service->cancel(d.owner, running.view.task.task_id));
    CHECK(later.view.task.status == TaskStatus::canceling);
    CHECK(later.asynchronous);
    CHECK(unwrap(d.service->cancel(d.owner, running.view.task.task_id)).view.task.version == later.view.task.version);
    CHECK(d.service->cancel(d.other, queued.view.task.task_id).error().code == ErrorCode::not_found);

    (void)d.create(d.request("both", "done"), 3);
    (void)d.complete("a/b.mp3");
    const auto finished = unwrap(d.tasks->list({d.owner, TaskStatus::completed, std::nullopt, std::nullopt, 10, std::nullopt}));
    CHECK(d.service->cancel(d.owner, finished.front().task_id).error().code == ErrorCode::task_terminal);
}

TEST_CASE("retry creates a new task from the stored intent", "[downloads]") {
    Downloads d{"retry"};
    const auto cookie = unwrap(d.credentials->create({d.owner, "aud", "", ports::CredentialKind::cookie, "web_cookie",
                                                      ports::SecretBytes{"a=1"}, std::nullopt}))
                            .id;
    REQUIRE(d.credentials->set_default(d.owner, "aud", cookie));
    const auto original = d.create(d.request("aud"), 1);
    CHECK(d.service->retry(d.owner, original.view.task.task_id, d.key(2), "retry").error().code == ErrorCode::conflict);
    REQUIRE(d.service->cancel(d.owner, original.view.task.task_id));

    const auto retried = unwrap(d.service->retry(d.owner, original.view.task.task_id, d.key(2), "retry"));
    CHECK(retried.status_code == 202);
    CHECK(retried.view.task.retry_of == original.view.task.task_id);
    CHECK(retried.view.task.task_id != original.view.task.task_id);
    CHECK(unwrap(d.service->get(d.owner, original.view.task.task_id)).task.status == TaskStatus::canceled);
    // Same binding as the original: the stored intent, not today's default.
    CHECK(retried.view.intent.account_bindings() == original.view.intent.account_bindings());
    // Replay of the retry key.
    const auto replay = unwrap(d.service->retry(d.owner, original.view.task.task_id, d.key(2), "retry"));
    CHECK(replay.replayed);
    CHECK(replay.view.task.task_id == retried.view.task.task_id);
    CHECK(replay.view.task.status == TaskStatus::queued);
    // The retried intent is active: a second retry is a duplicate.
    CHECK(d.service->retry(d.owner, original.view.task.task_id, d.key(3), "retry").error().code ==
          ErrorCode::active_duplicate);

    // Retrying needs the bound account to still be usable; no other account is chosen.
    REQUIRE(d.service->cancel(d.owner, retried.view.task.task_id));
    const auto canceled_replay = unwrap(d.service->retry(d.owner, original.view.task.task_id, d.key(2), "retry"));
    CHECK(canceled_replay.replayed);
    CHECK(canceled_replay.view.task.status == TaskStatus::queued);
    REQUIRE(d.credentials->revoke(d.owner, cookie));
    CHECK(d.service->retry(d.owner, original.view.task.task_id, d.key(4), "retry").error().code ==
          ErrorCode::credentials_required);
    CHECK(d.service->retry(d.other, original.view.task.task_id, d.key(5), "retry").error().code == ErrorCode::not_found);
}

TEST_CASE("task lists page newest first with filter-bound cursors", "[downloads]") {
    Downloads d{"list"};
    std::vector<std::string> created;
    for (int i = 0; i < 5; ++i) {
        created.push_back(d.create(d.request("both", std::to_string(i)), i).view.task.task_id);
        d.clock->advance(1s);
    }
    std::vector<std::string> seen;
    TaskFilter filter;
    filter.limit = 2;
    while (true) {
        const auto page = unwrap(d.service->list(d.owner, filter));
        for (const auto& item : page.items) {
            seen.push_back(item.task.task_id);
        }
        if (!page.next_cursor) {
            break;
        }
        filter.cursor = page.next_cursor;
    }
    std::reverse(created.begin(), created.end());
    CHECK(seen == created);

    REQUIRE(d.service->cancel(d.owner, created.front()));
    TaskFilter canceled;
    canceled.status = TaskStatus::canceled;
    const auto only = unwrap(d.service->list(d.owner, canceled));
    REQUIRE(only.items.size() == 1);
    CHECK(only.items.front().task.task_id == created.front());

    TaskFilter first_page;
    first_page.limit = 2;
    auto cursor = unwrap(d.service->list(d.owner, first_page)).next_cursor;
    TaskFilter other_filter = canceled;
    other_filter.cursor = cursor;
    CHECK(d.service->list(d.owner, other_filter).error().code == ErrorCode::cursor_mismatch);
    first_page.cursor = cursor;
    CHECK(d.service->list(d.other, first_page).error().code == ErrorCode::cursor_mismatch);
    CHECK(unwrap(d.service->list(d.other, {})).items.empty());
    TaskFilter too_many;
    too_many.limit = 101;
    CHECK(d.service->list(d.owner, too_many).error().code == ErrorCode::validation_failed);
}

TEST_CASE("batches are all or nothing and keep their items", "[downloads]") {
    Downloads d{"batches"};
    const auto existing = d.create(d.request("both", "old"), 1);

    // New, duplicate within the batch, and an already active intent.
    const std::vector<DownloadRequest> items{d.request("both", "a"), d.request("both", "a"), d.request("both", "old")};
    const auto batch = unwrap(d.service->create_batch(d.owner, items, d.key(2), "batch"));
    CHECK(batch.status_code == 202);
    CHECK(batch.created_count == 1);
    CHECK(batch.reused_count == 2);
    REQUIRE(batch.items.size() == 3);
    CHECK(batch.items[0].task.task_id == batch.items[1].task.task_id);
    CHECK(batch.items[2].task.task_id == existing.view.task.task_id);
    CHECK(batch.reused == std::vector<bool>{false, true, true});

    const auto replay = unwrap(d.service->create_batch(d.owner, items, d.key(2), "batch"));
    CHECK(replay.replayed);
    CHECK(replay.batch_id == batch.batch_id);
    CHECK(replay.reused == batch.reused);
    CHECK(d.service->create_batch(d.owner, items, d.key(2), "different").error().code == ErrorCode::idempotency_conflict);

    const auto view = unwrap(d.service->get_batch(d.owner, batch.batch_id));
    REQUIRE(view.items.size() == 3); // duplicates kept in order
    CHECK(d.service->get_batch(d.other, batch.batch_id).error().code == ErrorCode::not_found);
    TaskFilter by_batch;
    by_batch.batch_id = batch.batch_id;
    CHECK(unwrap(d.service->list(d.owner, by_batch)).items.size() == 2); // each task once

    // One bad item: nothing is created; every failure is listed, the first decides.
    const auto before = total_tasks(d, d.owner);
    const std::vector<DownloadRequest> bad{d.request("both", "x"), d.request("cat", "y"), d.request("ghost", "z")};
    const auto refused = d.service->create_batch(d.owner, bad, d.key(3), "bad");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ErrorCode::capability_unsupported);
    REQUIRE(refused.error().details.size() == 2);
    CHECK(refused.error().details[0].field == "items[1]");
    CHECK(refused.error().details[1].field == "items[2]");
    CHECK(total_tasks(d, d.owner) == before);
    CHECK(d.service->create_batch(d.owner, {d.request(), d.request(), d.request(), d.request()}, d.key(4), "h")
              .error()
              .code == ErrorCode::validation_failed); // more than download_limits.max_batch_items
    CHECK(d.service->create_batch(d.owner, {}, d.key(5), "h").error().code == ErrorCode::validation_failed);

    // Everything already exists: 200.
    const auto all_reused = unwrap(d.service->create_batch(d.owner, {d.request("both", "a")}, d.key(6), "again"));
    CHECK(all_reused.status_code == 200);
    CHECK(all_reused.created_count == 0);

    // Cancel: each task once; the queued ones are canceled at once.
    const auto canceled = unwrap(d.service->cancel_batch(d.owner, batch.batch_id));
    CHECK(canceled.accepted.size() == 2);
    CHECK(canceled.refused.empty());
    CHECK_FALSE(canceled.asynchronous);
    for (const auto& item : canceled.accepted) {
        CHECK(item.task.status == TaskStatus::canceled);
    }
    // The replay still answers with the first statuses.
    const auto replay_after_cancel = unwrap(d.service->create_batch(d.owner, items, d.key(2), "batch"));
    CHECK(replay_after_cancel.replayed);
    CHECK(replay_after_cancel.status_code == 202);
    REQUIRE(replay_after_cancel.items.size() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(replay_after_cancel.items[i].task.status == batch.items[i].task.status);
    }
    CHECK(replay_after_cancel.items[0].task.status == TaskStatus::queued);
}

TEST_CASE("a lowered batch limit still replays committed batches", "[downloads]") {
    Downloads d{"batch-limit"};
    const std::vector<DownloadRequest> items{d.request("both", "1"), d.request("both", "2")};
    const auto batch = unwrap(d.service->create_batch(d.owner, items, d.key(1), "two"));
    CHECK(batch.created_count == 2);

    // download_limits.max_batch_items lowered from 3 to 1 after the commit.
    DownloadService narrow{d.tasks, d.registry, d.credentials, {{"dst_main", "/music/dst_main", {d.owner}, true}},
                           {}, {}, 1};
    const auto replay = unwrap(narrow.create_batch(d.owner, items, d.key(1), "two"));
    CHECK(replay.replayed);
    CHECK(replay.batch_id == batch.batch_id);
    CHECK(replay.status_code == 202);
    CHECK(narrow.create_batch(d.owner, items, d.key(1), "other").error().code == ErrorCode::idempotency_conflict);
    // A new request is held to today's limit.
    CHECK(narrow.create_batch(d.owner, items, d.key(2), "two").error().code == ErrorCode::validation_failed);
    CHECK(total_tasks(d, d.owner) == 2);
}

TEST_CASE("a batch over the quota stores nothing", "[downloads]") {
    Downloads d{"batch-quota", {2, 100, std::chrono::hours{24}}};
    const std::vector<DownloadRequest> items{d.request("both", "1"), d.request("both", "2"), d.request("both", "3")};
    const auto refused = d.service->create_batch(d.owner, items, d.key(1), "h");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ErrorCode::queue_limit_exceeded);
    CHECK(refused.error().details.front().field == "items[2]");
    CHECK(total_tasks(d, d.owner) == 0);
}

TEST_CASE("maintenance removes work directories by their task's state, never an active one", "[downloads][maintenance]") {
    Downloads d{"maintenance"};
    const auto root = d.dir / "main";
    fs::create_directories(root);
    auto files = std::make_shared<infrastructure::filesystem::LocalDestinationFiles>(
        std::map<std::string, fs::path>{{"dst_main", root}, {"dst_offline", d.dir / "offline"}});
    const auto workspace = [&](const std::string& task_id) {
        const auto path = root / ".music-service-work" / task_id;
        fs::create_directories(path);
        std::ofstream{path / "audio.part", std::ios::binary} << "partial bytes";
        return path;
    };

    // One task in every situation, each with a work directory. Tasks are driven in
    // creation order, so the one that stays queued comes last.
    const auto completed_task = d.create(d.request("both", "2"), 2).view.task.task_id;
    (void)d.complete("Song.mp3");
    const auto finalizing_task = d.create(d.request("both", "3"), 3).view.task.task_id;
    {
        const auto task = d.running();
        REQUIRE(task.task_id == finalizing_task);
        REQUIRE(d.publications->begin(
            {task.task_id, *task.execution_id, "dst_main", ".work/" + task.task_id, "Other.mp3", "{}", "reserved"},
            task.version));
    }
    const auto failed_task = d.create(d.request("both", "4"), 4).view.task.task_id;
    {
        const auto task = d.running();
        REQUIRE(task.task_id == failed_task);
        ports::TransitionCommand failed{task.task_id, *task.execution_id, task.version, TaskStatus::failed,
                                        std::nullopt, std::nullopt, std::nullopt};
        failed.error_json = R"({"code":"UPSTREAM_FAILED"})";
        REQUIRE(d.tasks->transition(failed));
    }
    const auto canceled_task = d.create(d.request("both", "5"), 5).view.task.task_id;
    REQUIRE(d.service->cancel(d.owner, canceled_task));
    const auto queued = d.create(d.request("both", "1"), 1).view.task.task_id;
    const auto orphan = d.ids->next("tsk_"); // no such task, e.g. after restoring an older database
    for (const auto& id : {queued, completed_task, finalizing_task, failed_task, canceled_task, orphan}) {
        workspace(id);
    }
    fs::create_directories(root / ".music-service-work" / "notes"); // not a task directory
    std::ofstream{root / "Song.mp3", std::ios::binary} << "published";
    REQUIRE(unwrap(files->list_workspaces("dst_main")).size() == 6);

    downloads::Maintenance maintenance{std::make_shared<sqlite::SqliteMaintenanceStore>(
                                           sqlite::RepositoryContext{d.database.get(), d.clock, d.ids}),
                                       files, {"dst_main", "dst_offline"}};
    const auto verdicts = [](const downloads::MaintenanceReport& report) {
        std::map<std::string, downloads::WorkspaceVerdict> out;
        for (const auto& w : report.workspaces) {
            out[w.task_id] = w.verdict;
        }
        return out;
    };
    const auto exists = [&](const std::string& id) { return fs::exists(root / ".music-service-work" / id); };

    // An hour later: finished and unknown tasks lose their directories, the failed one is
    // kept for diagnosis, active ones keep theirs.
    const auto first = unwrap(maintenance.run(d.clock->now() + 1h));
    auto seen = verdicts(first);
    CHECK(seen.at(queued) == downloads::WorkspaceVerdict::kept_active);
    CHECK(seen.at(finalizing_task) == downloads::WorkspaceVerdict::kept_active);
    CHECK(seen.at(failed_task) == downloads::WorkspaceVerdict::kept_retained);
    CHECK(seen.at(completed_task) == downloads::WorkspaceVerdict::removed_finished);
    CHECK(seen.at(canceled_task) == downloads::WorkspaceVerdict::removed_finished);
    CHECK(seen.at(orphan) == downloads::WorkspaceVerdict::removed_orphan);
    CHECK(first.unavailable_destinations == std::vector<std::string>{"dst_offline"});
    CHECK(first.errors.empty());
    CHECK(exists(queued));
    CHECK(fs::exists(root / ".music-service-work" / queued / "audio.part"));
    CHECK(exists(finalizing_task));
    CHECK(exists(failed_task));
    CHECK_FALSE(exists(completed_task));
    CHECK_FALSE(exists(canceled_task));
    CHECK_FALSE(exists(orphan));
    CHECK(fs::is_directory(root / ".music-service-work" / "notes"));
    CHECK(fs::exists(root / "Song.mp3")); // published files are never touched
    CHECK(first.idempotency_purged == 0);        // keys live 24 hours

    // Past the retention the failed task's files go too; the rest is unchanged, and the
    // expired idempotency keys are purged.
    const auto second = unwrap(maintenance.run(d.clock->now() + 25h));
    seen = verdicts(second);
    CHECK(seen.at(failed_task) == downloads::WorkspaceVerdict::removed_expired);
    CHECK(seen.at(queued) == downloads::WorkspaceVerdict::kept_active);
    CHECK(seen.at(finalizing_task) == downloads::WorkspaceVerdict::kept_active);
    CHECK(second.workspaces.size() == 3);
    CHECK_FALSE(exists(failed_task));
    CHECK(second.idempotency_purged == 5);
    // A purged key is free again: the same key and request give a new task.
    CHECK(d.create(d.request("both", "9"), 1).view.task.task_id != queued);

    // Nothing left to do: a third run removes nothing.
    const auto third = unwrap(maintenance.run(d.clock->now() + 26h));
    CHECK(third.count(downloads::WorkspaceVerdict::kept_active) == 2);
    CHECK(third.workspaces.size() == 2);
}
