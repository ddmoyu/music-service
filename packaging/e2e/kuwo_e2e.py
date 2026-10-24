#!/usr/bin/env python3
"""The S1 Kuwo loop (T20) against the live service, without a frontend; --source fivesing
runs the same loop on 5sing (T22).

search -> detail -> resolve -> durable task (the creating connection closed) -> SSE and
HTTP -> file metadata -> media ticket with a range and a full read compared with the
published file -> a client-side save that fails half-way while the task stays completed
(A29) -> a cancel racing the transfer (either result must be consistent) -> a restart
with a new instance that still serves the task and file, replays the idempotency key
and reuses the completed result.

Two modes:
  --binary PATH           init, serve and restart a music-service of its own (Windows,
                          Linux, macOS) in a temporary directory.
  --api URL --token T     drive a running service (the container); --restart-command
                          restarts it, --downloads-dir is where its destination root is
                          visible to compare the published bytes.

Standard library only. Exit code 0 when every check passed; the report goes to stdout.
"""

from __future__ import annotations

import argparse
import hashlib
import http.client
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import uuid

KEYWORDS = {"kuwo": "晴天", "fivesing": "千年", "netease": "屋顶"}
TRACKS = {"kuwo": "78932517", "fivesing": "fc-16394286", "netease": "5257138"}


class Failure(Exception):
    pass


def check(condition: bool, message: str) -> None:
    if not condition:
        raise Failure(message)
    print(f"  ok  {message}")


class Http:
    def __init__(self, base: str, token: str | None):
        parsed = urllib.parse.urlsplit(base)
        self.host = parsed.hostname
        self.port = parsed.port or 80
        self.token = token

    def call(self, method, path, body=None, headers=None, timeout=30):
        conn = http.client.HTTPConnection(self.host, self.port, timeout=timeout)
        all_headers = dict(headers or {})
        if self.token:
            all_headers["Authorization"] = f"Bearer {self.token}"
        payload = None
        if body is not None:
            payload = json.dumps(body).encode()
            all_headers["Content-Type"] = "application/json"
        conn.request(method, path, body=payload, headers=all_headers)
        response = conn.getresponse()
        data = response.read()
        conn.close()
        return response.status, dict(response.getheaders()), data

    def json(self, method, path, body=None, headers=None, expect=(200,)):
        status, response_headers, data = self.call(method, path, body, headers)
        if status not in expect:
            raise Failure(f"{method} {path}: HTTP {status} {data[:400]!r}")
        return status, response_headers, json.loads(data)["data"]

    def wait_credential(self, credential_id, timeout=20):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            credential = self.json("GET", f"/api/v1/credentials/{credential_id}")[2]
            if credential["verification"]["state"] != "pending":
                return credential
            time.sleep(0.05)
        raise Failure("credential verification did not finish within its deadline")


class Sse:
    """One SSE stream read on its own thread; frames end at a blank line."""

    def __init__(self, base: str, token: str):
        parsed = urllib.parse.urlsplit(base)
        self.conn = http.client.HTTPConnection(parsed.hostname, parsed.port, timeout=60)
        self.conn.request("GET", "/events/v1/downloads", headers={"Authorization": f"Bearer {token}",
                                                                   "Accept": "text/event-stream"})
        self.sock = self.conn.sock  # http.client lets go of it for a response without a length
        self.response = self.conn.getresponse()
        if self.response.status != 200:
            raise Failure(f"SSE: HTTP {self.response.status}")
        self.events: list[tuple[str, dict]] = []
        self.lock = threading.Condition()
        self.closed = False
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        event, data = "", ""
        try:
            while True:
                line = self.response.readline()
                if not line:
                    break
                line = line.decode().rstrip("\r\n")
                if line == "":
                    if event:
                        with self.lock:
                            self.events.append((event, json.loads(data) if data else {}))
                            self.lock.notify_all()
                    event, data = "", ""
                elif line.startswith("event: "):
                    event = line[7:]
                elif line.startswith("data: "):
                    data += line[6:]
        except OSError:
            pass
        with self.lock:
            self.closed = True
            self.lock.notify_all()

    def wait(self, predicate, timeout=60):
        deadline = time.monotonic() + timeout
        with self.lock:
            while True:
                for item in self.events:
                    if predicate(*item):
                        return item
                remaining = deadline - time.monotonic()
                if remaining <= 0 or self.closed:
                    return None
                self.lock.wait(remaining)

    def close(self):
        try:
            self.sock.shutdown(2)
        except OSError:
            pass
        self.conn.close()


class OwnService:
    """A music-service started by this script in a temporary directory."""

    def __init__(self, binary: str, port: int):
        self.binary = os.path.abspath(binary)
        self.work = tempfile.mkdtemp(prefix="music-service-e2e-")
        self.downloads = os.path.join(self.work, "下载")
        os.makedirs(self.downloads)
        self.config = os.path.join(self.work, "config.json")
        with open(self.config, "w", encoding="utf-8") as f:
            json.dump({"config_version": 1,
                       "data_dir": os.path.join(self.work, "data"),
                       "control": {"port": port},
                       "media": {"port": port + 1 if port else 0},
                       "events": {"port": port + 2 if port else 0},
                       "destinations": [{"id": "dst_default", "name": "downloads", "root": self.downloads,
                                         "principals": ["prn_local"]}]}, f, ensure_ascii=False)
        subprocess.run([self.binary, "--config", self.config, "init"], check=True, stdout=subprocess.DEVNULL)
        with open(os.path.join(self.work, "data", "secrets", "bootstrap-token.txt"), encoding="utf-8") as f:
            self.token = f.readline().strip()
        self.process = None
        self.api = None
        self.start()

    def start(self):
        self.stdout = open(os.path.join(self.work, "stdout.txt"), "a+", encoding="utf-8")
        self.stderr = open(os.path.join(self.work, "stderr.txt"), "a+", encoding="utf-8")
        offset = self.stdout.tell()
        self.process = subprocess.Popen([self.binary, "--config", self.config, "serve"],
                                        stdout=self.stdout, stderr=self.stderr)
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            self.stdout.seek(offset)
            for line in self.stdout.read().splitlines():
                if '"event":"ready"' in line:
                    self.api = json.loads(line)["api_base_url"]
                    return
            if self.process.poll() is not None:
                break
            time.sleep(0.1)
        raise Failure("the service did not become ready")

    def restart(self):
        # Windows has no SIGTERM for a console child; terminate() is a hard stop there and
        # SIGTERM elsewhere. Either way the next start recovers from the database.
        self.process.terminate()
        self.process.wait(timeout=30)
        self.start()

    def close(self):
        if self.process and self.process.poll() is None:
            self.process.terminate()
            self.process.wait(timeout=30)
        for f in (self.stdout, self.stderr):
            f.close()
        if os.environ.get("MUSIC_SERVICE_E2E_KEEP") != "1":
            shutil.rmtree(self.work, ignore_errors=True)


class AttachedService:
    def __init__(self, api: str, token: str, restart_command: str | None, downloads: str | None):
        self.api, self.token, self.restart_command, self.downloads = api, token, restart_command, downloads

    def restart(self):
        subprocess.run(self.restart_command, shell=True, check=True)
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            try:
                if Http(self.api, None).call("GET", "/health/ready", timeout=2)[0] == 200:
                    return
            except OSError:
                pass
            time.sleep(0.5)
        raise Failure("the service did not come back")

    def close(self):
        pass


def wait_task(api: Http, task_id: str, statuses, timeout=120):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        task = api.json("GET", f"/api/v1/downloads/{task_id}")[2]
        if task["status"] in statuses:
            return task
        time.sleep(0.5)
    raise Failure(f"task {task_id} stayed {task['status']}")


def ticket_for(api: Http, file_id: str) -> str:
    access = api.json("POST", "/api/v1/media-access", {"target": {"kind": "file", "id": file_id}})[2]
    check(access["auth"] == "url" and access["range_supported"], "media-access gives a ticket URL with ranges")
    return access["url"]


def get_url(url: str, headers=None, stop_after=None):
    parsed = urllib.parse.urlsplit(url)
    conn = http.client.HTTPConnection(parsed.hostname, parsed.port, timeout=60)
    conn.request("GET", parsed.path + (("?" + parsed.query) if parsed.query else ""), headers=headers or {})
    sock = conn.sock
    response = conn.getresponse()
    if stop_after is None:
        data = response.read()
    else:
        data = response.read(stop_after)
        sock.shutdown(2)  # the client's save fails: it drops the connection
    conn.close()
    return response.status, dict(response.getheaders()), data


def run(service, source: str, track_id: str) -> dict:
    report: dict = {"source": source, "track": track_id}
    keyword = KEYWORDS[source]
    track = {"source": source, "id": track_id}
    api = Http(service.api, service.token)
    info = api.json("GET", "/api/v1/info")[2]
    check(info["features"]["downloads"] and info["features"]["file_access"] and info["features"]["progress_sse"],
          "info declares downloads, file_access and progress_sse")
    report["instance_1"] = info["instance_id"]

    stream = Sse(info["events_base_url"], service.token)
    check(stream.wait(lambda e, d: e == "stream.ready", 10) is not None, "SSE starts with stream.ready")

    search = api.json("GET", "/api/v1/search/tracks?" + urllib.parse.urlencode(
        {"q": keyword, "sources": source, "limit": 10}))[2]
    hits = [t for t in search["items"] if t["ref"]["source"] == source]
    check(len(hits) > 0, f"search '{keyword}' returns {len(hits)} {source} tracks")
    # The song page is refused from some regions; the download names the file by source-id then.
    status, _, data = api.call("GET", f"/api/v1/tracks/detail?source={source}&id={track_id}")
    if status == 200:
        detail = json.loads(data)["data"]
        check(bool(detail["title"]), f"detail of {track_id}: {detail['title']}")
    else:
        report["detail_unavailable"] = json.loads(data)["error"]
        print(f"  --  detail of {track_id} unavailable here: HTTP {status} {report['detail_unavailable']['code']}")
    asset = api.json("POST", "/api/v1/assets/resolve", {"track": track})[2]
    check(asset["actual_quality"] == "standard" and asset["completeness"] == "full",
          f"resolve: standard, full, {asset['container']} {asset['bitrate_bps']} bit/s")
    report["asset"] = {k: asset[k] for k in ("container", "bitrate_bps", "completeness", "actual_quality")}

    key = "e2e-" + uuid.uuid4().hex
    status, _, created = api.json("POST", "/api/v1/downloads", {"track": track},
                                  {"Idempotency-Key": key}, expect=(200, 202))
    task_id = created["task_id"]
    report["create_status"] = status
    check(status == 202 or created["reused"], f"download accepted ({status}) as {task_id}")

    done = stream.wait(lambda e, d: e == "task.updated" and d.get("task_id") == task_id
                       and d.get("status") in ("completed", "failed"), 180)
    check(done is not None, "SSE reports the end of the task")
    updates = [d for e, d in stream.events if e == "task.updated" and d.get("task_id") == task_id]
    versions = [d["version"] for d in updates]
    check(versions == sorted(versions), f"SSE versions only move forward: {[d['status'] for d in updates]}")
    task = wait_task(api, task_id, ("completed", "failed"))
    check(task["status"] == "completed", f"task completed (error: {task['error']})")
    report["task"] = {k: task[k] for k in ("attempt", "actual_quality", "warnings")}
    file_id = task["result"]["file_id"]
    file = api.json("GET", f"/api/v1/files/{file_id}")[2]
    report["file"] = {k: file[k] for k in ("display_name", "size_bytes", "container", "content_type", "etag")}
    check(file["size_bytes"] > 100_000, f"file {file['display_name']}: {file['size_bytes']} bytes")

    ticket = ticket_for(api, file_id)
    status, headers, data = get_url(ticket, {"Range": "bytes=0-15"})
    check(status == 206 and headers.get("Content-Range") == f"bytes 0-15/{file['size_bytes']}",
          "a ticket range read is 206 with the right Content-Range")
    status, _, whole = get_url(ticket)
    check(status == 200 and len(whole) == file["size_bytes"], "a full ticket read returns every byte")
    digest = hashlib.sha256(whole).hexdigest()
    report["sha256"] = digest
    if service.downloads:
        on_disk = os.path.join(service.downloads, file["display_name"])
        with open(on_disk, "rb") as f:
            check(hashlib.sha256(f.read()).hexdigest() == digest, "the read equals the published file")

    # A29: the client's own save fails; the backend task is not affected.
    get_url(ticket, stop_after=64 * 1024)
    check(api.json("GET", f"/api/v1/downloads/{task_id}")[2]["status"] == "completed",
          "a failed client save leaves the task completed")

    # A cancel racing a forced second download: canceled or completed, never both.
    status, _, forced = api.json("POST", "/api/v1/downloads",
                                 {"track": track, "duplicate_policy": "force"},
                                 {"Idempotency-Key": "e2e-" + uuid.uuid4().hex}, expect=(202,))
    forced_id = forced["task_id"]
    stream.wait(lambda e, d: e == "task.updated" and d.get("task_id") == forced_id
                and d.get("status") in ("downloading", "processing", "completed"), 60)
    status, _, data = api.call("POST", f"/api/v1/downloads/{forced_id}/cancel")
    end = wait_task(api, forced_id, ("canceled", "completed", "failed"))
    consistent = (status in (200, 202) and end["status"] == "canceled" and end["result"] is None) or \
                 (status == 409 and end["status"] == "completed")
    check(consistent, f"cancel answered {status}, task ended {end['status']}")
    report["cancel"] = {"http": status, "final": end["status"]}
    stream.close()

    # Restart: a new instance with the same durable task and file.
    service.restart()
    api = Http(service.api, service.token)
    info = api.json("GET", "/api/v1/info")[2]
    check(info["instance_id"] != report["instance_1"], "the restart is a new instance")
    after = api.json("GET", f"/api/v1/downloads/{task_id}")[2]
    check(after["status"] == "completed" and after["result"]["file_id"] == file_id, "the task survived the restart")
    status, _, data = get_url(ticket)
    check(status == 404, "tickets do not survive a restart")
    status, _, whole = get_url(ticket_for(api, file_id))
    check(status == 200 and hashlib.sha256(whole).hexdigest() == digest, "the file is served after the restart")
    status, headers, replay = api.json("POST", "/api/v1/downloads", {"track": track},
                                       {"Idempotency-Key": key}, expect=(200, 202))
    check(replay["task_id"] == task_id and headers.get("Idempotency-Replayed") == "true",
          "the first key replays the first answer")
    status, _, reused = api.json("POST", "/api/v1/downloads", {"track": track},
                                 {"Idempotency-Key": "e2e-" + uuid.uuid4().hex}, expect=(200, 202))
    check(status == 200 and reused["reused"], "a new key reuses the completed file")
    return report


def main() -> int:
    # -I ignores PYTHONIOENCODING; a Windows console would fail on the Chinese titles.
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary")
    parser.add_argument("--port", type=int, default=17652, help="control port; media and events use the next two")
    parser.add_argument("--api")
    parser.add_argument("--token")
    parser.add_argument("--restart-command")
    parser.add_argument("--downloads-dir")
    parser.add_argument("--source", choices=sorted(TRACKS), default="kuwo")
    parser.add_argument("--track", help="a free track with a full standard file (default per source)")
    args = parser.parse_args()
    if args.binary:
        service = OwnService(args.binary, args.port)
    elif args.api and args.token and args.restart_command:
        service = AttachedService(args.api, args.token, args.restart_command, args.downloads_dir)
    else:
        parser.error("give --binary, or --api, --token and --restart-command")
    started = time.time()
    try:
        report = run(service, args.source, args.track or TRACKS[args.source])
    except Failure as failure:
        print(f"FAILED: {failure}")
        return 1
    finally:
        service.close()
    report["seconds"] = round(time.time() - started, 1)
    print(json.dumps(report, ensure_ascii=False, indent=2))
    print(f"{args.source} e2e passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
