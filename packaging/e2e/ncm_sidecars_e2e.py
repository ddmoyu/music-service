#!/usr/bin/env python3
"""Independent CLI/HTTP acceptance for NCM import and optional download sidecars.

Only the --online section contacts music platforms. Uses synthetic NCM/FLAC/MP3
fixtures and the Python standard library; no decryption code is reused here.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sqlite3
import subprocess
import uuid

from kuwo_e2e import OwnService, Http, check, get_url, ticket_for, wait_task


def digest(data):
    return hashlib.sha256(data).hexdigest()


def configure(service, **output):
    path = Path(service.config)
    config = json.loads(path.read_text(encoding="utf-8"))
    config["downloads"] = {"output": output}
    path.write_text(json.dumps(config, ensure_ascii=False), encoding="utf-8")


def stop(service):
    service.process.terminate()
    service.process.wait(timeout=30)


def command(service, input_path, expect=0):
    result = subprocess.run([service.binary, "--config", service.config, "import-ncm", "--input", str(input_path),
        "--principal", "prn_local", "--destination", "dst_default", "--metadata-policy", "skip"],
        capture_output=True, encoding="utf-8", timeout=40)
    check(result.returncode == expect, "NCM command exit: " + result.stderr)
    return json.loads(result.stdout) if result.returncode == 0 else result


def read_parts(service, file_id, expected):
    api = Http(service.api, service.token)
    url = ticket_for(api, file_id)
    for kind, value in expected.items():
        status, headers, data = get_url(url + ("&" if "?" in url else "?") + "part=" + kind)
        check(status == 200 and data == value, kind + " bytes match independently known input")
        if kind == "lyrics":
            check("text/plain" in headers.get("Content-Type", ""), "lyrics MIME")
        if kind == "cover":
            check("image/png" in headers.get("Content-Type", ""), "cover MIME")


def local(service, report):
    fixtures = Path(__file__).resolve().parents[2] / "tests/fixtures/audio"
    locked = command(service, fixtures / "synthetic-mp3.ncm", expect=1)
    check("INSTANCE_LOCKED" in locked.stderr, "live server prevents offline import")
    stop(service)
    configure(service, export_lyrics=True, export_cover=True, sidecars_required=True,
              embed_lyrics=False, embed_cover=False)
    results = []
    expected_cover = (fixtures / "cover.png").read_bytes()
    expected_lyrics = b"[00:00.00]Synthetic lyric\n"
    for fmt, plain in [("mp3", "ncm-plain.mp3"), ("flac", "sine-3s.flac")]:
        source = fixtures / ("synthetic-" + fmt + ".ncm")
        before = digest(source.read_bytes())
        outcome = command(service, source)
        audio = (fixtures / plain).read_bytes()
        path = Path(service.downloads) / outcome["relative_path"]
        check(path.read_bytes() == audio, fmt + " payload is unchanged")
        check(path.with_suffix(".lrc").read_bytes() == expected_lyrics, "same-basename lyrics")
        check(path.with_suffix(".png").read_bytes() == expected_cover, "same-basename cover")
        check(digest(source.read_bytes()) == before, "NCM source remains unchanged")
        repeated = command(service, source)
        check(repeated["file_id"] == outcome["file_id"], "intact NCM import is reused")
        results.append((source, outcome, {"audio": audio, "lyrics": expected_lyrics, "cover": expected_cover}))
    damaged = Path(service.work) / "broken.ncm"
    damaged.write_bytes((fixtures / "synthetic-mp3.ncm").read_bytes()[:-1])
    bad = command(service, damaged, expect=1)
    check("AUDIO_INVALID" in bad.stderr, "truncated NCM cannot complete")
    service.start()
    for _, outcome, expected in results:
        read_parts(service, outcome["file_id"], expected)
    service.restart()
    for _, outcome, expected in results:
        read_parts(service, outcome["file_id"], expected)
    source, outcome, expected = results[0]
    lyric = (Path(service.downloads) / outcome["relative_path"]).with_suffix(".lrc")
    lyric.write_bytes(b"X" * len(expected_lyrics))
    api = Http(service.api, service.token)
    status, _, body = api.call("GET", "/api/v1/files/" + outcome["file_id"])
    check(status == 410 and json.loads(body)["error"]["code"] == "FILE_MISSING", "same-size sidecar corruption invalidates product")
    stop(service)
    replaced = command(service, source)
    check(replaced["file_id"] != outcome["file_id"], "corrupt import is not reused")
    check(lyric.read_bytes() == b"X" * len(expected_lyrics), "new import never overwrites damaged existing files")
    # Simulate a command disappearing before publication. Only local imports are failed;
    # they must never be sent through a network provider on restart.
    database = Path(service.work) / "data/db/music-service.sqlite3"
    candidates = list((Path(service.work) / "data").rglob("*.sqlite3"))
    check(len(candidates) == 1, "one service database")
    database = candidates[0]
    with sqlite3.connect(database) as db:
        db.row_factory = sqlite3.Row
        row = dict(db.execute("SELECT * FROM download_tasks WHERE id=?", (replaced["task_id"],)).fetchone())
        row.update(id="tsk_interrupted", status="processing", execution_id="exe_interrupted", version=1,
                   result=None, error=None, progress=None, finished_at=None)
        columns = ",".join(row)
        db.execute("INSERT INTO download_tasks (" + columns + ") VALUES (" + ",".join("?" for _ in row) + ")", list(row.values()))
        db.commit()
    service.start()
    api = Http(service.api, service.token)
    interrupted = api.json("GET", "/api/v1/downloads/tsk_interrupted")[2]
    check(interrupted["status"] == "failed" and interrupted["error"]["code"] == "LOCAL_IMPORT_INTERRUPTED",
          "interrupted local imports fail explicitly without a network retry")
    report["local"] = {"containers": ["mp3", "flac"], "payload_hashes": [digest(item[2]["audio"]) for item in results],
        "source_unchanged": True, "reused": True, "restart_readback": True, "sidecar_corruption_detected": True,
        "interrupted_import": "failed_without_network_retry"}


def online(service, report):
    configure(service, export_lyrics=True, export_cover=True, embed_lyrics=False, embed_cover=False)
    service.restart()
    api = Http(service.api, service.token)
    report["online"] = []
    products = []
    for source, track in [("netease", "5257138"), ("kuwo", "228908"), ("fivesing", "fc-16394286")]:
        request = {"track": {"source": source, "id": track}, "metadata_policy": "skip"}
        created = api.json("POST", "/api/v1/downloads", request,
            {"Idempotency-Key": "sidecars-" + uuid.uuid4().hex}, expect=(200, 202))[2]
        task = wait_task(api, created["task_id"], ("completed", "failed"), timeout=240)
        check(task["status"] == "completed", source + " download: " + str(task.get("error")))
        file_id = task["result"]["file_id"]
        asset = api.json("GET", "/api/v1/files/" + file_id)[2]
        check(task["result"]["metadata_status"] == "skipped", "tag writing remains independently disabled")
        url = ticket_for(api, file_id)
        parts = {}
        for part in ["audio", "lyrics", "cover"]:
            status, _, data = get_url(url + ("&" if "?" in url else "?") + "part=" + part)
            if status == 200:
                check(bool(data), part + " has bytes")
                parts[part] = digest(data)
            else:
                check(part != "audio" and status == 404, "unavailable optional sidecar has no fake file")
                check(task["result"]["metadata_fields"][part + "_file"] == "unavailable", "unavailable sidecar is explicit")
        check("lyrics" in parts, source + " real lyrics exported even with metadata_policy skip")
        if source == "netease":
            check("cover" in parts, "NetEase real cover exported")
        repeated = api.json("POST", "/api/v1/downloads", request,
            {"Idempotency-Key": "sidecars-repeat-" + uuid.uuid4().hex}, expect=(200, 202))[2]
        check(repeated["task_id"] == task["task_id"], "complete bundle is reused")
        products.append((file_id, parts))
        report["online"].append({"source": source, "track": track, "parts": parts, "metadata_fields": task["result"]["metadata_fields"]})
    service.restart()
    api = Http(service.api, service.token)
    for file_id, parts in products:
        url = ticket_for(api, file_id)
        for part, expected in parts.items():
            status, _, data = get_url(url + ("&" if "?" in url else "?") + "part=" + part)
            check(status == 200 and digest(data) == expected, "real bundle survives restart")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--port", type=int, default=18902)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--online", action="store_true")
    args = parser.parse_args()
    service = OwnService(args.binary, args.port)
    report = {"status": "incomplete"}
    try:
        local(service, report)
        if args.online:
            online(service, report)
        report["status"] = "passed"
    finally:
        service.close()
        args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
