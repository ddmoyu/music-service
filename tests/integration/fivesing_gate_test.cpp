// T22 without network: the real music-service process with the real 5sing and Kuwo
// packages side by side, upstream answers from the recorded fixtures and a CDN serving
// an MP3 at the tier's primary and backup addresses. 5sing search, detail, lyrics and
// resolve; a durable download through the shared runner to a tagged file; the backup
// address used only when the primary does not answer; and the capability differences
// between the two sources answered by the common routes. The online run is
// packaging/e2e/kuwo_e2e.py --source fivesing. The [t41] case is the second-platform
// gate: two real packages, two principals, one runner and no platform branches.

#include "sse_client.hpp"
#include "two_platforms.hpp"

#include <map>
#include <set>

using namespace music_service;
using namespace music_service::test;
using namespace music_service::test::two_platforms;

TEST_CASE("T22: 5sing next to Kuwo, from search to a published file", "[gate][fivesing]") {
    Deployment d{"fivesing-gate"};
    d.write_config();
    server::CommandHooks hooks;
    hooks.host.bundles = both();
    REQUIRE(d.run({"init"}, hooks).code == server::exit_ok);
    const auto token = d.read(d.bootstrap());
    const auto root = d.dir / "下载";

    const auto audio = mp3(800);
    auto upstream = std::make_shared<Upstream>(audio);
    RunningService service{d, std::make_shared<infrastructure::observability::MemorySink>(), both(), false, upstream};
    auto api = service.client(token);

    // Both sources are registered with their own capabilities.
    const auto listed = body(api.Get("/api/v1/providers"))["data"]["items"];
    REQUIRE(listed.size() == 2);
    const auto five = std::find_if(listed.begin(), listed.end(), [](const json& p) { return p["source"] == "fivesing"; });
    REQUIRE(five != listed.end());
    CHECK((*five)["name"] == "5sing");
    const auto& capabilities = (*five)["capabilities"];
    const auto has = [&](const char* key) { return capabilities.contains(key); };
    CHECK(has("search_tracks"));
    CHECK(has("lyrics"));
    CHECK(has("resolve_audio"));
    CHECK(has("playlist_tracks"));
    CHECK_FALSE(has("search_albums"));

    // One search over both sources: each answers on its own.
    const auto search = api.Get("/api/v1/search/tracks?q=%E5%8D%83%E5%B9%B4%E9%85%92&sources=kuwo,fivesing&limit=5");
    REQUIRE(search->status == 200);
    const auto envelope = body(search);
    const auto& items = envelope["data"]["items"];
    std::set<std::string> sources;
    for (const auto& item : items) {
        sources.insert(item["ref"]["source"].get<std::string>());
    }
    CHECK(sources == std::set<std::string>{"fivesing", "kuwo"});
    CHECK(envelope["meta"]["partial"] == false);
    REQUIRE(envelope["meta"]["source_results"].size() == 2);
    for (const auto& result : envelope["meta"]["source_results"]) {
        INFO(result.dump());
        CHECK(result["status"] == "ok");
    }
    const auto cover = std::find_if(items.begin(), items.end(), [](const json& t) { return t["ref"]["id"] == work; });
    REQUIRE(cover != items.end());
    CHECK((*cover)["title"] == "千年食谱颂");
    CHECK((*cover)["duration_ms"].is_null()); // unknown stays unknown
    CHECK((*cover)["version"]["kind"] == "cover");

    // Detail, lyrics and capability differences through the same routes.
    const auto detail = body(api.Get("/api/v1/tracks/detail?source=fivesing&id=" + work))["data"];
    CHECK(detail["artists"][0]["name"] == "琴酒蜀黍");
    const auto lyrics = api.Get("/api/v1/lyrics?source=fivesing&id=" + work);
    REQUIRE(lyrics->status == 200);
    CHECK(body(lyrics)["data"]["lrc"].get<std::string>().find("[00:03.37]") != std::string::npos);
    const auto kuwo_lyrics = api.Get("/api/v1/lyrics?source=kuwo&id=78932517");
    CHECK(kuwo_lyrics->status == 422);
    CHECK(code_of(kuwo_lyrics) == "CAPABILITY_UNSUPPORTED");
    CHECK(api.Get("/api/v1/tracks/detail?source=fivesing&id=fc-999999999")->status == 404);
    CHECK(api.Get("/api/v1/tracks/detail?source=fivesing&id=16394286")->status == 400); // no type: no work

    const auto resolved =
        api.Post("/api/v1/assets/resolve", R"({"track":{"source":"fivesing","id":"fc-16394286"}})", "application/json");
    REQUIRE(resolved->status == 200);
    const auto asset = body(resolved)["data"];
    CHECK(asset["actual_quality"] == "standard");
    CHECK(asset["bitrate_bps"] == 128000);
    CHECK(asset["completeness"] == "full");
    CHECK(asset["warnings"].empty());
    const auto high = api.Post("/api/v1/assets/resolve",
                               R"({"track":{"source":"fivesing","id":"fc-16394286"},"quality":"high"})",
                               "application/json");
    CHECK(high->status == 422); // this work has no sq tier
    CHECK(code_of(high) == "QUALITY_UNAVAILABLE");

    // The shared runner downloads it into a tagged file named from the detail.
    upstream->clear();
    const auto created = api.Post("/api/v1/downloads", {{"Idempotency-Key", "fivesing-gate-0001"}},
                                  R"({"track":{"source":"fivesing","id":"fc-16394286"}})", "application/json");
    REQUIRE(created->status == 202);
    const auto task_id = body(created)["data"]["task_id"].get<std::string>();
    const auto task = wait_status(api, task_id, "completed");
    REQUIRE(task["status"] == "completed");
    CHECK(task["actual_track"]["id"] == work);
    CHECK(task["actual_quality"] == "standard");
    const auto file = body(api.Get("/api/v1/files/" + task["result"]["file_id"].get<std::string>()))["data"];
    CHECK(file["display_name"] == "琴酒蜀黍 - 千年食谱颂.mp3");
    CHECK(file["metadata_status"] == "complete");
    const auto stored = read_file(root / infrastructure::filesystem::path_from_utf8("琴酒蜀黍 - 千年食谱颂.mp3"));
    CHECK(without_id3v2(stored) == audio);

    // Plaintext only to the registered API endpoints, never with a secret; audio from
    // the primary over HTTPS.
    std::size_t audio_gets = 0;
    for (const auto& request : upstream->requests()) {
        INFO(request.url);
        CHECK(request.secret_headers.empty());
        if (request.url.rfind("http://", 0) == 0) {
            CHECK((request.url.rfind("http://mobileapi.5sing.kugou.com/song/", 0) == 0 ||
                   request.url.rfind("http://search.5sing.kugou.com/home/json?", 0) == 0));
        } else if (request.method == ports::HttpMethod::get) {
            CHECK(request.url.rfind(primary_host, 0) == 0);
            ++audio_gets;
        }
    }
    CHECK(audio_gets == 1);

    // The primary down: the same file comes from the backup.
    upstream->fail_primary(true);
    upstream->clear();
    const auto forced = api.Post("/api/v1/downloads", {{"Idempotency-Key", "fivesing-gate-0002"}},
                                 R"({"track":{"source":"fivesing","id":"fc-16394286"},"duplicate_policy":"force"})",
                                 "application/json");
    REQUIRE(forced->status == 202);
    const auto second = wait_status(api, body(forced)["data"]["task_id"].get<std::string>(), "completed");
    REQUIRE(second["status"] == "completed");
    const auto second_file = body(api.Get("/api/v1/files/" + second["result"]["file_id"].get<std::string>()))["data"];
    CHECK(second_file["size_bytes"] == file["size_bytes"]);
    std::vector<std::string> fetched;
    for (const auto& request : upstream->requests()) {
        if (request.url.rfind("https://", 0) == 0 && request.method == ports::HttpMethod::get) {
            fetched.push_back(request.url);
        }
    }
    REQUIRE(fetched.size() == 1);
    CHECK(fetched[0].rfind(backup_host, 0) == 0);
    CHECK(fetched[0].ends_with("/" + lq_file)); // the very file the primary names
}

namespace {

std::string create_principal(Deployment& d, const server::CommandHooks& hooks, const std::string& name) {
    const auto created = d.run({"principal", "create", "--name", name}, hooks);
    REQUIRE(created.code == server::exit_ok);
    return created.out.substr(0, created.out.find('\t'));
}

std::string issue(Deployment& d, const server::CommandHooks& hooks, const std::string& principal,
                  const std::string& file) {
    const auto output = d.dir / "out" / file;
    REQUIRE(d.run({"token", "issue", "--principal", principal, "--scopes",
                   "catalog:read,downloads:read,downloads:write,files:read", "--output",
                   infrastructure::filesystem::path_to_utf8(output)},
                  hooks)
                .code == server::exit_ok);
    return d.read(output);
}

std::set<std::string> listed_sources(httplib::Client& api, const std::string& query) {
    std::set<std::string> sources;
    const auto listed = body(api.Get("/api/v1/providers" + query));
    for (const auto& item : listed["data"]["items"]) {
        sources.insert(item["source"].get<std::string>());
    }
    return sources;
}

// The cache hit of each source in a search's per-source results.
std::map<std::string, bool> cache_hits(httplib::Client& api, const std::string& path) {
    const auto response = api.Get(path);
    REQUIRE(response->status == 200);
    std::map<std::string, bool> hits;
    const auto searched = body(response);
    for (const auto& result : searched["meta"]["source_results"]) {
        CHECK(result["status"] == "ok");
        hits[result["source"].get<std::string>()] = !result["cache"].is_null() && result["cache"]["hit"] == true;
    }
    return hits;
}

std::string start_download(httplib::Client& api, const std::string& key, const std::string& track_json) {
    const auto created = api.Post("/api/v1/downloads", {{"Idempotency-Key", key}}, R"({"track":)" + track_json + "}",
                                  "application/json");
    REQUIRE(created->status == 202);
    return body(created)["data"]["task_id"].get<std::string>();
}

// The statuses one SSE stream reported for each task, in order, until every listed task
// ended; only newer versions count (the client merge rule).
std::map<std::string, std::vector<std::string>> follow(SseClient& stream, const std::set<std::string>& tasks) {
    TaskBoard board;
    std::map<std::string, std::vector<std::string>> seen;
    std::set<std::string> done;
    for (auto frame = stream.next_event(20s); frame; frame = stream.next_event(20s)) {
        if (frame->event != "task.updated") {
            continue;
        }
        const auto update = json::parse(frame->data);
        const auto id = update["task_id"].get<std::string>();
        if (!board.apply(update)) {
            continue;
        }
        auto& statuses = seen[id];
        const auto status = update["status"].get<std::string>();
        if (statuses.empty() || statuses.back() != status) {
            statuses.push_back(status);
        }
        if (status == "completed" || status == "failed") {
            done.insert(id);
        }
        if (done == tasks) {
            break;
        }
    }
    return seen;
}

} // namespace

TEST_CASE("T41: two real platforms, two principals, one download path", "[gate][t41]") {
    Deployment d{"t41-gate"};
    server::CommandHooks hooks;
    hooks.host.bundles = both();
    REQUIRE(d.run({"init"}, hooks).code == server::exit_ok);
    const auto token_a = d.read(d.bootstrap());
    const auto principal_b = create_principal(d, hooks, "第二个客户端");
    d.config["destinations"].push_back({{"id", "dst_b"},
                                        {"name", "第二目录"},
                                        {"root", infrastructure::filesystem::path_to_utf8(d.dir / "second")},
                                        {"principals", {principal_b}}});
    d.config["event_stream"] = {{"heartbeat_seconds", 1}, {"progress_interval_ms", 100}};
    d.write_config();
    const auto token_b = issue(d, hooks, principal_b, "b.txt");

    const auto five_audio = mp3(800);
    const auto kuwo_audio = mp3(6738); // the 176 s Kuwo declares for 78932517
    auto upstream = std::make_shared<Upstream>(five_audio, kuwo_audio);
    RunningService service{d, std::make_shared<infrastructure::observability::MemorySink>(), both(), false, upstream};
    REQUIRE(service.report.events_base_url);
    auto a = service.client(token_a);
    auto b = service.client(token_b);
    const auto& events_url = *service.report.events_base_url;
    const auto events_port = std::stoi(events_url.substr(events_url.rfind(':') + 1));
    SseClient stream_a{events_port, token_a};
    SseClient stream_b{events_port, token_b};
    const auto ready_a = stream_a.next_event();
    const auto ready_b = stream_b.next_event();
    REQUIRE(ready_a);
    REQUIRE(ready_b);
    CHECK(ready_a->event == "stream.ready");
    CHECK(ready_b->event == "stream.ready");

    // Registration: both packages from the build list, filtered by what each declares.
    CHECK(listed_sources(a, "") == std::set<std::string>{"fivesing", "kuwo"});
    CHECK(listed_sources(a, "?capability=resolve_audio") == std::set<std::string>{"fivesing", "kuwo"});
    CHECK(listed_sources(a, "?capability=lyrics") == std::set<std::string>{"fivesing"});
    CHECK(listed_sources(a, "?capability=search_playlists") == std::set<std::string>{"fivesing", "kuwo"});
    CHECK(listed_sources(a, "?capability=search_albums") == std::set<std::string>{"kuwo"});
    CHECK(listed_sources(a, "?capability=search_artists").empty());
    const auto providers = body(a.Get("/api/v1/providers"));
    for (const auto& item : providers["data"]["items"]) {
        CHECK(item["implementation"]["execution_mode"] == "native");
        CHECK(item["auth_methods"].empty());
        const auto expected = item["source"] == "kuwo" ? json::array({"standard"}) : json::array({"standard", "high"});
        CHECK(item["quality_hints"] == expected);
    }

    // Capability differences through the same routes, decided by the declarations.
    CHECK(code_of(a.Get("/api/v1/lyrics?source=kuwo&id=78932517")) == "CAPABILITY_UNSUPPORTED");
    CHECK(a.Get("/api/v1/lyrics?source=fivesing&id=" + work)->status == 200);
    const auto kuwo_high = a.Post("/api/v1/assets/resolve",
                                  R"({"track":{"source":"kuwo","id":"78932517"},"quality":"high"})", "application/json");
    CHECK(code_of(kuwo_high) == "QUALITY_UNAVAILABLE");
    const auto five_lower = a.Post("/api/v1/assets/resolve",
                                   R"({"track":{"source":"fivesing","id":"fc-16394286"},"quality":"high",)"
                                   R"("quality_policy":"allow_lower"})",
                                   "application/json");
    REQUIRE(five_lower->status == 200);
    CHECK(body(five_lower)["data"]["actual_quality"] == "standard");
    // Collections through the shared routes: Kuwo's playlist pages under a service cursor,
    // its live chart list; 5sing declares none of them.
    const auto first_page = a.Get("/api/v1/playlists/tracks?source=kuwo&id=2234580349&limit=2");
    REQUIRE(first_page->status == 200);
    const auto first_body = body(first_page)["data"];
    CHECK(first_body["items"].size() == 2);
    CHECK(first_body["page"]["total"] == 17);
    const auto cursor = first_body["page"]["next_cursor"].get<std::string>();
    const auto second_page = a.Get("/api/v1/playlists/tracks?source=kuwo&id=2234580349&limit=2&cursor=" + cursor);
    REQUIRE(second_page->status == 200);
    CHECK(body(second_page)["data"]["items"][0]["ref"]["source"] == "kuwo");
    // A cursor is its owner's: another principal gets CURSOR_EXPIRED.
    CHECK(code_of(b.Get("/api/v1/playlists/tracks?source=kuwo&id=2234580349&limit=2&cursor=" + cursor)) == "CURSOR_EXPIRED");
    const auto charts = a.Get("/api/v1/charts?source=kuwo");
    REQUIRE(charts->status == 200);
    CHECK(body(charts)["data"]["catalog_kind"] == "live");
    CHECK(body(charts)["data"]["items"].size() == 36);
    const auto chart = body(a.Get("/api/v1/charts/detail?source=kuwo&id=16"))["data"];
    CHECK(chart["name"] == "酷我热歌榜");
    CHECK(code_of(a.Get("/api/v1/charts?source=fivesing")) == "CAPABILITY_UNSUPPORTED");

    // Each package owns its id format; one source's id is not valid at the other.
    CHECK(a.Get("/api/v1/tracks/detail?source=kuwo&id=" + work)->status == 400);
    CHECK(a.Get("/api/v1/tracks/detail?source=fivesing&id=78932517")->status == 400);

    // Catalog results are kept per identity: B never gets A's cached answers.
    const std::string search = "/api/v1/search/tracks?q=%E5%8D%83%E5%B9%B4&sources=kuwo,fivesing&limit=3";
    CHECK(cache_hits(a, search) == std::map<std::string, bool>{{"fivesing", false}, {"kuwo", false}});
    CHECK(cache_hits(a, search) == std::map<std::string, bool>{{"fivesing", true}, {"kuwo", true}});
    const auto searches = upstream->api_requests("http://search.5sing.kugou.com/");
    CHECK(cache_hits(b, search) == std::map<std::string, bool>{{"fivesing", false}, {"kuwo", false}});
    CHECK(upstream->api_requests("http://search.5sing.kugou.com/") == searches + 1);

    // One runner for both sources and both principals, at the same time.
    const auto a_kuwo = start_download(a, "t41-gate-a-kuwo-0001", R"({"source":"kuwo","id":"78932517"})");
    const auto a_five = start_download(a, "t41-gate-a-five-0001", R"({"source":"fivesing","id":"fc-16394286"})");
    const auto b_five = start_download(b, "t41-gate-b-five-0001", R"({"source":"fivesing","id":"fc-16394286"})");
    const auto seen_a = follow(stream_a, {a_kuwo, a_five});
    const auto seen_b = follow(stream_b, {b_five});

    // Events reach their owner only, and every task runs the same state machine.
    std::set<std::string> ids_a;
    for (const auto& [id, statuses] : seen_a) {
        ids_a.insert(id);
    }
    CHECK(ids_a == std::set<std::string>{a_kuwo, a_five});
    REQUIRE(seen_b.size() == 1);
    CHECK(seen_b.begin()->first == b_five);
    REQUIRE(seen_a.count(a_kuwo) == 1);
    REQUIRE(seen_a.count(a_five) == 1);
    CHECK(seen_a.at(a_kuwo).back() == "completed");
    CHECK(seen_a.at(a_kuwo) == seen_a.at(a_five));
    CHECK(seen_b.at(b_five) == seen_a.at(a_five));

    const auto kuwo_task = wait_status(a, a_kuwo, "completed");
    const auto five_task = wait_status(a, a_five, "completed");
    const auto theirs = wait_status(b, b_five, "completed");
    REQUIRE(kuwo_task["status"] == "completed");
    REQUIRE(five_task["status"] == "completed");
    REQUIRE(theirs["status"] == "completed");
    const auto five_file_id = five_task["result"]["file_id"].get<std::string>();
    const auto kuwo_file = body(a.Get("/api/v1/files/" + kuwo_task["result"]["file_id"].get<std::string>()))["data"];
    const auto five_file = body(a.Get("/api/v1/files/" + five_file_id))["data"];
    const auto b_file_id = theirs["result"]["file_id"].get<std::string>();
    CHECK(kuwo_file["metadata_status"] == "complete");
    CHECK(five_file["metadata_status"] == "complete");
    CHECK(five_file["display_name"] == "琴酒蜀黍 - 千年食谱颂.mp3");

    // Each principal's files land in its own destination and stay invisible to the other.
    const auto five_name = infrastructure::filesystem::path_from_utf8("琴酒蜀黍 - 千年食谱颂.mp3");
    CHECK(without_id3v2(read_file(d.dir / "下载" / five_name)) == five_audio);
    CHECK(without_id3v2(read_file(d.dir / "second" / five_name)) == five_audio);
    CHECK(without_id3v2(read_file(d.dir / "下载" / infrastructure::filesystem::path_from_utf8(
                                                       kuwo_file["display_name"].get<std::string>()))) == kuwo_audio);
    CHECK(b.Get("/api/v1/downloads/" + a_five)->status == 404);
    CHECK(b.Get("/api/v1/files/" + five_file_id)->status == 404);
    CHECK(a.Get("/api/v1/files/" + b_file_id)->status == 404);
    CHECK(body(b.Get("/api/v1/downloads"))["data"]["items"].size() == 1);
    CHECK(body(a.Get("/api/v1/downloads"))["data"]["items"].size() == 2);

    // Reuse is per principal and per track: A's new keys reuse A's files and nothing is
    // fetched again; B downloaded its own copy.
    for (const auto& [key, track] : {std::pair{"t41-gate-a-kuwo-0002", R"({"source":"kuwo","id":"78932517"})"},
                                     std::pair{"t41-gate-a-five-0002", R"({"source":"fivesing","id":"fc-16394286"})"}}) {
        INFO(key);
        const auto reused = a.Post("/api/v1/downloads", {{"Idempotency-Key", key}},
                                   std::string{R"({"track":)"} + track + "}", "application/json");
        CHECK(reused->status == 200);
        CHECK(body(reused)["data"]["reused"] == true);
    }
    CHECK(upstream->audio_gets(kuwo_cdn) == 1);
    CHECK(upstream->audio_gets(primary_host) == 2); // A's and B's
}
