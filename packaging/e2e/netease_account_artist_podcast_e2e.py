#!/usr/bin/env python3
"""D 阶段在线验收客户端；Cookie 只从本机文件读取，不写入报告。

只读个人歌单、歌手选曲批量下载、歌词变体、节目下载及重启文件回读。
"""
import argparse
import hashlib
import json
from pathlib import Path
import urllib.parse
import uuid

from kuwo_e2e import OwnService, Http, check, wait_task, ticket_for, get_url
from metadata_e2e import frames, text, fetch_task
from collection_batch_client import collect, monitor, replay
from netease_quality_discovery_e2e import measured


def run(service, cookie_file, report):
    api = Http(service.api, service.token)
    def get(route, **params):
        return api.json("GET", "/api/v1/" + route + "?" + urllib.parse.urlencode({"source": "netease", **params}))[2]
    def error(route, code, **params):
        status, _, raw = api.call("GET", "/api/v1/" + route + "?" + urllib.parse.urlencode({"source": "netease", **params}))
        check(status >= 400 and json.loads(raw)["error"]["code"] == code, "expected " + code)
    def variant(value):
        nonlocal api
        config = json.loads(Path(service.config).read_text(encoding="utf-8"))
        config["downloads"] = {"output": {"lyrics_variant": value}}
        Path(service.config).write_text(json.dumps(config, ensure_ascii=False), encoding="utf-8")
        service.restart()
        api = Http(service.api, service.token)
    def lyric_text(data):
        return frames(data)["USLT"][0][4:].split(b"\0", 1)[1].decode("utf-8").rstrip("\0")
    def read_file(task):
        status, _, data = get_url(ticket_for(api, task["result"]["file_id"]))
        check(status == 200, "published file is readable")
        return data

    error("me/playlists", "CREDENTIALS_REQUIRED")
    credential = api.json("POST", "/api/v1/credentials", {"source": "netease",
        "kind": "cookie", "auth_profile": "web_cookie", "secret": {"cookie": cookie_file.read_text(encoding="utf-8").strip()}, "label": "D acceptance"}, expect=(201,))[2]["credential_id"]
    verified = api.json("POST", f"/api/v1/credentials/{credential}/verify", {})[2]
    check(verified["result"] == "valid", "provided account verifies")
    api.json("PUT", "/api/v1/credential-defaults/netease", {"credential_id": credential})
    first = get("me/playlists", limit=2)
    check(bool(first["items"]), "personal playlist exists")
    check(all(p["relationship"] in ("created", "subscribed") for p in first["items"]), "personal relationships are explicit")
    detail = get("playlists/detail", id=first["items"][0]["ref"]["id"])
    expanded = get("playlists/tracks", id=detail["ref"]["id"], limit=2)
    report["personal"] = {"first_page_count": len(first["items"]), "expanded_track_count": len(expanded["items"]),
        "relationships": sorted(set(p["relationship"] for p in first["items"]))}
    old_cursor = first["page"].get("next_cursor")
    if old_cursor:
        second = get("me/playlists", limit=2, cursor=old_cursor)
        check(first["items"][0]["ref"] != second["items"][0]["ref"], "personal pagination advances")
        report["personal"]["second_page_count"] = len(second["items"])
    api.call("DELETE", "/api/v1/credential-defaults/netease")
    error("me/playlists", "CREDENTIALS_REQUIRED")
    api.json("PUT", "/api/v1/credential-defaults/netease", {"credential_id": credential})
    if old_cursor:
        error("me/playlists", "CURSOR_EXPIRED", limit=2, cursor=old_cursor)
    report["personal"]["default_removal_isolated"] = True

    hot = get("artists/tracks", id="6452", limit=2)
    check(hot["page"]["listed_total"] <= 50 and hot["page"]["total"] is None, "hot songs are a bounded list")
    next_hot = get("artists/tracks", id="6452", limit=2, cursor=hot["page"]["next_cursor"])
    check(next_hot["items"][0]["ref"] != hot["items"][0]["ref"], "artist page advances")
    selected = {item["ref"]["id"] for item in hot["items"]}
    manifest = Path(service.work) / "artist-batch.json"
    collect(api, "https://music.163.com/#/artist?id=6452", manifest, ids=selected, max_items=2)
    batch = monitor(api, manifest, timeout=240)
    if batch["task_entries_by_status"] != {"completed": 2}:
        failures = []
        for record in batch["batches"]:
            for item in record["status"]["items"]:
                if item["status"] != "completed":
                    task = api.json("GET", "/api/v1/downloads/" + item["task_id"])[2]
                    failures.append({"status": task["status"], "error": task.get("error")})
        report["artist_failures"] = failures
    check(batch["task_entries_by_status"] == {"completed": 2}, "two selected hot songs download")
    report["artist"] = {"id": "6452", "listed_total": hot["page"]["listed_total"], "selected_ids": sorted(selected),
        "completed": 2}

    variant("verbatim")
    lyrics = get("lyrics", id="5257138")
    check(lyrics["lrc"] and len(lyrics["karaoke"]) > 10, "live YRC and original lyrics are available")
    task, audio = fetch_task(api, {"source": "netease", "id": "5257138"}, "d-yrc-" + uuid.uuid4().hex)
    embedded = lyric_text(audio)
    def stamp(ms):
        return f"{ms // 60000:02}:{ms // 1000 % 60:02}.{ms % 1000:03}"
    expected = "".join("[" + stamp(line["start_ms"]) + "]" +
        "".join("<" + stamp(word["start_ms"]) + ">" + word["text"] for word in line["words"]) +
        "<" + stamp(line["start_ms"] + line["duration_ms"]) + ">\n" for line in lyrics["karaoke"])
    check(not lyrics["translated_lrc"] and embedded == expected, "YRC file lyrics exactly match API word timing")
    translation = get("lyrics", id="400876427")
    check(translation["translated_lrc"] and translation["karaoke"], "live translation remains independent of YRC")
    report["lyrics"] = {"yrc_lines": len(lyrics["karaoke"]), "yrc_file_matches": True,
        "warnings": lyrics.get("warnings"), "translation_present": True}
    variant("romanized")
    roma = get("lyrics", id="1409311773")
    check(roma["romanized_lrc"] and roma["lrc"], "live romaji and original both available")
    _, roma_audio = fetch_task(api, {"source": "netease", "id": "1409311773"}, "d-romaji-" + uuid.uuid4().hex)
    expected = roma["romanized_lrc"] + ("\n\n[translation]\n" + roma["translated_lrc"] if roma["translated_lrc"] else "")
    check(lyric_text(roma_audio) == expected, "romanized file lyrics exactly match chosen variant")
    # No romaji for this song: retain the original and expose fallback.
    fallback, fallback_audio = fetch_task(api, {"source": "netease", "id": "5257138"}, "d-fallback-" + uuid.uuid4().hex)
    check(fallback["task_id"] != task["task_id"], "changed variant does not reuse another product")
    check(lyric_text(fallback_audio) == lyrics["lrc"], "missing romaji falls back to original")
    check("LYRICS_VARIANT_FALLBACK" in fallback["result"]["warnings"], "fallback is explicit")
    report["lyrics"].update(romanized_file_matches=True, fallback_explicit=True, variant_reuse_isolated=True)

    podcast = get("podcasts/detail", id="1218133742")
    episodes = get("podcasts/episodes", id="1218133742", limit=2)
    check(episodes["page"]["has_more"], "podcast paginates")
    second = get("podcasts/episodes", id="1218133742", limit=2, cursor=episodes["page"]["next_cursor"])
    check(second["items"][0]["ref"] != episodes["items"][0]["ref"], "episode page advances")
    episode = get("episodes/detail", id="3068973045")
    check(episode["audio_ref"]["id"] == "program:3068973045:2661623100", "distinct radio program and audio identity")
    check(episode["presenter"] and episode["podcast_title"] == podcast["title"], "episode presenter and radio are explicit")
    manifest = Path(service.work) / "episode-batch.json"
    collect(api, "https://music.163.com/#/program?id=3068973045", manifest)
    batch = monitor(api, manifest, timeout=240)
    check(batch["task_entries_by_status"] == {"completed": 1}, "selected episode downloads")
    tasks = api.json("GET", "/api/v1/download-batches/" + batch["batches"][0]["result"]["batch_id"])[2]
    episode_task = api.json("GET", "/api/v1/downloads/" + tasks["items"][0]["task_id"])[2]
    audio = read_file(episode_task)
    tags = frames(audio)
    check(text(tags["TIT2"][0]) == episode["title"], "episode title tag")
    check(text(tags["TALB"][0]) == podcast["title"], "podcast title tag")
    check("TPE1" not in tags or not text(tags["TPE1"][0]), "presenter is not mislabeled as a singer")
    check("APIC" in tags, "episode cover tag")
    facts = measured(audio)
    check(abs(facts["duration_ms"] - episode["duration_ms"]) < 2000, "episode full length independently measured")
    service.restart()
    api = Http(service.api, service.token)
    replay(api, manifest)
    restored = api.json("GET", "/api/v1/downloads/" + episode_task["task_id"])[2]
    check(read_file(restored) == audio, "episode restart and file readback preserve exact bytes")
    report["podcast"] = {"radio_id": "1218133742", "program_id": "3068973045", "measured": facts,
        "bytes": len(audio), "sha256": hashlib.sha256(audio).hexdigest(), "tags_match": True, "restart_file_readback": True}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--cookie-file", type=Path, required=True)
    parser.add_argument("--port", type=int, default=18750)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    service = OwnService(args.binary, args.port)
    report = {"status": "incomplete", "account_condition": "user-confirmed VIP1"}
    try:
        run(service, args.cookie_file, report)
        report["status"] = "passed"
    finally:
        service.close()
        args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))
