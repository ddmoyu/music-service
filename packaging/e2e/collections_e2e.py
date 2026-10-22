#!/usr/bin/env python3
"""B 阶段在线验收：独立临时服务、三平台集合与网易云有限批次／恢复／文件取回。"""
import argparse
import hashlib
import json
from pathlib import Path
import urllib.parse

from kuwo_e2e import OwnService, Http, check, wait_task, ticket_for, get_url
from collection_batch_client import collect, replay, monitor


def query(ref, **extra):
    return urllib.parse.urlencode({**ref, **extra})


def run(service):
    api = Http(service.api, service.token)
    report = {"collections": [], "files": []}
    samples = [
        ("netease", "playlist", "https://music.163.com/#/playlist?id=3778678"),
        ("netease", "album", "https://music.163.com/album?id=512175"),
        ("kuwo", "playlist", "https://www.kuwo.cn/playlist_detail/2234580349"),
        ("kuwo", "album", "https://www.kuwo.cn/album_detail/1293"),
        ("fivesing", "playlist", "https://5sing.kugou.com/62571411/dj/59b6bcf8da767f5ace4bb449.html")]
    chosen = {}
    for source, kind, link in samples:
        parsed = api.json("POST", "/api/v1/links/parse", {"text": link})[2]
        check(parsed["matched"] and parsed["kind"] == kind and parsed["ref"]["source"] == source,
              f"{source} {kind}: link keeps entity kind")
        ref = parsed["ref"]
        detail = api.json("GET", f"/api/v1/{kind}s/detail?" + query(ref))[2]
        first = api.json("GET", f"/api/v1/{kind}s/tracks?" + query(ref, limit=10))[2]
        check(len(first["items"]) > 0 and first["page"]["next_cursor"], f"{source} {kind}: first page")
        second = api.json("GET", f"/api/v1/{kind}s/tracks?" + query(ref, limit=10, cursor=first["page"]["next_cursor"]))[2]
        check(len(second["items"]) > 0, f"{source} {kind}: second page")
        report["collections"].append({"source": source, "kind": kind, "ref": ref,
                                      "first_page": {k: v for k, v in first["page"].items() if k != "next_cursor"},
                                      "second_count": len(second["items"]), "detail": detail})
        if source == "netease":
            check(first["page"]["listed_total"] >= len(first["items"]) and first["page"]["unavailable_count"] == 0,
                  "declared, visible and unavailable counts are separate")
            # 选取当前匿名条件下有完整音源的真实曲目，不把目录可见等同可下载。
            for item in first["items"] + second["items"]:
                status, _, raw = api.call("POST", "/api/v1/assets/resolve", {"track": item["ref"]})
                if status == 200 and json.loads(raw)["data"]["completeness"] == "full":
                    chosen[kind] = (link, item["ref"]["id"])
                    break
            check(kind in chosen, f"{kind}: a selected anonymous full track is playable")

    for kind, keyword in [("playlists", "周杰伦"), ("albums", "叶惠美")]:
        params = {"q": keyword, "sources": "netease", "limit": 2}
        status, _, raw = api.call("GET", f"/api/v1/search/{kind}?" + query(params))
        search = json.loads(raw)
        check(status == 200 and len(search["data"]["items"]) == 2 and not search["meta"]["partial"], f"netease search {kind}")
        cursor = search["meta"]["source_results"][0]["page"]["next_cursor"]
        check(bool(cursor), "search has a second page")
        next_page = api.json("GET", f"/api/v1/search/{kind}?" + query(params, cursor=cursor))[2]
        check(len(next_page["items"]) > 0 and next_page["items"][0]["ref"] != search["data"]["items"][0]["ref"],
              f"{kind} search cursor advances")

    # 暂停领取，接受后停止客户端、硬重启服务：已受理工作恢复，不依赖客户端存活。
    api.json("PATCH", "/api/v1/admin/download-policy", {"paused": True})
    paths = []
    for kind, (link, track_id) in chosen.items():
        path = Path(service.work) / f"{kind}-manifest.json"
        accepted = collect(api, link, path, ids={track_id}, page_size=10, batch_size=1, stop_after_batches=1)
        check(accepted["submitted_items"] == 1 and not accepted["enumeration_finished"],
              f"{kind}: exactly one batch accepted; future pages not claimed")
        paths.append(path)
    service.restart()
    api = Http(service.api, service.token)
    api.json("PATCH", "/api/v1/admin/download-policy", {"paused": False})
    for path in paths:
        original = json.loads(path.read_text(encoding="utf-8"))["batches"][0]["result"]
        repeated = replay(api, path)["batches"][0]["result"]
        check(original["batch_id"] == repeated["batch_id"], "restart preserves batch idempotency")
        monitored = monitor(api, path)
        check(monitored["task_entries_by_status"] == {"completed": 1}, "accepted batch completes after client stop and service restart")
        task_id = repeated["items"][0]["task_id"]
        task = wait_task(api, task_id, ("completed", "failed"))
        file_id = task["result"]["file_id"]
        file = api.json("GET", f"/api/v1/files/{file_id}")[2]
        status, _, data = get_url(ticket_for(api, file_id))
        check(status == 200 and len(data) == file["size_bytes"] and len(data) > 100000, "batch file is retrievable")
        report["files"].append({"collection_kind": path.stem, "task_id": task_id, "size": len(data),
                                "sha256": hashlib.sha256(data).hexdigest(), "name": file["display_name"]})
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--port", type=int, default=18670)
    args = parser.parse_args()
    service = OwnService(args.binary, args.port)
    try:
        print(json.dumps(run(service), ensure_ascii=False, indent=2))
    finally:
        service.close()


if __name__ == "__main__":
    main()
