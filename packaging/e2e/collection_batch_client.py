#!/usr/bin/env python3
"""有限集合验收客户端。只调用公共 API；不属于服务运行依赖。

先持久保存批次请求及幂等键，再发送。--replay 只重放已保存请求，
不继续发现集合页；服务重启后旧分页游标不可用于继续导入。
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import time
import urllib.parse
import uuid

from kuwo_e2e import Http, Failure


def save(path, report):
    path = Path(path)
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("w", encoding="utf-8") as stream:
        json.dump(report, stream, ensure_ascii=False, indent=2)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def submit(api, record):
    result = api.json("POST", "/api/v1/download-batches", record["request"],
                      {"Idempotency-Key": record["key"]}, expect=(200, 202))[2]
    record["result"] = result
    return result


def replay(api, path):
    report = json.loads(Path(path).read_text(encoding="utf-8"))
    for record in report["batches"]:
        submit(api, record)
        save(path, report)
    report["submitted_items"] = sum(len(b["request"]["items"]) for b in report["batches"])
    save(path, report)
    return report


def collect(api, link, path, *, ids=None, page_size=50, batch_size=100,
            max_items=10000, max_pages=1000, stop_after_batches=None):
    if not 1 <= page_size <= 100 or not 1 <= batch_size <= 100 or max_items < 1 or max_pages < 1:
        raise ValueError("page/batch size must be 1..100; item/page bounds must be positive")
    if stop_after_batches is not None and stop_after_batches < 1:
        raise ValueError("stop_after_batches must be positive")
    # 不覆盖旧请求清单，避免丢失应答未知时的幂等键。
    if Path(path).exists():
        raise Failure("manifest already exists; use --replay or a new path")
    parsed = api.json("POST", "/api/v1/links/parse", {"text": link})[2]
    if not parsed["matched"] or parsed["kind"] not in ("playlist", "album", "artist", "podcast", "episode"):
        raise Failure("link is not a supported collection or episode")
    ref, kind = parsed["ref"], parsed["kind"]
    if kind == "artist" and page_size > 50:
        raise ValueError("artist hot songs page size must be at most 50")
    report = {"collection": {"kind": kind, "ref": ref}, "pages_read": 0,
              "fetched_items": 0, "submitted_items": 0, "unavailable_items": 0,
              "enumeration_finished": False, "stop_reason": None, "batches": []}
    save(path, report)
    pending, seen, cursor = [], set(), None

    def flush():
        record = {"key": "collection-" + uuid.uuid4().hex, "request": {"items": list(pending)}}
        report["batches"].append(record)
        save(path, report)  # 请求结果未知也能以原 key 重放。
        submit(api, record)
        report["submitted_items"] += len(pending)
        pending.clear()
        save(path, report)
        return stop_after_batches is not None and len(report["batches"]) >= stop_after_batches

    for _ in range(max_pages):
        query = {**ref, "limit": page_size}
        if cursor:
            query["cursor"] = cursor
        if kind == "episode":
            episode = api.json("GET", "/api/v1/episodes/detail?" + urllib.parse.urlencode(ref))[2]
            data = {"items": [episode], "page": {"has_more": False}}
        else:
            route = "podcasts/episodes" if kind == "podcast" else f"{kind}s/tracks"
            data = api.json("GET", f"/api/v1/{route}?" + urllib.parse.urlencode(query))[2]
        report["pages_read"] += 1
        report["fetched_items"] += len(data["items"])
        page = data["page"]
        report["last_page"] = {k: page.get(k) for k in ("total", "listed_total", "unavailable_count", "has_more")}
        report["unavailable_items"] += page.get("unavailable_count") or 0
        for item in data["items"]:
            if ids is not None and item["ref"]["id"] not in ids:
                continue
            audio = item.get("audio_ref") if kind in ("podcast", "episode") else item["ref"]
            if audio is None or item.get("requires_purchase"):
                report["unavailable_items"] += 1
                continue
            pending.append({"track": audio})
            at_bound = report["submitted_items"] + len(pending) >= max_items
            if len(pending) == batch_size or at_bound:
                stopped = flush()
                if stopped or at_bound:
                    report["stop_reason"] = "batch_bound" if stopped else "item_bound"
                    save(path, report)
                    return report
        cursor = page.get("next_cursor")
        if not cursor:
            if page.get("has_more") is True:
                raise Failure("has_more without a cursor; refusing to claim enumeration complete")
            report["enumeration_finished"] = True
            report["stop_reason"] = "end"
            break
        if cursor in seen:
            raise Failure("repeated cursor")
        seen.add(cursor)
        save(path, report)
    else:
        report["stop_reason"] = "page_bound"
    if pending:
        flush()
    save(path, report)
    return report


def monitor(api, path, timeout=300):
    report = json.loads(Path(path).read_text(encoding="utf-8"))
    deadline = time.monotonic() + timeout
    while True:
        stats = {}
        for record in report["batches"]:
            if "result" not in record:
                raise Failure("batch outcome unknown; --replay before monitoring")
            batch = api.json("GET", "/api/v1/download-batches/" + record["result"]["batch_id"])[2]
            record["status"] = batch
            for status, count in batch["stats"].items():
                stats[status] = stats.get(status, 0) + count
        report["task_entries_by_status"] = stats  # 重复引用按批次位置计数。
        save(path, report)
        if all(status in ("completed", "failed", "canceled") for status in stats):
            return report
        if time.monotonic() >= deadline:
            raise Failure("monitor timeout; accepted tasks continue on the service")
        time.sleep(0.5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--api", required=True)
    parser.add_argument("--token-file", required=True)
    parser.add_argument("--manifest", required=True)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--link")
    mode.add_argument("--replay", action="store_true")
    mode.add_argument("--monitor", action="store_true")
    select = parser.add_mutually_exclusive_group()
    select.add_argument("--all", action="store_true")
    select.add_argument("--ids", help="comma-separated ids; collection order and duplicate positions are retained")
    parser.add_argument("--page-size", type=int, default=50)
    parser.add_argument("--batch-size", type=int, default=100)
    parser.add_argument("--max-items", type=int, default=10000)
    parser.add_argument("--max-pages", type=int, default=1000)
    parser.add_argument("--stop-after-batches", type=int)
    parser.add_argument("--wait", action="store_true")
    args = parser.parse_args()
    if args.link and not (args.all or args.ids):
        parser.error("choose --all or --ids for a new collection")
    api = Http(args.api, Path(args.token_file).read_text(encoding="utf-8").strip())
    if args.link:
        report = collect(api, args.link, args.manifest, ids=set(args.ids.split(",")) if args.ids else None,
                         page_size=args.page_size, batch_size=args.batch_size, max_items=args.max_items,
                         max_pages=args.max_pages, stop_after_batches=args.stop_after_batches)
    elif args.replay:
        report = replay(api, args.manifest)
    if args.monitor or args.wait:
        report = monitor(api, args.manifest)
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
