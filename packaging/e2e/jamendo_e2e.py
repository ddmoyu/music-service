#!/usr/bin/env python3
"""Jamendo 匿名原生接口验收；Python 仅为测试客户端，生产不依赖它。"""
import argparse
import hashlib
import json
from pathlib import Path
import time
import urllib.parse
import uuid

from kuwo_e2e import OwnService, Http, check, wait_task, ticket_for, get_url
from metadata_e2e import frames


def text(frame):
    # 平台原文件已含 ID3。TagLib 可保留已有帧的合法编码，不能强制假定 UTF-8。
    encodings = {0: "latin-1", 1: "utf-16", 2: "utf-16-be", 3: "utf-8"}
    check(bool(frame) and frame[0] in encodings, "ID3 text has a valid encoding")
    return frame[1:].decode(encodings[frame[0]]).rstrip("\0")


def run(service, report):
    api = Http(service.api, service.token)

    def call(route, **params):
        query = {"sources" if route.startswith("search/") else "source": "jamendo", **params}
        path = "/api/v1/" + route + "?" + urllib.parse.urlencode(query)
        for attempt in range(3):
            status, _, raw = api.call("GET", path)
            response = json.loads(raw)
            codes = [(response.get("error") or {}).get("code")]
            codes += [s.get("error", {}).get("code") for s in response.get("meta", {}).get("source_results", []) if s.get("error")]
            if not any(code in ("UPSTREAM_TIMEOUT", "UPSTREAM_RATE_LIMITED", "SERVICE_BUSY") for code in codes):
                break
            report.setdefault("network_retries", []).append({"route": route, "codes": [code for code in codes if code]})
            if attempt < 2:
                time.sleep(1)
        if status != 200:
            report["request_error"] = {"route": route, "status": status, "error": response.get("error")}
        check(status == 200, route + " status: " + str(status) + " " + str(response.get("error")))
        return response

    def get(route, **params):
        return call(route, **params)["data"]

    provider = next(p for p in api.json("GET", "/api/v1/providers")[2]["items"] if p["source"] == "jamendo")
    report["provider"] = provider
    report["search"] = {}
    tracks = []
    for kind in ("tracks", "albums", "artists"):
        keyword = "rock" if kind == "artists" else "love"
        cursor, seen, counts = None, set(), []
        for _ in range(3):
            params = {"q": keyword, "limit": 2}
            if cursor:
                params["cursor"] = cursor
            response = call("search/" + kind, **params)
            rows = response["data"]["items"]
            check(rows, kind + " search has results")
            ids = [row["ref"]["id"] for row in rows]
            check(not seen.intersection(ids), kind + " page advances")
            seen.update(ids)
            if kind == "tracks":
                tracks.extend(rows)
            page = response["meta"]["source_results"][0]["page"]
            check(page["total"] is None, kind + " unknown total stays null")
            counts.append(len(rows))
            cursor = page["next_cursor"]
            check(cursor, kind + " continuation exists")
        report["search"][kind] = {"query": keyword, "pages": counts}
    chinese = call("search/tracks", q="周杰伦", limit=2)
    report["search"]["chinese_count"] = len(chinese["data"]["items"])
    check(chinese["meta"]["source_results"][0]["status"] == "ok", "Chinese search is a valid response even when empty")

    # 精选目录本地筛选；listed_total 只表示精选目录内的匹配数，全站 total 未知。
    cursor, seen, counts = None, set(), []
    for _ in range(3):
        params = {"q": "best of", "limit": 2}
        if cursor:
            params["cursor"] = cursor
        response = call("search/playlists", **params)
        rows = response["data"]["items"]
        check(rows, "featured playlist search has real matches")
        ids = [row["ref"]["id"] for row in rows]
        check(not seen.intersection(ids), "featured search page advances")
        seen.update(ids)
        for row in rows:
            check("best of" in (row["name"] + " " + (row.get("description") or "")).lower(), "featured playlist matches keyword")
        page = response["meta"]["source_results"][0]["page"]
        check(page["total"] is None and page["listed_total"] >= len(seen), "scoped search count does not invent a global total")
        counts.append(len(rows))
        cursor = page["next_cursor"]
    featured_match = get("playlists/detail", id=rows[0]["ref"]["id"])
    check(featured_match["name"] == rows[0]["name"], "featured search opens same playlist detail")
    report["search"]["playlists"] = {"scope": "official_featured", "query": "best of", "pages": counts, "matches": page["listed_total"]}
    empty = call("search/playlists", q="jamendo-no-match-62c7be85", limit=2)
    check(not empty["data"]["items"] and empty["meta"]["source_results"][0]["status"] == "ok", "no match is a successful empty search")

    categories = get("categories")
    check(categories["catalog_kind"] == "live" and categories["items"], "native categories are live")
    check(all(row["lists"] == "track" for row in categories["items"]), "native categories advertise tracks")
    rock = next(row for row in categories["items"] if row["name"] == "rock")
    cursor, seen, counts = None, set(), []
    for _ in range(3):
        params = {"category_id": rock["id"], "limit": 2}
        if cursor:
            params["cursor"] = cursor
        result = get("categories/tracks", **params)
        ids = [row["ref"]["id"] for row in result["items"]]
        check(ids and not seen.intersection(ids), "native category page advances")
        seen.update(ids)
        counts.append(len(ids))
        check(result["page"]["total"] is None, "category total remains unknown")
        cursor = result["page"]["next_cursor"]
    category_track = result["items"][0]
    check(get("tracks/detail", id=category_track["ref"]["id"])["title"] == category_track["title"], "category track opens detail")
    report["categories"] = {"count": len(categories["items"]), "groups": sorted({row["group"] for row in categories["items"]}), "id": rock["id"], "pages": counts}

    charts = get("charts")
    check(charts["catalog_kind"] == "live" and charts["items"], "official chart directory is live")
    report["charts"] = {"count": len(charts["items"]), "verified": []}
    for chart_id in ("bestof", "trending", "genre:" + rock["id"]):
        chart = get("charts/detail", id=chart_id)
        check(chart["ref"]["id"] == chart_id and chart["track_count"] is None, "chart details preserve identity and unknown count")
        cursor, seen, counts = None, set(), []
        for _ in range(2):
            params = {"id": chart_id, "limit": 3}
            if cursor:
                params["cursor"] = cursor
            result = get("charts/tracks", **params)
            ids = [row["ref"]["id"] for row in result["items"]]
            check(ids and not seen.intersection(ids), "chart pages preserve distinct positions")
            seen.update(ids)
            counts.append(len(ids))
            check(result["page"]["total"] is None and result["page"]["listed_total"] <= 100, "chart is an explicitly bounded top 100")
            cursor = result["page"]["next_cursor"]
        report["charts"]["verified"].append({"id": chart_id, "pages": counts, "listed_total": result["page"]["listed_total"]})

    chosen = next(row for row in tracks if row["ref"]["id"] == "49292")
    track = chosen["ref"]
    detail = get("tracks/detail", id=track["id"])
    check(detail["title"] == chosen["title"] and detail["artists"], "search result opens complete detail")
    check(detail["track_number"] == 5 and detail["track_total"] == 10, "detail preserves actual album position")
    album_id = detail["album"]["ref"]["id"]
    artist_id = detail["artists"][0]["ref"]["id"]
    album = get("albums/detail", id=album_id)
    artist = get("artists/detail", id=artist_id)
    check(album["ref"]["id"] == album_id and artist["ref"]["id"] == artist_id, "album and artist identities match")
    artist_albums = get("artists/albums", id=artist_id, limit=2)
    check(any(row["ref"]["id"] == album_id for row in artist_albums["items"]), "artist album directory includes source album")
    recommendations = get("recommendations/playlists", limit=2)
    check(recommendations["items"] and recommendations["page"]["next_cursor"], "official recommendations paginate")
    second = get("recommendations/playlists", limit=2, cursor=recommendations["page"]["next_cursor"])
    check(second["items"][0]["ref"] != recommendations["items"][0]["ref"], "recommendation page advances")
    playlist_id = recommendations["items"][0]["ref"]["id"]
    playlist = get("playlists/detail", id=playlist_id)
    check(playlist["ref"]["id"] == playlist_id, "recommended playlist opens by the same id")

    def traverse(route, entity_id, limit):
        cursor, count, unavailable, pages = None, 0, 0, 0
        while True:
            params = {"id": entity_id, "limit": limit}
            if cursor:
                params["cursor"] = cursor
            result = get(route, **params)
            page = result["page"]
            count += len(result["items"])
            unavailable += page["unavailable_count"] or 0
            pages += 1
            check(pages <= 40, route + " bounded traversal")
            cursor = page["next_cursor"]
            if not cursor:
                check(count + unavailable == page["listed_total"] == page["total"], route + " accounts for every position")
                return {"id": entity_id, "pages": pages, "tracks": count, "unavailable": unavailable}

    report["album"] = traverse("albums/tracks", album_id, 3)
    report["playlist"] = traverse("playlists/tracks", playlist_id, 50)
    for kind, entity_id in (("track", track["id"]), ("album", album_id), ("artist", artist_id), ("playlist", playlist_id)):
        result = api.json("POST", "/api/v1/links/parse", {"text": "https://www.jamendo.com/" + kind + "/" + entity_id})[2]
        check(result["matched"] and result["ref"]["id"] == entity_id, kind + " link parses")
    lyrics = get("lyrics", id=track["id"])
    report["lyrics"] = "unavailable" if lyrics["unavailable"] else "present"

    for route in ("categories/playlists?source=jamendo&category_id=23", "artists/tracks?source=jamendo&id=" + artist_id):
        status, _, raw = api.call("GET", "/api/v1/" + route)
        check(status == 422 and b"CAPABILITY_UNSUPPORTED" in raw, "unimplemented capability reports unsupported")
    resolved = api.json("POST", "/api/v1/assets/resolve", {"track": track})[2]
    check(resolved["actual_quality"] == "standard" and resolved["completeness"] == "full", "standard full download resource resolves")
    report["quality"] = {}
    for tier in ("high", "lossless", "hires"):
        status, _, raw = api.call("POST", "/api/v1/assets/resolve", {"track": track, "quality": tier})
        check(status == 422 and json.loads(raw)["error"]["code"] == "QUALITY_UNAVAILABLE", "strict " + tier + " is rejected")
        lower = api.json("POST", "/api/v1/assets/resolve", {"track": track, "quality": tier, "quality_policy": "allow_lower"})[2]
        check(lower["actual_quality"] == "standard", "explicit lower-quality policy returns standard")
        report["quality"][tier] = {"strict": "QUALITY_UNAVAILABLE", "allow_lower": lower["actual_quality"]}

    request = {"track": track, "metadata_policy": "required"}
    key = "jamendo-" + uuid.uuid4().hex
    created = api.json("POST", "/api/v1/downloads", request, {"Idempotency-Key": key}, expect=(200, 202))[2]
    task = wait_task(api, created["task_id"], {"completed", "failed", "canceled"}, timeout=180)
    if task["status"] != "completed":
        report["download_error"] = task.get("error")
    check(task["status"] == "completed", "Jamendo native download completes")
    check(task["actual_quality"] == "standard" and task["result"]["processing"]["completeness"] == "full",
          "downloaded bytes verify standard quality and full duration")
    file_id = task["result"]["file_id"]
    status, _, payload = get_url(ticket_for(api, file_id))
    check(status == 200 and len(payload) > 10000, "published audio is readable")
    sha = hashlib.sha256(payload).hexdigest()
    tags = frames(payload)
    check(text(tags["TIT2"][0]) == detail["title"], "ID3 title matches detail")
    check(text(tags["TPE1"][0]) == detail["artists"][0]["name"], "ID3 artist matches detail")
    check(text(tags["TALB"][0]) == detail["album"]["title"] and "APIC" in tags, "album and cover are embedded")
    check(text(tags["TRCK"][0]) == "5/10", "actual album position is embedded")
    cover_url = ticket_for(api, file_id)
    cover_url += ("&" if "?" in cover_url else "?") + "part=cover"
    status, _, cover = get_url(cover_url)
    check(status == 200 and cover, "cover sidecar is readable")
    cover_sha = hashlib.sha256(cover).hexdigest()
    reused = api.json("POST", "/api/v1/downloads", request, {"Idempotency-Key": "jamendo-reuse-" + uuid.uuid4().hex}, expect=(200, 202))[2]
    check(reused["task_id"] == created["task_id"], "completed download is reused")
    service.restart()
    api = Http(service.api, service.token)
    restored_task = api.json("GET", "/api/v1/downloads/" + created["task_id"])[2]
    check(restored_task["status"] == "completed", "completed task survives restart")
    status, _, restored = get_url(ticket_for(api, file_id))
    check(status == 200 and hashlib.sha256(restored).hexdigest() == sha, "audio survives restart byte for byte")
    cover_url = ticket_for(api, file_id)
    cover_url += ("&" if "?" in cover_url else "?") + "part=cover"
    status, _, restored = get_url(cover_url)
    check(status == 200 and hashlib.sha256(restored).hexdigest() == cover_sha, "cover sidecar survives restart")
    replay = api.json("POST", "/api/v1/downloads", request, {"Idempotency-Key": key}, expect=(200, 202))[2]
    check(replay["task_id"] == created["task_id"], "idempotency replay survives restart")
    report["download"] = {"track": track["id"], "bytes": len(payload), "sha256": sha, "cover_sha256": cover_sha,
                          "task": task, "reused": True, "restart_readback": True}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--port", type=int, default=19032)
    args = parser.parse_args()
    service = None
    report = {"status": "failed"}
    try:
        service = OwnService(args.binary, args.port)
        config = Path(service.config)
        settings = json.loads(config.read_text(encoding="utf-8"))
        settings["downloads"] = {"output": {"export_cover": True}}
        config.write_text(json.dumps(settings, ensure_ascii=False), encoding="utf-8")
        service.restart()
        run(service, report)
        report["status"] = "passed"
    finally:
        if service:
            service.close()
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
