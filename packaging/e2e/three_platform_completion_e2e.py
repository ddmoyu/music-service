#!/usr/bin/env python3
"""三平台补齐在线验收；独立测试客户端，生产代码不依赖 Python。

报告只保留公开曲目 ID、文件证据和错误码，不记录 Cookie、账号信息或音源 URL。
"""
import argparse
import hashlib
import json
from pathlib import Path
import urllib.parse
import uuid

from kuwo_e2e import OwnService, Http, check, wait_task, ticket_for, get_url
from metadata_e2e import frames
from netease_quality_discovery_e2e import measured


def run(service, cookie_file, report):
    api = Http(service.api, service.token)
    def get(route, source, **params):
        if route.startswith("search/"):
            status, _, raw = api.call("GET", "/api/v1/" + route + "?" + urllib.parse.urlencode({"sources": source, **params}))
            body = json.loads(raw)
            check(status == 200, "search succeeds")
            return {**body["data"], "page": body["meta"]["source_results"][0]["page"]}
        return api.json("GET", "/api/v1/" + route + "?" + urllib.parse.urlencode({"source": source, **params}))[2]
    def resolve(source, track, quality, policy="strict"):
        status, _, raw = api.call("POST", "/api/v1/assets/resolve", {"track": {"source": source, "id": track},
            "quality": quality, "quality_policy": policy})
        return status, json.loads(raw)

    report["catalog"] = {}
    for route, params in [("search/artists", {"q": "周杰伦"}), ("artists/albums", {"id": "6452"})]:
        first = get(route, "netease", limit=2, **params)
        check(len(first["items"]) == 2 and first["page"]["next_cursor"], route + " first page")
        second = get(route, "netease", limit=2, cursor=first["page"]["next_cursor"], **params)
        check(len(second["items"]) == 2 and first["items"][0]["ref"] != second["items"][0]["ref"], route + " advances")
        report["catalog"][route] = {"first_count": 2, "second_count": 2}
    for source, track in [("kuwo", "228908"), ("fivesing", "fc-16394286")]:
        lyrics = get("lyrics", source, id=track)
        check(lyrics["lrc"] and not lyrics["unavailable"], source + " lyrics available")
        report["catalog"][source + "_lyrics"] = {"characters": len(lyrics["lrc"])}

    credential = api.json("POST", "/api/v1/credentials", {"source": "netease",
        "kind": "cookie", "auth_profile": "web_cookie", "secret": {"cookie": cookie_file.read_text(encoding="utf-8").strip()}, "label": "E acceptance"}, expect=(201,))[2]["credential_id"]
    verified = api.wait_credential(credential)
    check(verified["login_status"] == "valid", "provided account verifies")
    api.json("PUT", "/api/v1/credential-defaults/netease", {"credential_id": credential})
    status, strict = resolve("netease", "210049", "hires")
    lower_status, lower = resolve("netease", "210049", "hires", "allow_lower")
    check(lower_status == 200, "Hi-Res allow_lower resolves")
    actual = lower["data"]["actual_quality"]
    if actual != "hires":
        check(status >= 400 and strict["error"]["code"] == "QUALITY_UNAVAILABLE", "strict rejects account downgrade")
        check(any(w["code"] == "QUALITY_LOWERED" for w in lower["data"]["warnings"]), "downgrade is explicit")
    else:
        check(status == 200, "Hi-Res strict resolves for an entitled sample")
    report["netease_hires"] = {"requested": "hires", "actual": actual, "strict": "accepted" if status == 200 else strict["error"]["code"]}

    report["files"] = []
    files = []
    for source, track, quality, policy in [("kuwo", "228908", "high", "strict"),
        ("kuwo", "228908", "lossless", "strict"), ("fivesing", "bz-2493423", "high", "strict"),
        ("netease", "210049", "hires", "allow_lower")]:
        created = api.json("POST", "/api/v1/downloads", {"track": {"source": source, "id": track},
            "quality": quality, "quality_policy": policy, "metadata_policy": "required"},
            {"Idempotency-Key": "stage-e-" + uuid.uuid4().hex}, expect=(200, 202))[2]
        task = wait_task(api, created["task_id"], ("completed", "failed"), timeout=240)
        check(task["status"] == "completed", source + " " + quality + " completes: " + str(task.get("error")))
        file_id = task["result"]["file_id"]
        status, _, audio = get_url(ticket_for(api, file_id))
        check(status == 200, "published file readable")
        facts = measured(audio)
        if quality == "high":
            check(facts["container"] == "mp3" and 300000 <= facts["bitrate_bps"] <= 340000, "measured high MP3")
        else:
            check(facts["container"] == "flac", "measured FLAC")
        if source == "kuwo":
            check(abs(facts["duration_ms"] - 269000) < 2000, "full song duration")
            if quality == "high":
                embedded = frames(audio)["USLT"][0][4:].split(b"\0", 1)[1].decode("utf-8").rstrip("\0")
                check(embedded == get("lyrics", "kuwo", id=track)["lrc"], "Kuwo embedded lyrics match API")
        if task["actual_quality"] == "hires":
            check(facts["sample_rate_hz"] >= 48000 and facts["bits_per_sample"] >= 24, "measured Hi-Res")
        digest = hashlib.sha256(audio).hexdigest()
        files.append((file_id, digest))
        report["files"].append({"source": source, "track": track, "requested": quality,
            "actual": task["actual_quality"], "bytes": len(audio), "sha256": digest, "measured": facts})
    service.restart()
    api = Http(service.api, service.token)
    for file_id, digest in files:
        status, _, audio = get_url(ticket_for(api, file_id))
        check(status == 200 and hashlib.sha256(audio).hexdigest() == digest, "file survives service restart")
    report["restart_readback"] = len(files)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--cookie-file", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument("--port", type=int, default=18872)
    args = parser.parse_args()
    service = OwnService(args.binary, args.port)
    report = {"status": "incomplete", "account_condition": "user-confirmed VIP1"}
    try:
        run(service, args.cookie_file, report)
        report["status"] = "passed"
    finally:
        service.close()
        args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
