#!/usr/bin/env python3
"""酷狗歌手目录、整曲下载与审查修复的独立在线验收，无账号凭证。"""
import argparse
import hashlib
import json
from pathlib import Path
import time
import urllib.parse
import uuid

from kuwo_e2e import OwnService, Http, check, wait_task, ticket_for, get_url
from metadata_e2e import frames, text


def run(service, report):
    api = Http(service.api, service.token)

    def call(route, **params):
        query = {"sources" if route.startswith("search/") else "source": "kugou", **params}
        for attempt in range(3):
            status, _, body = api.call("GET", "/api/v1/" + route + "?" + urllib.parse.urlencode(query))
            doc = json.loads(body)
            error = doc.get("error") or {}
            if status == 200 or not error.get("retryable") or error.get("code") not in (
                    "UPSTREAM_TIMEOUT", "UPSTREAM_RATE_LIMITED", "UPSTREAM_FAILED"):
                break
            report.setdefault("network_retries", []).append({"route": route, "code": error["code"]})
            if attempt < 2:
                time.sleep(1)
        check(status == 200, route + ": " + str((doc.get("error") or {}).get("code", status)))
        return doc

    cursor, seen = None, set()
    report["search"] = {"pages": 0, "items": 0}
    for _ in range(3):
        params = {"q": "周杰伦", "limit": 2}
        if cursor:
            params["cursor"] = cursor
        doc = call("search/artists", **params)
        items = doc["data"]["items"]
        ids = {item["ref"]["id"] for item in items}
        check(len(items) == 2 and len(ids) == len(items) and not seen.intersection(ids), "artist search advances real pages")
        seen.update(ids)
        page = doc["meta"]["source_results"][0]["page"]
        cursor = page["next_cursor"]
        check(cursor, "artist search supplies continuation")
        report["search"]["pages"] += 1
        report["search"]["items"] += len(items)
        report["search"]["total"] = page["total"]
    check("3520" in seen, "artist search identifies Zhou Jielun")
    check(call("search/artists", q="Taylor Swift", limit=2)["data"]["items"], "English artist search works")
    check(not call("search/artists", q="zzxqvnotanartist74928163", limit=2)["data"]["items"], "unknown artist keyword returns an empty page")

    report["details"] = {}
    for artist_id, name in (("3060", "薛之谦"), ("3520", "周杰伦")):
        artist = call("artists/detail", id=artist_id)["data"]
        check(artist["ref"]["id"] == artist_id and artist["name"] == name, "artist detail identity matches")
        check(artist["description"] and artist["artwork"].startswith("https://singerimg.kugou.com/"), "biography and actual artist avatar are available")
        check(artist["birth_date"] is None and artist["country"] is None, "unstructured biography does not invent structured fields")
        report["details"][artist_id] = {"name": name, "tracks": artist["track_count"], "albums": artist["album_count"],
                                      "biography_bytes": len(artist["description"].encode("utf-8"))}
    status, _, body = api.call("GET", "/api/v1/artists/detail?source=kugou&id=999999999")
    check(status == 404 and json.loads(body)["error"]["code"] == "NOT_FOUND", "unknown artist returns not found")
    for link in ("https://m.kugou.com/singer/info/3060", "https://www.kugou.com/yy/singer/home/3060.html"):
        parsed = api.json("POST", "/api/v1/links/parse", {"text": link})[2]
        check(parsed["matched"] and parsed["kind"] == "artist" and parsed["ref"]["id"] == "3060", "artist link preserves identity")

    report["traversal"] = {}
    for kind, limit in (("tracks", 50), ("albums", 10)):
        cursor, total, count, unavailable, pages, unique = None, None, 0, 0, 0, set()
        while True:
            params = {"id": "3060", "limit": limit}
            if cursor:
                params["cursor"] = cursor
            data = call("artists/" + kind, **params)["data"]
            if total is None:
                total = data["page"]["total"]
            check(data["page"]["total"] == total, "artist collection total remains stable")
            count += len(data["items"])
            unique.update(item["ref"]["id"] for item in data["items"])
            unavailable += data["page"].get("unavailable_count") or 0
            pages += 1
            check(pages <= 80, "artist traversal remains bounded")
            cursor = data["page"]["next_cursor"]
            if pages == 1 and cursor:
                wrong = {"source": "kugou", "id": "3520", "limit": limit, "cursor": cursor}
                status, _, body = api.call("GET", "/api/v1/artists/" + kind + "?" + urllib.parse.urlencode(wrong))
                check(status == 400 and json.loads(body)["error"]["code"] == "CURSOR_MISMATCH", "artist cursor is bound to its parent")
            if not cursor:
                check(data["page"]["has_more"] is False and count + unavailable == total, "complete artist collection matches declared total")
                break
        report["traversal"][kind] = {"id": "3060", "pages": pages, "items": count, "unique_ids": len(unique),
                                      "unavailable": unavailable, "total": total}

    # 从歌手目录取得标准 hash，再走原有详情、解析、下载、标签和文件出口。
    items = call("artists/tracks", id="283640", limit=10)["data"]["items"]
    song = next(item for item in items if item["ref"]["id"] == "89a84d23c85b70e0d7a23f5c427ba075")
    check(song["artists"][0]["ref"]["id"] == "283640", "exactly matching artist name keeps the parent reference")
    track = song["ref"]
    detail = call("tracks/detail", id=track["id"])["data"]
    resolved = api.json("POST", "/api/v1/assets/resolve", {"track": track})[2]
    check(resolved["actual_quality"] == "standard" and resolved["completeness"] == "full", "artist track resolves full standard audio")
    created = api.json("POST", "/api/v1/downloads", {"track": track},
                       {"Idempotency-Key": "kg-artist-" + uuid.uuid4().hex}, expect=(200, 202))[2]
    task = wait_task(api, created["task_id"], {"completed", "failed", "canceled"}, timeout=180)
    check(task["status"] == "completed", "artist track download completed: " + str(task.get("error")))
    file_id = task["result"]["file_id"]
    status, _, audio = get_url(ticket_for(api, file_id))
    check(status == 200 and len(audio) > 10000 and task["actual_quality"] == "standard", "published audio reads with verified quality")
    tags = frames(audio)
    check(text(tags["TIT2"][0]) == detail["title"] and "APIC" in tags and "USLT" in tags, "track title cover and lyrics are embedded")
    sha = hashlib.sha256(audio).hexdigest()
    parts = {}
    for part in ("lyrics", "cover"):
        url = ticket_for(api, file_id)
        status, _, payload = get_url(url + ("&" if "?" in url else "?") + "part=" + part)
        check(status == 200 and payload, "artist download exports " + part)
        parts[part] = hashlib.sha256(payload).hexdigest()
    reused = api.json("POST", "/api/v1/downloads", {"track": track},
                      {"Idempotency-Key": "kg-artist-reuse-" + uuid.uuid4().hex}, expect=(200, 202))[2]
    check(reused["task_id"] == created["task_id"], "artist download result is reused")
    service.restart()
    api = Http(service.api, service.token)
    status, _, restored = get_url(ticket_for(api, file_id))
    check(status == 200 and hashlib.sha256(restored).hexdigest() == sha, "artist download survives restart")
    for part, expected in parts.items():
        url = ticket_for(api, file_id)
        status, _, payload = get_url(url + ("&" if "?" in url else "?") + "part=" + part)
        check(status == 200 and hashlib.sha256(payload).hexdigest() == expected, "artist sidecar survives restart")
    report["download"] = {"track": track["id"], "artist": "283640", "bytes": len(audio), "sha256": sha,
                          "quality": "standard", "reused": True, "restart_readback": True, "sidecars": parts}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--port", type=int, default=18923)
    args = parser.parse_args()
    service = OwnService(args.binary, args.port)
    report = {"status": "failed", "authentication": "anonymous"}
    try:
        config = Path(service.config)
        settings = json.loads(config.read_text(encoding="utf-8"))
        settings["downloads"] = {"output": {"export_lyrics": True, "export_cover": True, "sidecars_required": True}}
        config.write_text(json.dumps(settings, ensure_ascii=False), encoding="utf-8")
        service.restart()
        run(service, report)
        report["status"] = "passed"
    finally:
        service.close()
        args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
