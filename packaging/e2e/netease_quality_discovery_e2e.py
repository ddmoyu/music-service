#!/usr/bin/env python3
"""C 阶段：公共发现分页与本地 Cookie 账号的 high/lossless 真文件验收。

仅为独立测试客户端，生产协议、下载与媒体检查仍由原生服务执行。
Cookie 从文件读取，不输出 Cookie、账号昵称、用户 ID 或临时音源 URL。
"""
import argparse
import hashlib
import json
from pathlib import Path
import urllib.parse
import uuid

from kuwo_e2e import OwnService, Http, check, wait_task, ticket_for, get_url


def measured(data):
    """Independently inspect FLAC STREAMINFO or consecutive MPEG-1 layer III frames."""
    if data[:4] == b"fLaC":
        check(data[4] & 127 == 0 and int.from_bytes(data[5:8], "big") == 34, "FLAC STREAMINFO")
        packed = int.from_bytes(data[18:26], "big")
        rate = packed >> 44
        samples = packed & ((1 << 36) - 1)
        check(rate > 0 and samples > 0, "FLAC has real samples")
        return {"container": "flac", "sample_rate_hz": rate, "channels": ((packed >> 41) & 7) + 1,
                "bits_per_sample": ((packed >> 36) & 31) + 1, "duration_ms": round(samples * 1000 / rate)}
    at = 0
    if data[:3] == b"ID3":
        at = 10 + sum((data[6 + i] & 127) << (7 * (3 - i)) for i in range(4))
        if data[5] & 16:
            at += 10
    rates = (44100, 48000, 32000)
    bitrates = (0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320)
    frames, sizes, samples = 0, 0, 0
    while at + 4 <= len(data):
        word = int.from_bytes(data[at:at + 4], "big")
        if word >> 21 != 0x7ff or ((word >> 19) & 3) != 3 or ((word >> 17) & 3) != 1:
            break
        b, s = (word >> 12) & 15, (word >> 10) & 3
        if not (0 < b < 15 and s < 3):
            raise ValueError("invalid MP3 frame")
        rate = rates[s]
        size = 144000 * bitrates[b] // rate + ((word >> 9) & 1)
        if at + size > len(data):
            raise ValueError("incomplete MP3 frame")
        frames += 1
        sizes += size
        samples += 1152
        at += size
    check(frames > 100, "MP3 contains consecutive audio frames")
    return {"container": "mp3", "sample_rate_hz": rate, "frames": frames,
            "bitrate_bps": round(sizes * 8 * rate / samples), "duration_ms": round(samples * 1000 / rate)}


def run(service, cookie_file, report):
    api = Http(service.api, service.token)
    def get(route, **params):
        return api.json("GET", "/api/v1/" + route + "?" + urllib.parse.urlencode({"source": "netease", **params}))[2]
    def resolve(track_id, quality, policy="strict"):
        status, _, raw = api.call("POST", "/api/v1/assets/resolve", {
            "track": {"source": "netease", "id": track_id}, "quality": quality, "quality_policy": policy})
        return status, json.loads(raw)
    def expect_error(result, code):
        status, value = result
        check(status >= 400 and value["error"]["code"] == code, "expected " + code)
    def create_cookie(value):
        return api.json("POST", "/api/v1/credentials", {"source": "netease", "cookie": value,
            "label": "C acceptance"}, expect=(201,))[2]["credential_id"]

    report["discovery"] = {}
    categories = get("categories")
    check(categories["catalog_kind"] == "live" and any(x["id"] == "华语" for x in categories["items"]), "live category directory")
    report["discovery"]["categories"] = len(categories["items"])
    for route, params in [("categories/playlists", {"category_id": "华语"}),
                          ("recommendations/playlists", {}), ("charts/tracks", {"id": "3778678"})]:
        first = get(route, limit=2, **params)
        check(len(first["items"]) == 2 and first["page"]["next_cursor"], route + " first page")
        second = get(route, limit=2, cursor=first["page"]["next_cursor"], **params)
        check(len(second["items"]) == 2 and second["items"][0]["ref"] != first["items"][0]["ref"], route + " advances")
        report["discovery"][route] = {"first_count": 2, "second_count": 2,
            "page": {k: v for k, v in first["page"].items() if k != "next_cursor"}}
    charts = get("charts")
    check(charts["catalog_kind"] == "live" and any(x["ref"]["id"] == "3778678" for x in charts["items"]), "hot chart belongs to live directory")
    chart = get("charts/detail", id="3778678")
    check(chart["track_count"] > 2, "chart total is not page size")
    report["discovery"]["charts"] = len(charts["items"])
    report["discovery"]["hot_chart"] = {"id": "3778678", "name": chart["name"], "track_count": chart["track_count"]}

    expect_error(resolve("210049", "high"), "CREDENTIALS_REQUIRED")
    expect_error(resolve("5257138", "lossless"), "QUALITY_UNAVAILABLE")
    status, lower = resolve("5257138", "lossless", "allow_lower")
    check(status == 200 and lower["data"]["actual_quality"] == "high" and
          any(w["code"] == "QUALITY_LOWERED" for w in lower["data"]["warnings"]), "anonymous lower quality is explicit")
    report["anonymous"] = {"lossless_strict": "QUALITY_UNAVAILABLE", "lossless_allow_lower": "high",
                           "member_song": "CREDENTIALS_REQUIRED"}
    invalid = create_cookie("MUSIC_U=invalid-local-acceptance-session")
    verification = api.json("POST", f"/api/v1/credentials/{invalid}/verify", {})[2]
    check(verification["result"] == "invalid", "invalid session is invalid online")
    report["invalid_session"] = "invalid"
    credential = create_cookie(cookie_file.read_text(encoding="utf-8").strip())
    verified = api.json("POST", f"/api/v1/credentials/{credential}/verify", {})[2]
    check(verified["result"] == "valid", "provided account verifies online")
    report["verification"] = "valid"
    api.json("PUT", "/api/v1/credential-defaults/netease", {"credential_id": credential})
    expect_error(resolve("210049", "hires"), "QUALITY_UNAVAILABLE")
    report["files"] = []
    for quality in ("high", "lossless"):
        status, response = resolve("210049", quality)
        check(status == 200, quality + " resolves through API")
        asset = response["data"]
        check(asset["actual_quality"] == quality and asset["completeness"] == "full", quality + " has verified tier and full length")
        accepted = api.json("POST", "/api/v1/downloads", {"track": {"source": "netease", "id": "210049"},
            "quality": quality, "quality_policy": "strict", "credential_id": credential},
            {"Idempotency-Key": "stage-c-" + uuid.uuid4().hex}, expect=(200, 202))[2]
        task = wait_task(api, accepted["task_id"], ("completed", "failed"), timeout=180)
        check(task["status"] == "completed", quality + " download completes: " + str((task.get("error") or {}).get("code", "")))
        file = api.json("GET", f"/api/v1/files/{task['result']['file_id']}")[2]
        status, _, audio = get_url(ticket_for(api, task["result"]["file_id"]))
        check(status == 200 and len(audio) == file["size_bytes"], quality + " file readback")
        facts = measured(audio)
        check(facts["container"] == ("mp3" if quality == "high" else "flac"), "measured container")
        check(abs(facts["duration_ms"] - 294600) < 2000, "full song independently measured")
        if quality == "high":
            check(300000 <= facts["bitrate_bps"] <= 340000, "high really is approximately 320 kbps")
        report["files"].append({"requested": quality, "actual_quality": asset["actual_quality"],
            "upstream_bitrate_bps": asset["bitrate_bps"], "bytes": len(audio),
            "sha256": hashlib.sha256(audio).hexdigest(), "measured": facts})
    # Removing the default must not expose the authenticated asset via an anonymous resolve cache.
    check(api.call("DELETE", "/api/v1/credential-defaults/netease")[0] in (200, 204), "default removed")
    expect_error(resolve("210049", "high"), "CREDENTIALS_REQUIRED")
    report["default_removed"] = "anonymous request does not reuse authenticated audio"
    report["hires"] = "not verified; not advertised"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--cookie-file", type=Path, required=True)
    parser.add_argument("--account-condition", default="user-confirmed VIP1")
    parser.add_argument("--port", type=int, default=18740)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    service = OwnService(args.binary, args.port)
    report = {"account_condition": args.account_condition, "status": "incomplete"}
    try:
        run(service, args.cookie_file, report)
        report["status"] = "passed"
    finally:
        service.close()
        args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
