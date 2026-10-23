#!/usr/bin/env python3
"""千千匿名原生能力验收。独立临时服务；报告不包含临时音源地址或凭证。"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import sys
import time
import urllib.parse
import uuid

from kuwo_e2e import OwnService, Http, Sse, check, wait_task, ticket_for, get_url
from metadata_e2e import frames
from kuwo_artists_e2e import embedded_cover, id3_text


def run(service, report, event_port):
    api = Http(service.api, service.token)
    report["service_version"] = api.json("GET", "/api/v1/info")[2]["version"]

    def call(route, **params):
        query = {"source": "qianqian", **params}
        if route.startswith("search/"):
            query.pop("source")
            query["sources"] = "qianqian"
        for attempt in range(3):
            status, _, body = api.call("GET", "/api/v1/" + route + "?" + urllib.parse.urlencode(query))
            doc = json.loads(body)
            error = (doc.get("error") or {}).get("code")
            source_errors = [item["error"] for item in doc.get("meta", {}).get("source_results", []) if item.get("error")]
            if len(source_errors) == 1:
                error = source_errors[0]["code"]
            if status == 200 or error not in ("UPSTREAM_TIMEOUT", "UPSTREAM_RATE_LIMITED"):
                break
            report.setdefault("network_retries", []).append({"route": route, "code": error})
            if attempt < 2:
                time.sleep(1)
        if status != 200:
            report["last_error"] = {"route": route, "error": doc.get("error"), "source_errors": source_errors}
        check(status == 200, route + ": " + str(error or status))
        return doc

    def get(route, **params):
        return call(route, **params)["data"]

    report["search"] = {}
    selected = {}
    for kind in ("tracks", "albums"):
        cursor = None
        seen = set()
        counts = []
        for page_number in range(3):
            params = {"q": "许嵩", "limit": 3}
            if cursor:
                params["cursor"] = cursor
            doc = call("search/" + kind, **params)
            items = doc["data"]["items"]
            ids = [item["ref"]["id"] for item in items]
            check(ids and not seen.intersection(ids), kind + " page " + str(page_number + 1) + " advances")
            seen.update(ids)
            counts.append(len(ids))
            selected.setdefault(kind, ids[0])
            cursor = doc["meta"]["source_results"][0]["page"]["next_cursor"]
            if page_number < 2:
                check(cursor, kind + " offers next page")
        report["search"][kind] = counts
    check(get("search/tracks", q="love", limit=3)["items"], "English search returns results")
    check(not get("search/tracks", q="zzzzzzzxyzy9876543210", limit=3)["items"], "empty search is successful and empty")

    categories = get("categories")["items"]
    recommendations = get("recommendations/playlists", limit=3)
    check(categories and recommendations["items"], "official live discovery works anonymously")
    selected["playlists"] = recommendations["items"][0]["ref"]["id"]
    report["discovery"] = {"categories": len(categories), "homepage_playlists": len(recommendations["items"])}
    for route, params in (("recommendations/playlists", {}), ("categories/playlists", {"category_id": "2376"})):
        first = get(route, limit=3, **params)
        second = get(route, limit=3, cursor=first["page"]["next_cursor"], **params)
        check(second["items"] and first["items"][0]["ref"] != second["items"][0]["ref"], route + " pagination advances")

    report["collections"] = {}
    # 搜索首项可能是单曲专辑；使用搜索歌曲真实对应的多曲目专辑验收完整遍历。
    detail = get("tracks/detail", id=selected["tracks"])
    album_id = detail["album"]["ref"]["id"]
    for kind, entity in (("playlists", selected["playlists"]), ("albums", album_id)):
        collection = get(kind + "/detail", id=entity)
        check(collection["ref"]["id"] == entity, kind + " detail preserves identity")
        cursor, ids, pages = None, [], 0
        while True:
            params = {"id": entity, "limit": 5}
            if cursor:
                params["cursor"] = cursor
            result = get(kind + "/tracks", **params)
            if kind == "playlists":
                check(all(item.get("track_number") is None for item in result["items"]), "playlist positions do not become album track numbers")
            ids.extend(item["ref"]["id"] for item in result["items"])
            pages += 1
            check(pages <= 50, kind + " traversal stays bounded")
            cursor = result["page"]["next_cursor"]
            if not cursor:
                check(len(ids) == result["page"]["total"] == collection["track_count"], kind + " full traversal equals declared count")
                break
        report["collections"][kind] = {"id": entity, "pages": pages, "tracks": len(ids), "unique_tracks": len(set(ids))}

    for route, kind, entity in (("song", "track", selected["tracks"]), ("album", "album", album_id),
                                ("songlist", "playlist", selected["playlists"])):
        result = api.json("POST", "/api/v1/links/parse", {"text": "https://music.91q.com/" + route + "/" + entity})[2]
        check(result["matched"] and result["kind"] == kind and result["ref"]["id"] == entity, kind + " link resolves")
    for kind, entity in (("tracks", "T99999999999"), ("albums", "P99999999999"), ("playlists", "99999999999")):
        status, _, body = api.call("GET", "/api/v1/" + kind + "/detail?source=qianqian&id=" + entity)
        check(status != 200 and json.loads(body)["error"]["code"] == "NOT_FOUND", kind + " missing ID is explicit")
    status, _, body = api.call("GET", "/api/v1/search/playlists?sources=qianqian&q=test&limit=3")
    doc = json.loads(body)
    check(status != 200 and doc["meta"]["source_results"][0]["error"]["code"] == "CAPABILITY_UNSUPPORTED", "unsupported playlist search stays disabled")

    track = {"source": "qianqian", "id": selected["tracks"]}
    lyrics = get("lyrics", id=track["id"])
    check(lyrics["lrc"] and not lyrics["unavailable"], "LRC is available")
    report["lyrics_characters"] = len(lyrics["lrc"])
    plain = get("lyrics", id="T10064949966")
    check(plain["lrc"] and not plain["unavailable"] and plain["lrc"].startswith("我的祖国")
          and "[00:" not in plain["lrc"], "official plain TXT lyrics remain untimed text")
    report["plain_lyrics_characters"] = len(plain["lrc"])
    status, _, body = api.call("POST", "/api/v1/assets/resolve", {"track": {"source": "qianqian", "id": "T10064935558"}})
    check(status != 200 and json.loads(body)["error"]["code"] in ("CREDENTIALS_REQUIRED", "PREVIEW_REJECTED"), "VIP access cannot produce a full anonymous download")
    status, _, body = api.call("POST", "/api/v1/assets/resolve", {"track": track, "quality": "hires"})
    check(status != 200 and json.loads(body)["error"]["code"] == "QUALITY_UNAVAILABLE", "strict Hi-Res is explicit")
    lowered = api.json("POST", "/api/v1/assets/resolve", {"track": track, "quality": "hires", "quality_policy": "allow_lower"})[2]
    check(lowered["actual_quality"] == "lossless", "allow_lower reports the real available tier")

    report["downloads"] = {}
    file_ids = {}
    sse = Sse("http://127.0.0.1:" + str(event_port), service.token)
    try:
        for quality in ("standard", "high", "lossless"):
            request = {"track": track, "quality": quality, "metadata_policy": "required"}
            resolved = api.json("POST", "/api/v1/assets/resolve", {"track": track, "quality": quality})[2]
            check(resolved["actual_quality"] == quality and resolved["completeness"] == "full", quality + " full audio resolves")
            key = "qianqian-" + uuid.uuid4().hex
            created = api.json("POST", "/api/v1/downloads", request, {"Idempotency-Key": key}, expect=(200, 202))[2]
            task = wait_task(api, created["task_id"], {"completed", "failed", "canceled"}, timeout=240)
            if task["status"] != "completed":
                report["download_error"] = task.get("error")
            check(task["status"] == "completed", quality + " download completes: " + str(task.get("error")))
            check(task["actual_quality"] == quality, quality + " bytes pass real audio validation")
            file_id = task["result"]["file_id"]
            file_ids[quality] = file_id
            status, _, payload = get_url(ticket_for(api, file_id))
            check(status == 200 and len(payload) > 10000, quality + " published audio reads back")
            sha = hashlib.sha256(payload).hexdigest()
            parts = {}
            for part in ("cover", "lyrics"):
                url = ticket_for(api, file_id)
                status, _, data = get_url(url + ("&" if "?" in url else "?") + "part=" + part)
                check(status == 200 and data, quality + " " + part + " sidecar exists")
                parts[part] = hashlib.sha256(data).hexdigest()
                if part == "cover":
                    check(embedded_cover(payload, "standard" if quality == "high" else quality) == data, quality + " embedded cover matches exported cover")
            if quality != "lossless":
                tags = frames(payload)
                check(id3_text(tags["TIT2"][0]) == detail["title"] and "USLT" in tags, quality + " title and lyrics are embedded")
            replay = api.json("POST", "/api/v1/downloads", request, {"Idempotency-Key": key}, expect=(200, 202))[2]
            check(replay["task_id"] == created["task_id"], quality + " idempotency replays the same task")
            reused = api.json("POST", "/api/v1/downloads", request, {"Idempotency-Key": key + "-reuse"}, expect=(200, 202))[2]
            check(reused["task_id"] == created["task_id"], quality + " completed artifact is reused")
            report["downloads"][quality] = {"track": track["id"], "bytes": len(payload), "sha256": sha,
                "metadata_fields": task["result"]["metadata_fields"], "sidecars": parts, "reused": True}
        event = sse.wait(lambda name, data: data.get("task_id") == created["task_id"] and data.get("status") == "completed", timeout=10)
        check(event, "SSE reports the completed download")
        report["sse_completed"] = True
    finally:
        sse.close()
    service.restart()
    api = Http(service.api, service.token)
    for quality, file_id in file_ids.items():
        result = report["downloads"][quality]
        status, _, payload = get_url(ticket_for(api, file_id))
        check(status == 200 and hashlib.sha256(payload).hexdigest() == result["sha256"], quality + " survives restart")
        for part, expected in result["sidecars"].items():
            url = ticket_for(api, file_id)
            status, _, data = get_url(url + ("&" if "?" in url else "?") + "part=" + part)
            check(status == 200 and hashlib.sha256(data).hexdigest() == expected, quality + " " + part + " survives restart")
        result["restart_readback"] = True


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--port", type=int, default=19032)
    args = parser.parse_args()
    service = OwnService(args.binary, args.port)
    report = {"status": "failed", "authentication": "anonymous", "started_at": datetime.now(timezone.utc).isoformat()}
    try:
        path = Path(service.config)
        config = json.loads(path.read_text(encoding="utf-8"))
        config["downloads"] = {"output": {"embed_cover": True, "embed_lyrics": True,
            "export_cover": True, "export_lyrics": True, "sidecars_required": True}}
        path.write_text(json.dumps(config, ensure_ascii=False), encoding="utf-8")
        service.restart()
        run(service, report, args.port + 2)
        report["status"] = "passed"
    finally:
        service.close()
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
