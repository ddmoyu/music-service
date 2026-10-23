#!/usr/bin/env python3
"""酷狗匿名目录与真实下载验收；临时独立服务，不读取或输出账号凭证。"""
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
            if status == 200 or doc.get("error", {}).get("code") not in ("UPSTREAM_TIMEOUT", "UPSTREAM_RATE_LIMITED"):
                break
            report.setdefault("network_retries", []).append({"route": route, "code": doc["error"]["code"]})
            if attempt < 2:
                time.sleep(1)
        check(status == 200, route + ": " + str((doc.get("error") or {}).get("code", status)))
        return doc

    def get(route, **params):
        return call(route, **params)["data"]

    report["search"] = {}
    selected = {}
    for kind, keyword in [("tracks", "两只老虎"), ("playlists", "轻音乐"), ("albums", "小星星")]:
        cursor, previous = None, None
        report["search"][kind] = []
        for index in range(3):
            params = {"q": keyword, "limit": 2}
            if cursor:
                params["cursor"] = cursor
            doc = call("search/" + kind, **params)
            items = doc["data"]["items"]
            check(items and items[0]["ref"] != previous, kind + " search advances")
            selected.setdefault(kind, items[0]["ref"]["id"])
            previous = items[0]["ref"]
            cursor = doc["meta"]["source_results"][0]["page"]["next_cursor"]
            report["search"][kind].append(len(items))
            if index < 2:
                check(cursor, kind + " search supplies continuation")
    check(get("search/tracks", q="Twinkle Twinkle Little Star", limit=2)["items"], "English search returns songs")
    empty = get("search/tracks", q="zzxqvnotasong74928163", limit=2)
    check(not empty["items"], "unmatched search succeeds with an empty page")

    charts = get("charts")["items"]
    categories = get("categories")["items"]
    check(len(charts) >= 2 and categories, "chart and category directories exist")
    recommendation = get("recommendations/playlists", limit=20)
    recommendation_id = recommendation["items"][0]["ref"]["id"]
    report["discovery"] = {"charts": len(charts), "categories": len(categories), "recommendations_total": recommendation["page"]["total"]}
    category = next(x["id"] for x in categories if x["name"] in ("风格", "流行", "华语", "轻音乐"))
    lists = [("recommendations/playlists", {}, 20),
             ("categories/playlists", {"category_id": category}, 2),
             ("playlists/tracks", {"id": recommendation_id}, 2),
             ("playlists/tracks", {"id": selected["playlists"]}, 2),
             ("albums/tracks", {"id": selected["albums"]}, 2)]
    lists += [("charts/tracks", {"id": chart["ref"]["id"]}, 2) for chart in charts[:2]]
    report["pages"] = []
    for route, params, limit in lists:
        first = get(route, limit=limit, **params)
        check(first["items"] and first["page"]["next_cursor"], route + " has a continuation")
        second = get(route, limit=limit, cursor=first["page"]["next_cursor"], **params)
        check(second["items"] and first["items"][0]["ref"] != second["items"][0]["ref"], route + " advances")
        report["pages"].append({"route": route, "first": len(first["items"]), "second": len(second["items"])})
    for route, entity in [("playlists/detail", recommendation_id), ("albums/detail", selected["albums"]), ("charts/detail", charts[0]["ref"]["id"])]:
        check(get(route, id=entity)["ref"]["id"] == entity, route + " identity matches")

    report["traversal"] = {}
    for kind, entity in [("playlists", recommendation_id), ("albums", selected["albums"])]:
        cursor, count, unavailable, pages = None, 0, 0, 0
        while True:
            params = {"id": entity, "limit": 50 if kind == "playlists" else 5}
            if cursor:
                params["cursor"] = cursor
            page = get(kind + "/tracks", **params)
            count += len(page["items"])
            unavailable += page["page"].get("unavailable_count") or 0
            pages += 1
            check(pages <= 40, "sample traversal is bounded")
            cursor = page["page"]["next_cursor"]
            if not cursor:
                check(count + unavailable == page["page"]["total"], kind + " traversal matches declared count")
                break
        report["traversal"][kind] = {"id": entity, "pages": pages, "tracks": count, "unavailable": unavailable}

    songs = get("search/tracks", q="两只老虎", limit=10)["items"]
    free = next(x for x in songs if any("贝瓦儿歌" in artist["name"] for artist in x["artists"]))
    track = free["ref"]
    detail = get("tracks/detail", id=track["id"])
    check(detail["ref"] == track and detail["duration_ms"] > 60000, "search track opens full metadata")
    for link, expected in [("https://www.kugou.com/song/#hash=" + track["id"].upper(), track["id"]),
        ("https://www.kugou.com/yy/special/single/" + recommendation_id + ".html", recommendation_id),
        ("https://www.kugou.com/album/" + selected["albums"] + ".html", selected["albums"])]:
        parsed = api.json("POST", "/api/v1/links/parse", {"text": link})[2]
        check(parsed["matched"] and parsed["ref"]["id"] == expected, "link resolves to canonical identity")
    lyrics = get("lyrics", id=track["id"])
    check(not lyrics["unavailable"] and lyrics["lrc"], "hash matched LRC exists")
    report["lyrics"] = {"available": True, "characters": len(lyrics["lrc"])}

    resolved = api.json("POST", "/api/v1/assets/resolve", {"track": track})[2]
    check(resolved["actual_quality"] == "standard" and resolved["completeness"] == "full", "full standard audio resolves")
    report["qualities"] = {}
    for quality in ("high", "lossless", "hires"):
        status, _, body = api.call("POST", "/api/v1/assets/resolve", {"track": track, "quality": quality})
        doc = json.loads(body)
        if status == 200:
            check(doc["data"]["actual_quality"] == quality, "strict quality never silently lowers")
            report["qualities"][quality] = "resolved_not_download_verified"
        else:
            code = doc["error"]["code"]
            check(code in ("QUALITY_UNAVAILABLE", "CREDENTIALS_REQUIRED"), "quality failure is explicit")
            report["qualities"][quality] = code
    lower = api.json("POST", "/api/v1/assets/resolve", {"track": track, "quality": "lossless", "quality_policy": "allow_lower"})[2]
    report["allow_lower"] = lower["actual_quality"]
    check(lower["actual_quality"] in ("standard", "high", "lossless"), "explicit fallback uses a supported tier")
    status, _, body = api.call("POST", "/api/v1/assets/resolve", {"track": {"source": "kugou", "id": selected["tracks"]}})
    paid = json.loads(body)
    check(status != 200 and paid["error"]["code"] == "CREDENTIALS_REQUIRED", "paid sample is not misreported as full playable audio")
    report["paid_sample"] = paid["error"]["code"]

    created = api.json("POST", "/api/v1/downloads", {"track": track}, {"Idempotency-Key": "kg-" + uuid.uuid4().hex}, expect=(200, 202))[2]
    task = wait_task(api, created["task_id"], {"completed", "failed", "canceled"}, timeout=180)
    if task["status"] != "completed":
        report["download_error"] = task.get("error")
    check(task["status"] == "completed", "Kugou download completed")
    file_id = task["result"]["file_id"]
    status, _, payload = get_url(ticket_for(api, file_id))
    check(status == 200 and len(payload) > 10000, "published audio reads back")
    sha = hashlib.sha256(payload).hexdigest()
    tags = frames(payload)
    check(text(tags["TIT2"][0]) == detail["title"], "embedded title matches track metadata")
    check("APIC" in tags and "USLT" in tags, "cover and lyrics are embedded")
    parts = {}
    for part in ("lyrics", "cover"):
        url = ticket_for(api, file_id)
        status, _, data = get_url(url + ("&" if "?" in url else "?") + "part=" + part)
        check(status == 200 and data, part + " sidecar reads back")
        parts[part] = hashlib.sha256(data).hexdigest()
    check(task["actual_quality"] == "standard", "download passes actual quality verification")
    replay = api.json("POST", "/api/v1/downloads", {"track": track}, {"Idempotency-Key": "kg-reuse-" + uuid.uuid4().hex}, expect=(200, 202))[2]
    check(replay["task_id"] == created["task_id"], "completed file is reused")
    service.restart()
    api = Http(service.api, service.token)
    status, _, restored = get_url(ticket_for(api, file_id))
    check(status == 200 and hashlib.sha256(restored).hexdigest() == sha, "published audio survives restart")
    for part, expected in parts.items():
        url = ticket_for(api, file_id)
        status, _, data = get_url(url + ("&" if "?" in url else "?") + "part=" + part)
        check(status == 200 and hashlib.sha256(data).hexdigest() == expected, part + " survives restart")
    report["download"] = {"track": track["id"], "quality": "standard", "bytes": len(payload), "sha256": sha,
        "reused": True, "restart_readback": True, "sidecars": parts, "metadata_fields": task["result"]["metadata_fields"]}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--port", type=int, default=18922)
    args = parser.parse_args()
    service = OwnService(args.binary, args.port)
    report = {"status": "failed"}
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
