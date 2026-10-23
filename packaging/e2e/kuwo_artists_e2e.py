#!/usr/bin/env python3
"""酷我歌手目录与专辑封面 Windows 在线验收；只调用原生服务，无账号 Cookie。"""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import time
import urllib.parse
import urllib.request
import uuid

from kuwo_e2e import OwnService, Http, check, wait_task, ticket_for, get_url
from metadata_e2e import frames


def id3_text(frame):
    # 上游已有文本帧可保留合法的 UTF-16 编码，不要求改写为 UTF-8。
    encodings = ("latin-1", "utf-16", "utf-16-be", "utf-8")
    check(bool(frame) and frame[0] < len(encodings), "ID3 text has a supported encoding")
    return frame[1:].decode(encodings[frame[0]]).rstrip("\0")


def embedded_cover(payload, quality):
    if quality == "standard":
        tags = frames(payload)
        check(len(tags.get("APIC", [])) == 1, "MP3 has one cover")
        picture = tags["APIC"][0]
        check(picture[0] in (0, 3), "MP3 cover text encoding is supported")
        mime, rest = picture[1:].split(b"\0", 1)
        check(mime == b"image/jpeg" and rest[0] == 3, "MP3 cover is a front JPEG")
        return rest[1:].split(b"\0", 1)[1]
    check(payload.startswith(b"fLaC"), "lossless file is actual FLAC")
    at, pictures = 4, []
    while at + 4 <= len(payload):
        kind, size = payload[at], int.from_bytes(payload[at + 1:at + 4], "big")
        block = payload[at + 4:at + 4 + size]
        check(len(block) == size, "FLAC metadata block is complete")
        if kind & 127 == 6:
            pos = 0
            def number():
                nonlocal pos
                value = int.from_bytes(block[pos:pos + 4], "big")
                pos += 4
                return value
            check(number() == 3, "FLAC picture is a front cover")
            mime_length = number()
            mime = block[pos:pos + mime_length]
            pos += mime_length
            description_length = number()
            pos += description_length + 16  # 宽、高、位深、索引颜色数。
            length = number()
            check(mime == b"image/jpeg" and pos + length == len(block), "FLAC JPEG picture has consistent length")
            pictures.append(block[pos:pos + length])
        at += 4 + size
        if kind & 128:
            break
    check(len(pictures) == 1, "FLAC has one cover")
    return pictures[0]


def run(service, report):
    api = Http(service.api, service.token)
    def call(route, **params):
        params = {("sources" if route.startswith("search/") else "source"): "kuwo", **params}
        for attempt in range(3):
            status, _, payload = api.call("GET", "/api/v1/" + route + "?" + urllib.parse.urlencode(params))
            doc = json.loads(payload)
            if status == 200 or (doc.get("error") or {}).get("code") not in ("UPSTREAM_TIMEOUT", "UPSTREAM_RATE_LIMITED"):
                break
            report.setdefault("network_retries", []).append(route)
            if attempt < 2:
                time.sleep(1)
        check(status == 200, route + " succeeds: " + str((doc.get("error") or {}).get("code", status)))
        return doc

    first = call("search/artists", q="周杰伦", limit=2)
    check(any(x["ref"]["id"] == "336" for x in first["data"]["items"]), "artist search returns Zhou Jielun")
    cursor = first["meta"]["source_results"][0]["page"]["next_cursor"]
    second = call("search/artists", q="周杰伦", limit=2, cursor=cursor)
    check({x["ref"]["id"] for x in first["data"]["items"]}.isdisjoint(x["ref"]["id"] for x in second["data"]["items"]),
          "artist search second page advances")
    report["search"] = {"pages": 2, "returned": len(first["data"]["items"]) + len(second["data"]["items"])}
    report["details"] = {}
    for artist_id, expected in (("336", "周杰伦"), ("1062", "林俊杰")):
        detail = call("artists/detail", id=artist_id)["data"]
        check(detail["ref"]["id"] == artist_id and detail["name"] == expected, "artist detail preserves identity")
        check(detail["description"] and detail["alternate_name"] and detail["birth_date"] and detail["country"], "artist profile is populated")
        report["details"][artist_id] = {"name": detail["name"], "track_count": detail["track_count"],
            "album_count": detail["album_count"], "description_bytes": len(detail["description"].encode("utf-8"))}
    status, _, payload = api.call("GET", "/api/v1/artists/detail?source=kuwo&id=999999999999999999")
    check(status == 404 and json.loads(payload)["error"]["code"] == "NOT_FOUND", "unknown artist is not found")
    parsed = api.json("POST", "/api/v1/links/parse", {"text": "https://www.kuwo.cn/singer_detail/336/info"})[2]
    check(parsed["matched"] and parsed["kind"] == "artist" and parsed["ref"]["id"] == "336", "artist info link parses")

    report["pagination"] = {}
    for kind, limit in (("tracks", 100), ("albums", 10)):
        cursor, seen, count, pages = None, set(), 0, 0
        while True:
            params = {"id": "336", "limit": limit}
            if cursor:
                params["cursor"] = cursor
            data = call("artists/" + kind, **params)["data"]
            ids = {x["ref"]["id"] for x in data["items"]}
            check(len(ids) == len(data["items"]) and not (seen & ids), kind + " page has distinct entries")
            seen.update(ids)
            count += len(data["items"])
            pages += 1
            check(pages <= 60, "sample traversal stays bounded")
            cursor = data["page"]["next_cursor"]
            if pages == 1 and cursor:
                status, _, payload = api.call("GET", "/api/v1/artists/" + kind + "?" + urllib.parse.urlencode(
                    {"source": "kuwo", "id": "1062", "limit": limit, "cursor": cursor}))
                check(status == 400 and json.loads(payload)["error"]["code"] == "CURSOR_MISMATCH", "cursor cannot switch artists")
            if not cursor:
                check(data["page"]["has_more"] is False, "last page terminates")
                check(count == data["page"]["total"], "complete traversal matches declared total")
                break
        report["pagination"][kind] = {"pages": pages, "items": count, "distinct_ids": len(seen), "total": data["page"]["total"]}

    detail = call("tracks/detail", id="228908")["data"]
    check(detail["album"]["ref"]["id"] == "1293" and detail["artwork"], "track carries its album cover")
    with urllib.request.urlopen(detail["artwork"], timeout=30) as response:
        cover = response.read(8 * 1024 * 1024 + 1)
    check(cover.startswith(b"\xff\xd8") and len(cover) <= 8 * 1024 * 1024, "upstream album cover is a bounded JPEG")
    report["cover"] = {"bytes": len(cover), "sha256": hashlib.sha256(cover).hexdigest()}
    check(call("tracks/detail", id="78932517")["data"]["artwork"] is None, "missing album cover stays absent")
    report["downloads"] = {}
    for quality in ("standard", "lossless"):
        request = {"track": {"source": "kuwo", "id": "228908"}, "quality": quality, "metadata_policy": "required"}
        created = api.json("POST", "/api/v1/downloads", request, {"Idempotency-Key": "kuwo-artists-" + uuid.uuid4().hex}, expect=(200, 202))[2]
        task = wait_task(api, created["task_id"], {"completed", "failed", "canceled"}, timeout=180)
        check(task["status"] == "completed", quality + " download completed: " + str(task.get("error")))
        file_id = task["result"]["file_id"]
        status, _, payload = get_url(ticket_for(api, file_id))
        check(status == 200 and embedded_cover(payload, quality) == cover, quality + " embedded cover equals upstream image")
        if quality == "standard":
            tags = frames(payload)
            check(id3_text(tags["TIT2"][0]) == detail["title"], "MP3 title matches the song")
            check(id3_text(tags["TRCK"][0]).split("/")[0] == str(detail["track_number"]), "MP3 track number matches the album")
        url = ticket_for(api, file_id)
        status, _, sidecar = get_url(url + ("&" if "?" in url else "?") + "part=cover")
        check(status == 200 and sidecar == cover, "exported cover equals embedded cover")
        check(task["actual_quality"] == quality and task["result"]["metadata_fields"]["cover"] == "written", "actual quality and cover status verified")
        reused = api.json("POST", "/api/v1/downloads", request, {"Idempotency-Key": "kuwo-cover-reuse-" + uuid.uuid4().hex}, expect=(200, 202))[2]
        check(reused["task_id"] == task["task_id"], "published result is reused")
        report["downloads"][quality] = {"bytes": len(payload), "sha256": hashlib.sha256(payload).hexdigest(),
            "file_id": file_id, "reused": True, "cover_written": True}
    service.restart()
    api = Http(service.api, service.token)
    for quality, result in report["downloads"].items():
        file_id = result.pop("file_id")
        status, _, payload = get_url(ticket_for(api, file_id))
        check(status == 200 and hashlib.sha256(payload).hexdigest() == result["sha256"], quality + " survives restart")
        url = ticket_for(api, file_id)
        status, _, sidecar = get_url(url + ("&" if "?" in url else "?") + "part=cover")
        check(status == 200 and sidecar == cover, "cover sidecar survives restart")
        result["restart_readback"] = True


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--port", type=int, default=18952)
    args = parser.parse_args()
    service = OwnService(args.binary, args.port)
    report = {"status": "failed", "authentication": "anonymous"}
    try:
        path = Path(service.config)
        config = json.loads(path.read_text(encoding="utf-8"))
        config["downloads"] = {"output": {"embed_cover": True, "embed_lyrics": False,
            "export_cover": True, "export_lyrics": False, "sidecars_required": True}}
        path.write_text(json.dumps(config, ensure_ascii=False), encoding="utf-8")
        service.restart()
        run(service, report)
        report["status"] = "passed"
    finally:
        service.close()
        args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
