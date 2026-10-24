#!/usr/bin/env python3
"""QQ 原生接口和下载验收；只从本机文件读取 Cookie，不输出凭证或账号信息。"""
import argparse
import hashlib
import json
from pathlib import Path
import urllib.parse
import uuid
import time

from kuwo_e2e import OwnService, Http, check, wait_task, ticket_for, get_url
from metadata_e2e import frames, text


def run(service, cookie_file, report):
    api = Http(service.api, service.token)

    def call(route, **params):
        query = {"source": "qq", **params}
        if route.startswith("search/"):
            query.pop("source")
            query["sources"] = "qq"
        for attempt in range(3):
            status, _, body = api.call("GET", "/api/v1/" + route + "?" + urllib.parse.urlencode(query))
            doc = json.loads(body)
            if status == 200 or doc.get("error", {}).get("code") not in ("UPSTREAM_TIMEOUT", "UPSTREAM_RATE_LIMITED"):
                break
            report.setdefault("network_retries", []).append({"route": route, "code": doc["error"]["code"]})
            if attempt < 2:
                time.sleep(1)
        check(status == 200, route + ": " + str(doc.get("error", {}).get("code") if doc.get("error") else status))
        return doc

    def get(route, **params):
        return call(route, **params)["data"]

    check(get("recommendations/playlists", limit=2)["items"], "recommendations work before credentials exist")
    status, _, body = api.call("GET", "/api/v1/search/tracks?sources=qq&q=test&limit=2")
    missing = json.loads(body)
    check(status != 200 and missing["meta"]["source_results"][0]["error"]["code"] == "CREDENTIALS_REQUIRED", "anonymous search reports login requirement")
    invalid = api.json("POST", "/api/v1/credentials", {
        "source": "qq", "kind": "cookie", "auth_profile": "web_cookie", "secret": {"cookie": "uin=12345; qqmusic_key=synthetic-invalid"}, "label": "invalid test"}, expect=(201,))[2]["credential_id"]
    check(api.wait_credential(invalid)["login_status"] == "invalid", "invalid Cookie is rejected")
    credential = api.json("POST", "/api/v1/credentials", {
        "source": "qq", "kind": "cookie", "auth_profile": "web_cookie", "secret": {"cookie": cookie_file.read_text(encoding="utf-8").strip()},
        "label": "QQ acceptance"}, expect=(201,))[2]["credential_id"]
    verified = api.wait_credential(credential)
    check(verified["login_status"] == "valid", "QQ cookie verifies")
    api.json("PUT", "/api/v1/credential-defaults/qq", {"credential_id": credential})
    report["credential"] = "valid"
    report["search"] = {}
    selected = {}
    for kind, keyword in [("tracks", "儿童歌曲 小星星"), ("playlists", "轻音乐"), ("albums", "周杰伦"), ("artists", "周杰伦")]:
        first = call("search/" + kind, q=keyword, limit=2)
        check(len(first["data"]["items"]) > 0, kind + " search returns results")
        selected[kind] = first["data"]["items"][0]["ref"]["id"]
        cursor = first["meta"]["source_results"][0]["page"]["next_cursor"]
        check(cursor, kind + " has a continuation")
        second = call("search/" + kind, q=keyword, limit=2, cursor=cursor)
        check(second["data"]["items"] and second["data"]["items"][0]["ref"] != first["data"]["items"][0]["ref"], kind + " search advances")
        report["search"][kind] = {"first": len(first["data"]["items"]), "second": len(second["data"]["items"])}
        cursor = second["meta"]["source_results"][0]["page"]["next_cursor"]
        check(cursor, kind + " has a third page")
        third = call("search/" + kind, q=keyword, limit=2, cursor=cursor)
        report["search"][kind]["third"] = len(third["data"]["items"])

    report["discovery"] = {}
    recommendations = get("recommendations/playlists", limit=2)
    check(recommendations["items"], "public recommendations exist")
    recommendation_id = recommendations["items"][0]["ref"]["id"]
    charts = get("charts")["items"]
    categories = get("categories")["items"]
    check(len(charts) >= 2 and categories, "chart and category directories exist")
    report["discovery"].update(charts=len(charts), categories=len(categories))
    lists = [("recommendations/playlists", {}),
             ("categories/playlists", {"category_id": next(x["id"] for x in categories if x["id"] != "0")}),
             ("playlists/tracks", {"id": recommendation_id}),
             ("playlists/tracks", {"id": selected["playlists"]}),
             ("albums/tracks", {"id": selected["albums"]}),
             ("artists/tracks", {"id": selected["artists"]}),
             ("artists/albums", {"id": selected["artists"]})]
    lists += [("charts/tracks", {"id": chart["ref"]["id"]}) for chart in charts[:2]]
    for index, (route, params) in enumerate(lists):
        first = get(route, limit=2, **params)
        check(first["items"], route + " returns entries")
        cursor = first["page"]["next_cursor"]
        check(cursor, route + " provides next cursor")
        second = get(route, limit=2, cursor=cursor, **params)
        check(second["items"] and second["items"][0]["ref"] != first["items"][0]["ref"], route + " page advances")
        report["discovery"][str(index)] = {"route": route, "first": len(first["items"]), "second": len(second["items"])}
    for route, entity in [("tracks/detail", selected["tracks"]), ("playlists/detail", recommendation_id),
                          ("albums/detail", selected["albums"]), ("charts/detail", charts[0]["ref"]["id"])]:
        check(get(route, id=entity)["ref"]["id"] == entity, route + " preserves identity")
    cursor, count, pages = None, 0, 0
    while True:
        params = {"id": selected["albums"], "limit": 5}
        if cursor:
            params["cursor"] = cursor
        page = get("albums/tracks", **params)
        count += len(page["items"])
        pages += 1
        check(pages <= 20, "sample album traversal stays bounded")
        cursor = page["page"]["next_cursor"]
        if not cursor:
            check(count == page["page"]["total"], "sample album traversal matches declared count")
            break
    report["album_traversal"] = {"pages": pages, "tracks": count}
    cursor, count, pages = None, 0, 0
    while True:
        params = {"id": recommendation_id, "limit": 50}
        if cursor:
            params["cursor"] = cursor
        page = get("playlists/tracks", **params)
        count += len(page["items"])
        pages += 1
        check(pages <= 40, "sample playlist traversal stays bounded")
        cursor = page["page"]["next_cursor"]
        if not cursor:
            check(count == page["page"]["total"], "sample playlist traversal matches declared count")
            break
    report["playlist_traversal"] = {"pages": pages, "tracks": count}
    check(call("search/tracks", q="Twinkle Twinkle Little Star", limit=2)["data"]["items"], "English search returns results")
    for kind, path in [("tracks", "songDetail"), ("playlists", "playlist"), ("albums", "albumDetail")]:
        link = "https://y.qq.com/n/ryqq/" + path + "/" + selected[kind]
        parsed = api.json("POST", "/api/v1/links/parse", {"text": link})[2]
        check(parsed["matched"] and parsed["ref"]["id"] == selected[kind], kind + " link resolves")

    track = {"source": "qq", "id": selected["tracks"]}
    lyrics = get("lyrics", id=track["id"])
    report["lyrics"] = {"available": not lyrics["unavailable"], "characters": len(lyrics.get("lrc") or "")}
    resolved = api.json("POST", "/api/v1/assets/resolve", {"track": track})[2]
    check(resolved["actual_quality"] == "standard" and resolved["completeness"] == "full", "standard full resource resolves")
    status, _, body = api.call("POST", "/api/v1/assets/resolve", {"track": track, "quality": "lossless"})
    strict = json.loads(body)
    if status == 200:
        check(strict["data"]["actual_quality"] == "lossless", "strict lossless never silently lowers quality")
        report["strict_lossless"] = "resolved_not_download_verified"
    else:
        check(strict["error"]["code"] in ("CREDENTIALS_REQUIRED", "QUALITY_UNAVAILABLE"), "unavailable lossless reports access or quality restriction")
        report["strict_lossless"] = strict["error"]["code"]
    lower = api.json("POST", "/api/v1/assets/resolve", {"track": track, "quality": "lossless", "quality_policy": "allow_lower"})[2]
    check(lower["actual_quality"] in ("standard", "high", "lossless"), "explicit fallback resolves an accessible quality")
    report["allow_lower"] = lower["actual_quality"]
    created = api.json("POST", "/api/v1/downloads", {"track": track}, {"Idempotency-Key": "qq-" + uuid.uuid4().hex}, expect=(200, 202))[2]
    task = wait_task(api, created["task_id"], {"completed", "failed", "canceled"}, timeout=180)
    if task["status"] != "completed":
        report["download_error"] = task.get("error")
    check(task["status"] == "completed", "QQ download completed")
    file_id = task["result"]["file_id"]
    status, _, payload = get_url(ticket_for(api, file_id))
    check(status == 200 and len(payload) > 10000, "published audio reads back")
    sha = hashlib.sha256(payload).hexdigest()
    tags = frames(payload)
    detail = get("tracks/detail", id=track["id"])
    check(text(tags["TIT2"][0]) == detail["title"], "embedded title matches QQ metadata")
    check("APIC" in tags and "USLT" in tags, "cover and lyrics are embedded")
    parts = {}
    for part in ("lyrics", "cover"):
        url = ticket_for(api, file_id)
        status, _, data = get_url(url + ("&" if "?" in url else "?") + "part=" + part)
        check(status == 200 and data, part + " sidecar reads back")
        parts[part] = hashlib.sha256(data).hexdigest()
    check(task["actual_quality"] == "standard", "downloaded bytes pass standard quality verification")
    replay = api.json("POST", "/api/v1/downloads", {"track": track}, {"Idempotency-Key": "qq-reuse-" + uuid.uuid4().hex}, expect=(200, 202))[2]
    check(replay["task_id"] == created["task_id"], "complete QQ file is reused")
    service.restart()
    api = Http(service.api, service.token)
    status, _, restored = get_url(ticket_for(api, file_id))
    check(status == 200 and hashlib.sha256(restored).hexdigest() == sha, "QQ file survives restart")
    for part, expected in parts.items():
        url = ticket_for(api, file_id)
        status, _, data = get_url(url + ("&" if "?" in url else "?") + "part=" + part)
        check(status == 200 and hashlib.sha256(data).hexdigest() == expected, part + " survives restart")
    report["download"] = {"track": track["id"], "quality": "standard", "bytes": len(payload), "sha256": sha,
                          "reused": True, "restart_readback": True, "sidecars": parts, "metadata_fields": task["result"]["metadata_fields"]}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--cookie-file", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--port", type=int, default=18912)
    args = parser.parse_args()
    service = OwnService(args.binary, args.port)
    report = {"status": "failed"}
    try:
        config = Path(service.config)
        settings = json.loads(config.read_text(encoding="utf-8"))
        settings["downloads"] = {"output": {"export_lyrics": True, "export_cover": True, "sidecars_required": True}}
        config.write_text(json.dumps(settings, ensure_ascii=False), encoding="utf-8")
        service.restart()
        run(service, args.cookie_file, report)
        report["status"] = "passed"
    finally:
        service.close()
        args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
