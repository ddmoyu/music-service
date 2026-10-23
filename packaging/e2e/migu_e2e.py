#!/usr/bin/env python3
"""通过公共 HTTP API 验收咪咕；临时数据目录，无第三方解析或下载工具。"""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import time
import urllib.parse
import uuid

from kuwo_e2e import OwnService, Http, Sse, check, wait_task, ticket_for, get_url
from metadata_e2e import frames, text

sys.stdout.reconfigure(encoding="utf-8")


def run(service, report):
    api = Http(service.api, service.token)

    def part_url(file_id, part):
        url = ticket_for(api, file_id)
        return url + ("&" if "?" in url else "?") + "part=" + part

    def call(route, **params):
        query = {"sources" if route.startswith("search/") else "source": "migu", **params}
        for attempt in range(3):
            status, _, body = api.call("GET", "/api/v1/" + route + "?" + urllib.parse.urlencode(query))
            doc = json.loads(body)
            if status == 200 or doc.get("error", {}).get("code") not in ("UPSTREAM_TIMEOUT", "UPSTREAM_RATE_LIMITED"):
                break
            report.setdefault("network_retries", []).append({"route": route, "code": doc["error"]["code"]})
            if attempt < 2:
                time.sleep(1)
        if status != 200:
            report["failed_route"] = {"route": route, "error": doc.get("error"), "sources": doc.get("meta", {}).get("source_results")}
        check(status == 200, route + " succeeds")
        return doc

    def get(route, **params):
        return call(route, **params)["data"]

    report["search"] = {}
    selected = {}
    for kind, keyword in [("tracks", "小星星"), ("playlists", "轻音乐"), ("albums", "周杰伦"), ("artists", "周")]:
        first = call("search/" + kind, q=keyword, limit=7)
        entries = first["data"]["items"]
        check(bool(entries), kind + " has results")
        selected[kind] = entries[0]["ref"]["id"]
        cursor = first["meta"]["source_results"][0]["page"]["next_cursor"]
        counts = [len(entries)]
        for _ in range(3):
            if not cursor:
                break
            following = call("search/" + kind, q=keyword, limit=7, cursor=cursor)
            counts.append(len(following["data"]["items"]))
            entries.extend(following["data"]["items"])
            cursor = following["meta"]["source_results"][0]["page"]["next_cursor"]
        check(len(counts) >= 2, kind + " search paginates")
        if kind == "tracks":
            check(len(entries) > 20 and len({x["ref"]["id"] for x in entries}) == len(entries), "search crosses fixed 20-entry window without repeats")
        report["search"][kind] = {"pages": counts}
    check(bool(get("search/tracks", q="Twinkle", limit=2)["items"]), "English search works")
    # 咪咕可能把无匹配词回退到热门歌曲；不把这种上游行为误记为“空结果通过”。
    unmatched = get("search/tracks", q="musicservicemigunomatch987654321xyz", limit=2)["items"]
    check(len(unmatched) <= 2, "unmatched query still honors the public limit")
    report["unmatched_query"] = {"returned": len(unmatched), "behavior": "upstream_fallback" if unmatched else "empty"}

    recommendations = get("recommendations/playlists", limit=3)
    categories = get("categories")["items"]
    charts = get("charts")["items"]
    check(len(charts) >= 2 and categories and recommendations["items"], "live recommendations, categories and charts exist")
    report["discovery"] = {"charts": len(charts), "categories": len(categories), "recommendation_snapshot": recommendations["page"]["total"]}
    recommended_id = recommendations["items"][0]["ref"]["id"]
    for route, params in [("recommendations/playlists", {}), ("categories/playlists", {"category_id": "1000001762"}),
                          ("playlists/tracks", {"id": recommended_id})] + [("charts/tracks", {"id": c["ref"]["id"]}) for c in charts[:2]]:
        first = get(route, limit=3, **params)
        check(bool(first["items"]) and first["page"]["next_cursor"], route + " has a second page")
        second = get(route, limit=3, cursor=first["page"]["next_cursor"], **params)
        check(bool(second["items"]) and first["items"][0]["ref"] != second["items"][0]["ref"], route + " advances")
    for route, entity in [("playlists/detail", recommended_id), ("albums/detail", selected["albums"]), ("charts/detail", charts[0]["ref"]["id"])]:
        check(get(route, id=entity)["ref"]["id"] == entity, route + " preserves entity identity")

    cursor, category_counts = None, []
    for _ in range(3):
        params = {"category_id": "1000001762", "limit": 17}
        if cursor:
            params["cursor"] = cursor
        page = get("categories/playlists", **params)
        category_counts.append(len(page["items"]))
        cursor = page["page"]["next_cursor"]
        check(bool(cursor), "category cursor continues across upstream windows")
    check(category_counts == [17, 13, 17], "category pagination crosses its fixed 30-entry window")
    report["category_pages"] = category_counts

    report["collections"] = []
    for route, entity in [("albums/tracks", selected["albums"]), ("albums/tracks", "1000074978"),
                          ("playlists/tracks", recommended_id), ("playlists/tracks", "233754996"),
                          ("charts/tracks", charts[1]["ref"]["id"])]:
        cursor, count, pages = None, 0, 0
        while True:
            params = {"id": entity, "limit": 17}
            if cursor:
                params["cursor"] = cursor
            page = get(route, **params)
            count += len(page["items"])
            pages += 1
            check(pages <= 40, "sample collection traversal remains bounded")
            cursor = page["page"]["next_cursor"]
            if not cursor:
                check(count == page["page"]["total"], "collection traversal matches declared count")
                break
        report["collections"].append({"route": route, "id": entity, "pages": pages, "tracks": count})

    track = {"source": "migu", "id": "600902000008861771"}
    candidates = get("search/tracks", q="巴赫旧约", limit=20)["items"]
    check(any(item["ref"] == track for item in candidates), "download sample is reachable through search")
    detail = get("tracks/detail", id=track["id"])
    lyrics = get("lyrics", id=track["id"])
    check(detail["artwork"] and not lyrics["unavailable"], "track has cover and lyrics")
    for link, expected in [("https://music.migu.cn/v3/music/song/" + track["id"], track["id"]),
                           ("https://music.migu.cn/v5/playlist?playlistId=" + recommended_id, recommended_id),
                           ("https://music.migu.cn/v5/albumDetail?albumId=" + selected["albums"].removeprefix("d:") + "&playlistType=" + ("5" if selected["albums"].startswith("d:") else "2003"), selected["albums"])]:
        parsed = api.json("POST", "/api/v1/links/parse", {"text": link})[2]
        check(parsed["matched"] and parsed["ref"]["id"] == expected, "entity link resolves")

    resolved = api.json("POST", "/api/v1/assets/resolve", {"track": track})[2]
    check(resolved["actual_quality"] == "standard" and resolved["completeness"] == "full", "anonymous full standard audio resolves")
    report["quality"] = {}
    for quality in ("high", "lossless", "hires"):
        status, _, body = api.call("POST", "/api/v1/assets/resolve", {"track": track, "quality": quality})
        doc = json.loads(body)
        if status == 200:
            check(doc["data"]["actual_quality"] == quality, "strict quality never silently downgrades")
            report["quality"][quality] = "resolved_not_download_verified"
        else:
            check(doc["error"]["code"] in ("CREDENTIALS_REQUIRED", "QUALITY_UNAVAILABLE"), "quality restriction is explicit")
            report["quality"][quality] = doc["error"]["code"]
    lower = api.json("POST", "/api/v1/assets/resolve", {"track": track, "quality": "lossless", "quality_policy": "allow_lower"})[2]
    report["quality"]["allow_lower"] = lower["actual_quality"]
    preview = {"source": "migu", "id": "600902000006889366"}
    status, _, body = api.call("POST", "/api/v1/assets/resolve", {"track": preview})
    check(status != 200 and json.loads(body)["error"]["code"] == "PREVIEW_REJECTED", "preview is rejected by default")
    report["preview_rejected"] = True
    allowed = api.json("POST", "/api/v1/assets/resolve", {"track": preview, "allow_preview": True})[2]
    check(allowed["completeness"] == "preview", "explicit preview keeps its real completeness")
    check(any(w["code"] == "PREVIEW_RESOURCE" for w in allowed["warnings"]), "preview warning is retained")
    access = api.json("POST", "/api/v1/media-access", {
        "target": {"kind": "asset", "id": allowed["asset_id"]}, "mode": "proxy"})[2]
    status, headers, piece = get_url(access["url"], {"Range": "bytes=0-1023"})
    check(status == 206 and len(piece) == 1024, "preview media proxy returns the requested audio range")
    check(piece.startswith(b"ID3") or (piece[0] == 0xff and piece[1] & 0xe0 == 0xe0), "preview starts with MP3 bytes")
    report["preview"] = {"completeness": allowed["completeness"], "warning": "PREVIEW_RESOURCE",
                         "range_status": status, "range_bytes": len(piece), "content_type": headers.get("Content-Type")}

    info = api.json("GET", "/api/v1/info")[2]
    report["service_version"] = info.get("version")
    report["track"] = {**track, "title": detail["title"]}
    stream = Sse(info["events_base_url"], service.token)
    try:
        check(stream.wait(lambda e, d: e == "stream.ready", 10) is not None, "SSE starts")
        created = api.json("POST", "/api/v1/downloads", {"track": track}, {"Idempotency-Key": "migu-" + uuid.uuid4().hex}, expect=(200, 202))[2]
        event = stream.wait(lambda e, d: e == "task.updated" and d.get("task_id") == created["task_id"] and d.get("status") in ("completed", "failed"), 180)
        check(event is not None, "SSE reports task end")
        task = wait_task(api, created["task_id"], {"completed", "failed", "canceled"})
        report["download_status"] = {"status": task["status"], "error": task.get("error"), "warnings": task.get("warnings")}
        check(task["status"] == "completed", "Migu download completes")
        file_id = task["result"]["file_id"]
        status, _, payload = get_url(ticket_for(api, file_id))
        check(status == 200 and len(payload) > 100000, "published audio reads back")
        sha = hashlib.sha256(payload).hexdigest()
        tags = frames(payload)
        check(text(tags["TIT2"][0]) == detail["title"], "embedded title matches metadata")
        check("APIC" in tags and "USLT" in tags and b"image/webp" in tags["APIC"][0], "original WebP cover and lyrics are embedded")
        parts = {}
        for part in ("cover", "lyrics"):
            status, headers, body = get_url(part_url(file_id, part))
            check(status == 200 and body, part + " sidecar is available")
            if part == "cover":
                check(headers.get("Content-Type") == "image/webp" and body[:4] == b"RIFF", "WebP sidecar keeps its real type")
                check(tags["APIC"][0].endswith(body), "embedded and exported WebP bytes are identical")
            parts[part] = hashlib.sha256(body).hexdigest()
        status, headers, piece = get_url(ticket_for(api, file_id), {"Range": "bytes=0-15"})
        check(status == 206 and piece == payload[:16], "media range matches published bytes")
        reused = api.json("POST", "/api/v1/downloads", {"track": track}, {"Idempotency-Key": "migu-reuse-" + uuid.uuid4().hex}, expect=(200, 202))[2]
        check(reused["task_id"] == created["task_id"], "download reuse works")
    finally:
        stream.close()
    service.restart()
    api = Http(service.api, service.token)
    status, _, payload = get_url(ticket_for(api, file_id))
    check(status == 200 and hashlib.sha256(payload).hexdigest() == sha, "audio survives restart")
    for part, expected in parts.items():
        status, _, body = get_url(part_url(file_id, part))
        check(status == 200 and hashlib.sha256(body).hexdigest() == expected, part + " survives restart")
    report["download"] = {"quality": task["actual_quality"], "bytes": len(payload), "sha256": sha, "sidecars": parts,
                          "reused": True, "restart_readback": True, "metadata_fields": task["result"]["metadata_fields"]}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--port", type=int, default=19242)
    args = parser.parse_args()
    report = {"status": "failed", "source": "migu", "authentication": "anonymous"}
    service = None
    try:
        service = OwnService(args.binary, args.port)
        path = Path(service.config)
        config = json.loads(path.read_text(encoding="utf-8"))
        config["downloads"] = {"output": {"export_lyrics": True, "export_cover": True, "sidecars_required": True}}
        config.setdefault("features", {})["media_proxy"] = True
        path.write_text(json.dumps(config, ensure_ascii=False), encoding="utf-8")
        service.restart()
        run(service, report)
        report["status"] = "passed"
    except Exception as exc:
        report["failure"] = {"type": type(exc).__name__, "message": str(exc)[:500]}
        raise
    finally:
        if service:
            service.close()
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
