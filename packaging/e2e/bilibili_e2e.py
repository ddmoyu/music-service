#!/usr/bin/env python3
"""Bilibili 原生接口验收：匿名分页、分 P、独立音轨、下载、取消和重启回读。"""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import time
import urllib.parse
import uuid

from kuwo_e2e import OwnService, Http, check, wait_task, ticket_for, get_url


def run(service, args, report):
    api = Http(service.api, service.token)
    report["service_version"] = api.json("GET", "/api/v1/info")[2].get("version")

    def get(route, **params):
        query = {"source": "bilibili", **params}
        if route.startswith("search/"):
            query.pop("source")
            query["sources"] = "bilibili"
        path = "/api/v1/" + route + "?" + urllib.parse.urlencode(query)
        status, _, raw = api.call("GET", path)
        doc = json.loads(raw)
        # 只报告错误类别，不输出上游正文、Cookie 或临时音源地址。
        if status != 200:
            report["request_error"] = {"route": route, "http": status, "code": doc.get("error", {}).get("code"),
                                       "sources": [{"source": s.get("source"), "code": (s.get("error") or {}).get("code")}
                                                   for s in doc.get("meta", {}).get("source_results", [])]}
        check(status == 200, route + " succeeds")
        return doc

    invalid = api.json("POST", "/api/v1/credentials", {
        "source": "bilibili", "kind": "cookie", "auth_profile": "web_cookie", "secret": {"cookie": "SESSDATA=synthetic-invalid"}, "label": "Bilibili invalid acceptance"}, expect=(201,))[2]
    verified = api.wait_credential(invalid["credential_id"])
    check(verified["login_status"] == "invalid", "invalid session is rejected")
    report["invalid_cookie"] = "invalid"

    # 先验证无凭证搜索，用户 Cookie 为独立的可选验收。
    pages = []
    cursor = None
    for _ in range(3):
        params = {"q": args.keyword, "limit": 3}
        if cursor:
            params["cursor"] = cursor
        page = get("search/tracks", **params)
        items = page["data"]["items"]
        check(items and all(not item["artists"] for item in items), "search keeps uploader separate from artists")
        pages.append([item["ref"]["id"] for item in items])
        cursor = page["meta"]["source_results"][0]["page"]["next_cursor"]
        if not cursor:
            break
    check(len(pages) >= 2 and pages[0] != pages[1], "anonymous search advances pages")
    report["search"] = {"pages": len(pages), "counts": [len(page) for page in pages]}
    check(get("search/tracks", q="piano", limit=2)["data"]["items"], "English search works")

    if args.cookie_file:
        credential = api.json("POST", "/api/v1/credentials", {
            "source": "bilibili", "kind": "cookie", "auth_profile": "web_cookie", "secret": {"cookie": args.cookie_file.read_text(encoding="utf-8").strip()},
            "label": "Bilibili acceptance"}, expect=(201,))[2]
        verified = api.wait_credential(credential["credential_id"])
        check(verified["login_status"] == "valid", "provided session verifies")
        api.json("PUT", "/api/v1/credential-defaults/bilibili", {"credential_id": credential["credential_id"]})
        report["valid_cookie"] = "passed"
    else:
        report["valid_cookie"] = "not_tested_no_account_supplied"

    link = "https://www.bilibili.com/video/" + args.video + "/?p=" + str(args.page)
    parsed = api.json("POST", "/api/v1/links/parse", {"text": link})[2]
    check(parsed["matched"] and parsed["ref"]["id"] == args.video + ":p" + str(args.page), "link preserves requested part")
    detail = get("tracks/detail", id=parsed["ref"]["id"])["data"]
    check(":cid" in detail["ref"]["id"] and not detail["artists"], "detail fixes cid and does not fabricate an artist")
    track = detail["ref"]
    report["track"] = track
    if args.multipart_video:
        one = get("tracks/detail", id=args.multipart_video + ":p1")["data"]
        two = get("tracks/detail", id=args.multipart_video + ":p2")["data"]
        check(one["ref"] != two["ref"], "different parts resolve to different cid identities")
        report["multipart"] = {"first": one["ref"]["id"], "second": two["ref"]["id"]}

    resolved = api.json("POST", "/api/v1/assets/resolve", {"track": track})[2]
    check(resolved["actual_track"] == track and resolved["container"] == "m4a" and
          resolved["actual_quality"] == "standard" and resolved["completeness"] == "full", "full independent AAC resolves")
    check("url" not in resolved, "public asset does not disclose CDN URL")
    report["resolved"] = {k: resolved[k] for k in ("container", "bitrate_bps", "actual_quality", "completeness")}
    status, _, body = api.call("POST", "/api/v1/assets/resolve", {"track": track, "quality": "lossless"})
    check(status != 200 and json.loads(body)["error"]["code"] == "QUALITY_UNAVAILABLE", "strict lossless is refused")
    lower = api.json("POST", "/api/v1/assets/resolve", {"track": track, "quality": "lossless", "quality_policy": "allow_lower"})[2]
    check(lower["actual_quality"] == "standard", "explicit fallback reports standard")

    created = api.json("POST", "/api/v1/downloads", {"track": track},
        {"Idempotency-Key": "bilibili-" + uuid.uuid4().hex}, expect=(200, 202))[2]
    task = wait_task(api, created["task_id"], {"completed", "failed", "canceled"}, timeout=180)
    if task["status"] != "completed":
        report["download_error"] = task.get("error")
    check(task["status"] == "completed", "download passes common file validation")
    file_id = task["result"]["file_id"]
    status, _, payload = get_url(ticket_for(api, file_id))
    check(status == 200 and len(payload) > 10000 and payload[4:8] == b"ftyp", "published M4A reads back")
    digest = hashlib.sha256(payload).hexdigest()
    check(task["actual_quality"] == "standard", "published quality remains standard")
    report["download"] = {"bytes": len(payload), "sha256": digest, "quality": task["actual_quality"],
                          "warnings": task.get("warnings", [])}
    reused = api.json("POST", "/api/v1/downloads", {"track": track},
        {"Idempotency-Key": "bilibili-reuse-" + uuid.uuid4().hex}, expect=(200, 202))[2]
    check(reused["task_id"] == created["task_id"], "completed file is reused")

    forced = api.json("POST", "/api/v1/downloads", {"track": track, "duplicate_policy": "force"},
        {"Idempotency-Key": "bilibili-cancel-" + uuid.uuid4().hex}, expect=(202,))[2]
    status, _, _ = api.call("POST", "/api/v1/downloads/" + forced["task_id"] + "/cancel")
    ended = wait_task(api, forced["task_id"], {"canceled", "completed", "failed"}, timeout=30)
    check((status in (200, 202) and ended["status"] == "canceled" and ended["result"] is None) or
          (status == 409 and ended["status"] == "completed"), "cancel has a consistent terminal result")
    report["cancel"] = ended["status"]

    service.restart()
    api = Http(service.api, service.token)
    restored = api.json("GET", "/api/v1/downloads/" + created["task_id"])[2]
    check(restored["status"] == "completed", "download task survives restart")
    status, _, payload = get_url(ticket_for(api, file_id))
    check(status == 200 and hashlib.sha256(payload).hexdigest() == digest, "file SHA-256 survives restart")
    report["restart"] = "passed"


def main():
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--port", type=int, default=18372)
    parser.add_argument("--video", default="BV1Mq4y1U7Lw", help="公开可访问的短音轨样本 BV 号")
    parser.add_argument("--page", type=int, default=1)
    parser.add_argument("--keyword", default="CC0 音乐")
    parser.add_argument("--multipart-video")
    parser.add_argument("--cookie-file", type=Path)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    args.report.parent.mkdir(parents=True, exist_ok=True)
    report = {"status": "failed"}
    service = None
    started = time.monotonic()
    try:
        service = OwnService(args.binary, args.port)
        run(service, args, report)
        report["status"] = "passed"
    finally:
        if service:
            service.close()
        report["seconds"] = round(time.monotonic() - started, 1)
        args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
