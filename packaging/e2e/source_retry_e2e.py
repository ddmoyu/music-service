#!/usr/bin/env python3
"""真实平台换源验收；仅复用标准库与原生服务，不导入账号或输出上游 URL。"""

import argparse
import datetime
import hashlib
import http.client
import json
from pathlib import Path
import tempfile
import time
import urllib.parse
import uuid

from kuwo_e2e import OwnService


SAMPLES = [
    ("江南", "b83dbb8e3dd3fcf86bf32690b91547f5"),
    ("美人鱼", "cdae8c7e2faf7f08dc18e78a1d65117b"),
]


class LiveFailure(Exception):
    pass


def public_error(doc):
    error = doc.get("error") or {}
    return {"code": error.get("code"), "details": [
        item for item in error.get("details", [])
        if item.get("field") in ("attempted_sources", "next_action", "source_retry")
    ]}


def identity(track):
    return {key: track.get(key) for key in ("ref", "title", "artists", "duration_ms", "version")}


def validate_report(report):
    """独立复核实际响应，不用 source_switched 标志代替身份和统计核验。"""
    checked = []

    def expect(ok, message):
        checked.append({"check": message, "passed": bool(ok)})

    def counts(snapshot):
        return {item["source"]: (item["successes"], item["failures"]) for item in snapshot["items"]}

    previous = report["statistics_initial"]
    for sample in report["samples"]:
        original = sample["original_identity"]
        origin = original["ref"]["source"]
        expect((sample["original_resolve"].get("error") or {}).get("code") in
               ("CREDENTIALS_REQUIRED", "UPSTREAM_TIMEOUT", "UPSTREAM_FAILED", "UPSTREAM_RATE_LIMITED",
                "RESOURCE_EXPIRED", "REQUEST_DEADLINE_EXCEEDED"),
               sample["title"] + ": original audio resolution really fails")
        for action in ("audition", "download"):
            result = sample[action]
            actual = result.get("actual_identity")
            expect(actual is not None, sample["title"] + ": " + action + " obtains a real replacement")
            if actual is None:
                continue
            expect(actual["title"] == original["title"] and
                   sorted(a["name"] for a in actual["artists"]) == sorted(a["name"] for a in original["artists"]),
                   sample["title"] + ": " + action + " keeps the exact title and complete artist list")
            expect(actual["version"]["kind"] == original["version"]["kind"] and
                   actual["version"]["label"] == original["version"]["label"],
                   sample["title"] + ": " + action + " has consistent version metadata")
            expect(abs(actual["duration_ms"] - original["duration_ms"]) <= 2000,
                   sample["title"] + ": " + action + " duration differs by at most 2000 ms")
            expect(result["source_switched"] and result["actual_track"]["source"] != origin,
                   sample["title"] + ": " + action + " reports a different platform")
            ranked = sorted((s for s in previous["items"] if s["source"] != origin and s["ranking_score"] > 0.5),
                            key=lambda s: (-s["ranking_score"], s["source"]))
            if ranked:
                first_request = sample.get("audition_initial", result) if action == "audition" else result
                expect(first_request["source_retry"]["attempted_sources"][1] == ranked[0]["source"],
                       sample["title"] + ": " + action + " tries the previous highest-success source first")
        audition, download = sample["audition"], sample["download"]
        expect(audition.get("range_read", {}).get("http_status") == 206 and
               audition.get("range_read", {}).get("bytes") == 65536 and
               audition.get("full_read", {}).get("http_status") == 200 and audition.get("full_read", {}).get("bytes", 0) > 65536,
               sample["title"] + ": switched audition supports Range and complete byte transfer")
        expect(download["status"] == "completed" and download["attempt"] >= 2 and
               download["readback"]["sha256"] == download["file"]["sha256"] and
               download["readback"]["bytes"] == download["file"]["size_bytes"],
               sample["title"] + ": retried native download publishes a verified readable file")
        before, after = counts(previous), counts(sample["statistics_after"])
        delta = {s: (after.get(s, (0, 0))[0] - before.get(s, (0, 0))[0],
                     after.get(s, (0, 0))[1] - before.get(s, (0, 0))[1]) for s in before.keys() | after.keys()}
        success_count = 1 if download["status"] == "completed" else 0
        expect(sum(v[0] for v in delta.values()) == success_count and
               delta.get(download["actual_track"]["source"], (0, 0))[0] == success_count and
               sum(v[1] for v in delta.values()) == download["attempt"] - success_count and delta[origin][1] >= 1,
               sample["title"] + ": real download successes and failed attempts are counted exactly once")
        previous = sample["statistics_after"]
        if "audition_followup" in sample:
            followup = sample["audition_followup"]
            if followup["http_status"] == 422 and followup.get("error", {}).get("code") == "SOURCE_EXHAUSTED":
                returned = {item["reason"] for item in followup["error"]["details"] if item["field"] == "attempted_sources"}
                expect(set(followup["excluded_before_request"]).issubset(returned),
                       sample["title"] + ": exhausted explicit retry retains exclusions instead of returning an old source")
            else:
                expect(followup["http_status"] == 200 and
                       followup["actual_track"]["source"] not in followup["excluded_before_request"] and
                       followup["range_read"]["http_status"] == 206 and followup["range_read"]["bytes"] == 65536 and
                       followup["full_read"]["http_status"] == 200 and followup["full_read"]["bytes"] > 65536,
                       sample["title"] + ": a second explicit audition switch excludes all previous sources and reads audio")
    expect(report["statistics_before_restart"] == report["statistics_after_restart"],
           "statistics retain exactly the same counts and rankings after restart")
    return checked


def run(binary, output):
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    report = {"started_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "authentication": "anonymous", "samples": [], "checks": [], "status": "running"}
    service = None

    def save():
        (output / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")

    def log(kind, data):
        print(json.dumps({"event": kind, **data}, ensure_ascii=True), flush=True)
        save()

    def require(ok, message):
        report["checks"].append({"check": message, "passed": bool(ok)})
        if not ok:
            raise LiveFailure(message)

    try:
        service = OwnService(str(binary), 0)
        settings = json.loads(Path(service.config).read_text(encoding="utf-8"))
        settings["features"] = {"media_proxy": True}
        settings["download_timeouts"] = {"connect_seconds": 10, "read_idle_seconds": 15,
                                          "attempt_seconds": 90, "absolute_seconds": 240}
        settings["downloads"] = {"max_file_bytes": 32 * 1024 * 1024}
        Path(service.config).write_text(json.dumps(settings, ensure_ascii=False), encoding="utf-8")
        service.restart()

        def api(method, route, data=None, key=None):
            endpoint = urllib.parse.urlsplit(service.api)
            conn = http.client.HTTPConnection(endpoint.hostname, endpoint.port, timeout=25)
            headers = {"Authorization": "Bearer " + service.token}
            if key:
                headers["Idempotency-Key"] = key
            if data is not None:
                headers["Content-Type"] = "application/json"
            started = time.monotonic()
            try:
                conn.request(method, route, None if data is None else json.dumps(data).encode(), headers)
                response = conn.getresponse()
                payload = response.read(2 * 1024 * 1024 + 1)
                if len(payload) > 2 * 1024 * 1024:
                    raise LiveFailure("API response exceeded 2 MiB")
                return response.status, json.loads(payload), round((time.monotonic() - started) * 1000, 2)
            finally:
                conn.close()

        def data(method, route, payload=None, key=None):
            for attempt in range(3):
                status, doc, _ = api(method, route, payload, key)
                if method != "GET" or public_error(doc)["code"] not in ("UPSTREAM_TIMEOUT", "UPSTREAM_RATE_LIMITED"):
                    break
                report.setdefault("network_retries", []).append({"route": route.split("?")[0],
                    "code": public_error(doc)["code"], "attempt": attempt + 1})
                if attempt < 2:
                    time.sleep(1)
            if status not in (200, 201, 202):
                raise LiveFailure(f"{method} {route.split('?')[0]}: HTTP {status} {public_error(doc)['code']}")
            return doc["data"]

        def detail(ref):
            return data("GET", "/api/v1/tracks/detail?" + urllib.parse.urlencode(ref))

        def stats():
            return data("GET", "/api/v1/source-statistics")

        def media(kind, item_id, name=None, prefix=False):
            access = data("POST", "/api/v1/media-access", {
                "target": {"kind": kind, "id": item_id}, "mode": "proxy", "auth": "bearer"})
            url = urllib.parse.urlsplit(access["url"])
            require(url.hostname in ("127.0.0.1", "localhost", "::1"), "media uses the local service")
            conn = http.client.HTTPConnection(url.hostname, url.port, timeout=20)
            headers = {"Authorization": "Bearer " + service.token}
            if prefix:
                headers["Range"] = "bytes=0-65535"
            count, digest, leading = 0, hashlib.sha256(), b""
            target = open(output / name, "xb") if name else None
            started = time.monotonic()
            try:
                conn.request("GET", url.path + ("?" + url.query if url.query else ""), headers=headers)
                response = conn.getresponse()
                if response.status not in ((200, 206) if prefix else (200,)):
                    return {"http_status": response.status, "bytes": 0}
                while chunk := response.read(65536):
                    count += len(chunk)
                    if count > 32 * 1024 * 1024 or time.monotonic() - started > 120:
                        raise LiveFailure("media exceeded its size or time budget")
                    digest.update(chunk)
                    if not leading:
                        leading = chunk[:16]
                    if target:
                        target.write(chunk)
                return {"http_status": response.status, "bytes": count, "sha256": digest.hexdigest(),
                        "leading_hex": leading.hex(), "elapsed_ms": round((time.monotonic() - started) * 1000, 2)}
            finally:
                if target:
                    target.close()
                conn.close()

        report["info"] = {k: v for k, v in data("GET", "/api/v1/info").items() if k in ("version", "name")}
        report["providers"] = [p["source"] for p in data("GET", "/api/v1/providers")["items"]]
        report["statistics_initial"] = stats()
        require(not report["statistics_initial"]["items"], "new service starts with no download statistics")
        log("ready", {"providers": report["providers"]})
        completed = []
        for index, (title, song_id) in enumerate(SAMPLES):
            ref = {"source": "kugou", "id": song_id}
            sample = {"title": title, "original_track": ref}
            report["samples"].append(sample)
            sample["original_identity"] = identity(detail(ref))
            require(sample["original_identity"]["title"] == title, "live original detail confirms the sample title")
            status, doc, elapsed = api("POST", "/api/v1/assets/resolve", {"track": ref})
            sample["original_resolve"] = {"http_status": status, "elapsed_ms": elapsed,
                                           "error": public_error(doc) if status != 200 else None}
            log("original_resolve", {"title": title, **sample["original_resolve"]})

            before = stats()
            status, doc, elapsed = api("POST", "/api/v1/assets/switch-source", {
                "track": ref, "attempted_sources": ["kugou"], "allow_preview": False})
            sample["audition"] = {"http_status": status, "elapsed_ms": elapsed}
            if status == 200:
                asset = doc["data"]
                sample["audition"].update({k: asset[k] for k in
                    ("original_track", "actual_track", "source_switched", "source_retry", "completeness", "container")})
                sample["audition"]["actual_identity"] = identity(detail(asset["actual_track"]))
                require(asset["source_switched"] and asset["original_track"] == ref and asset["actual_track"]["source"] != "kugou",
                        "audition explicitly reports its switched source")
                sample["audition"]["range_read"] = media("asset", asset["asset_id"], prefix=True)
                sample["audition"]["full_read"] = media("asset", asset["asset_id"], f"{index + 1}-audition.{asset['container']}")
            else:
                sample["audition"]["error"] = public_error(doc)
            require(stats() == before, "audition does not change download statistics")
            log("audition", {"title": title, **sample["audition"]})

            audition = sample["audition"]
            playback_failed = audition.get("range_read", {}).get("http_status") != 206 or audition.get("full_read", {}).get("http_status") != 200
            if audition["http_status"] == 200 and (index == 0 or playback_failed):
                excluded = audition["source_retry"]["attempted_sources"]
                follow_status, follow_doc, follow_ms = api("POST", "/api/v1/assets/switch-source", {
                    "track": ref, "attempted_sources": excluded, "allow_preview": False})
                followup = {"http_status": follow_status, "elapsed_ms": follow_ms, "excluded_before_request": excluded,
                            "trigger": "media_request_failed" if playback_failed else "explicit_second_switch"}
                sample["audition_followup"] = followup
                if follow_status == 200:
                    asset = follow_doc["data"]
                    followup.update({k: asset[k] for k in
                        ("original_track", "actual_track", "source_switched", "source_retry", "completeness", "container")})
                    followup["actual_identity"] = identity(detail(asset["actual_track"]))
                    followup["range_read"] = media("asset", asset["asset_id"], prefix=True)
                    followup["full_read"] = media("asset", asset["asset_id"], f"{index + 1}-audition-followup.{asset['container']}")
                    if playback_failed:
                        sample["audition_initial"] = audition
                        sample["audition"] = followup
                else:
                    followup["error"] = public_error(follow_doc)
                require(stats() == before, "a second audition switch still does not change download statistics")
                log("audition_followup", {"title": title, **followup})

            created = data("POST", "/api/v1/downloads", {"track": ref, "metadata_policy": "skip"},
                           "source-retry-live-" + uuid.uuid4().hex)
            deadline = time.monotonic() + 250
            history, last, controls = [], None, []
            while time.monotonic() < deadline:
                task = data("GET", "/api/v1/downloads/" + created["task_id"])
                marker = (task["status"], task["attempt"], json.dumps(task.get("actual_track"), sort_keys=True))
                if marker != last:
                    history.append({k: task.get(k) for k in ("status", "attempt", "actual_track", "source_switched", "source_retry")})
                    log("download_state", {"title": title, **history[-1]})
                    last = marker
                health_status, _, health_ms = api("GET", "/health/ready")
                controls.append(health_ms)
                require(health_status == 200, "health stays available during the real download")
                if task["status"] in ("completed", "failed", "canceled"):
                    break
                time.sleep(0.3)
            else:
                data("POST", "/api/v1/downloads/" + created["task_id"] + "/cancel", {})
                raise LiveFailure("live download exceeded its external test budget")
            sample["download"] = {k: task.get(k) for k in ("status", "attempt", "actual_track", "source_switched", "source_retry")}
            sample["download"].update(history=history, control_max_ms=max(controls))
            if task["status"] == "completed":
                file_id = task["result"]["file_id"]
                file = data("GET", "/api/v1/files/" + file_id)
                readback = media("file", file_id, f"{index + 1}-download.{file['container']}")
                local_files = [p for p in Path(service.downloads).rglob("*")
                               if p.is_file() and p.name == file["display_name"]]
                require(len(local_files) == 1, "published audio has one local file")
                with local_files[0].open("rb") as source:
                    local_hash = hashlib.file_digest(source, "sha256").hexdigest()
                require(readback["bytes"] == file["size_bytes"] and readback["sha256"] == local_hash,
                        "full media read matches the published local file size and SHA-256")
                sample["download"]["file"] = {k: file.get(k) for k in ("container", "size_bytes", "etag")}
                sample["download"]["file"]["sha256"] = local_hash
                sample["download"]["readback"] = readback
                sample["download"]["actual_identity"] = identity(detail(task["actual_track"]))
                completed.append((file_id, readback["sha256"]))
            else:
                sample["download"]["error"] = public_error({"error": task.get("error")})
            sample["statistics_after"] = stats()
            log("download_result", {"title": title, **sample["download"], "statistics": sample["statistics_after"]})

        report["statistics_before_restart"] = stats()
        service.restart()
        report["statistics_after_restart"] = stats()
        require(report["statistics_before_restart"] == report["statistics_after_restart"], "download statistics survive a service restart")
        for file_id, digest in completed:
            require(media("file", file_id)["sha256"] == digest, "published audio survives a service restart unchanged")
        report["verification"] = validate_report(report)
        report["status"] = "passed" if all(item["passed"] for item in report["verification"]) else "incomplete"
        log("finished", {"status": report["status"], "statistics": report["statistics_after_restart"]})
        return report["status"] == "passed"
    except Exception as exc:
        report["status"] = "failed"
        report["failure"] = str(exc) if isinstance(exc, LiveFailure) else type(exc).__name__
        log("failed", {"reason": report["failure"]})
        return False
    finally:
        report["finished_at"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        save()
        if service:
            work = Path(service.work).resolve()
            assert work.parent == Path(tempfile.gettempdir()).resolve() and work.name.startswith("music-service-e2e-")
            service.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    raise SystemExit(0 if run(args.binary, args.output_dir) else 1)
