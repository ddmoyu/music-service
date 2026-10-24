"""酷我 Cookie 取流验收：独立临时服务，VIP 歌曲匿名／会员播放与完整下载。"""
import argparse
import hashlib
import http.client
import json
from pathlib import Path
import tempfile
import time
import urllib.parse
import uuid

from kuwo_e2e import OwnService


def run(binary, cookie_file, report_path):
    report = {"source": "kuwo", "membership": None, "samples": [], "runs": []}
    service = OwnService(binary, 0)
    try:
        config = Path(service.config)
        settings = json.loads(config.read_text(encoding="utf-8"))
        settings["features"] = {"media_proxy": True}
        config.write_text(json.dumps(settings, ensure_ascii=False), encoding="utf-8")
        service.restart()

        def api(method, route, data=None, expected=(200,), headers=None):
            parsed = urllib.parse.urlsplit(service.api)
            conn = http.client.HTTPConnection(parsed.hostname, parsed.port, timeout=35)
            request_headers = {"Authorization": "Bearer " + service.token, **(headers or {})}
            if data is not None:
                request_headers["Content-Type"] = "application/json"
            try:
                conn.request(method, route, body=None if data is None else json.dumps(data).encode(), headers=request_headers)
                response = conn.getresponse()
                raw = response.read(2 * 1024 * 1024 + 1)
                assert len(raw) <= 2 * 1024 * 1024, "API response limit"
                doc = json.loads(raw)
                if response.status not in expected:
                    raise RuntimeError(f"HTTP {response.status}: {(doc.get('error') or {}).get('code', 'unknown')}")
                return doc["data"]
            finally:
                conn.close()

        def read_media(kind, identity, prefix_only=False):
            access = api("POST", "/api/v1/media-access", {
                "target": {"kind": kind, "id": identity}, "mode": "proxy", "auth": "bearer"})
            url = urllib.parse.urlsplit(access["url"])
            assert url.hostname in ("127.0.0.1", "localhost", "::1"), "local proxy expected"
            conn = http.client.HTTPConnection(url.hostname, url.port, timeout=35)
            headers = {"Authorization": "Bearer " + service.token}
            if prefix_only:
                headers["Range"] = "bytes=0-65535"
            try:
                conn.request("GET", url.path + ("?" + url.query if url.query else ""), headers=headers)
                response = conn.getresponse()
                assert response.status == (206 if prefix_only else 200), "media response rejected"
                total, digest, prefix = 0, hashlib.sha256(), b""
                maximum = 65536 if prefix_only else 32 * 1024 * 1024
                deadline = time.monotonic() + 120
                while chunk := response.read(64 * 1024):
                    total += len(chunk)
                    assert total <= maximum and time.monotonic() < deadline, "media budget exceeded"
                    digest.update(chunk)
                    if not prefix:
                        prefix = chunk[:4]
                assert total > 1000, "empty or truncated audio"
                assert prefix.startswith(b"ID3") or prefix.startswith(b"fLaC") or (
                    len(prefix) >= 2 and prefix[0] == 0xFF and prefix[1] & 0xE0 == 0xE0), "unexpected audio signature"
                return {"http_status": response.status, "bytes": total, "sha256": digest.hexdigest()}
            finally:
                conn.close()

        provider = next(p for p in api("GET", "/api/v1/providers")["items"] if p["source"] == "kuwo")
        capability = provider["capabilities"]["resolve_audio"]
        assert capability["credentials"] == "optional"
        assert capability["requirements"]["auth_profiles"] == ["web_cookie"]
        report["implementation_version"] = provider["implementation"]["version"]
        query = urllib.parse.urlencode({"sources": "kuwo", "q": "林俊杰", "limit": 5})
        songs = api("GET", "/api/v1/search/tracks?" + query)["items"]
        for rid in ("93157", "93151"):
            song = next(x for x in songs if x["ref"]["id"] == rid)
            assert song["pay"]["pay_play"] == 1 and song["pay"]["pay_month"] == 1, "VIP evidence missing"
            report["samples"].append({"id": rid, "title": song["title"], "pay": song["pay"]})

        for mode in ("anonymous", "cookie"):
            if mode == "cookie":
                created = api("POST", "/api/v1/credentials", {
                    "source": "kuwo", "kind": "cookie", "auth_profile": "web_cookie",
                    "secret": {"cookie": cookie_file.read_text(encoding="utf-8-sig").strip()}}, expected=(201,))
                credential_id = created["credential_id"]
                deadline = time.monotonic() + 25
                while True:
                    credential = api("GET", "/api/v1/credentials/" + credential_id)
                    if credential["verification"]["state"] == "complete":
                        break
                    assert time.monotonic() < deadline, "credential verification timed out"
                    time.sleep(0.2)
                assert credential["login_status"] == "valid", "Cookie is not a verified login"
                assert credential["membership"]["status"] == "active", "active VIP required for this probe"
                report["membership"] = {k: credential["membership"][k] for k in ("status", "type", "name")}
                api("PUT", "/api/v1/credential-defaults/kuwo", {"credential_id": credential_id})
                print(json.dumps({"login": "valid", "membership": report["membership"]}, ensure_ascii=True), flush=True)

            for song in report["samples"]:
                track = {"source": "kuwo", "id": song["id"]}
                asset = api("POST", "/api/v1/assets/resolve", {"track": track, "quality": "standard"})
                assert asset["actual_quality"] == "standard" and asset["completeness"] == "full"
                playback = read_media("asset", asset["asset_id"], prefix_only=True)
                created = api("POST", "/api/v1/downloads", {
                    "track": track, "quality": "standard", "metadata_policy": "skip"}, expected=(200, 202),
                    headers={"Idempotency-Key": "kuwo-cookie-audio-" + uuid.uuid4().hex})
                deadline = time.monotonic() + 180
                while True:
                    task = api("GET", "/api/v1/downloads/" + created["task_id"])
                    if task["status"] in ("completed", "failed", "canceled"):
                        break
                    assert time.monotonic() < deadline, "download timed out"
                    time.sleep(0.3)
                assert task["status"] == "completed", "download failed: " + str((task.get("error") or {}).get("code"))
                file_id = task["result"]["file_id"]
                file = api("GET", "/api/v1/files/" + file_id)
                downloaded = read_media("file", file_id)
                assert downloaded["bytes"] == file["size_bytes"]
                record = {"mode": mode, "id": song["id"], "title": song["title"],
                          "asset": {k: asset[k] for k in ("actual_quality", "container", "completeness")},
                          "playback": playback, "download": downloaded, "reused": created["reused"]}
                assert not created["reused"], "accounts must not reuse each other's download"
                report["runs"].append(record)
                print(json.dumps(record, ensure_ascii=True), flush=True)
        api("DELETE", "/api/v1/credential-defaults/kuwo")
        assert api("GET", "/api/v1/credential-defaults/kuwo")["credential_id"] is None
        cleared = api("POST", "/api/v1/assets/resolve", {"track": {"source": "kuwo", "id": report["samples"][0]["id"]}})
        report["cleared_default_resolves_anonymously"] = cleared["completeness"] == "full"
        return report
    finally:
        report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        work = Path(service.work).resolve()
        assert work.parent == Path(tempfile.gettempdir()).resolve() and work.name.startswith("music-service-e2e-")
        service.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--cookie-file", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    run(args.binary, args.cookie_file, args.report)
