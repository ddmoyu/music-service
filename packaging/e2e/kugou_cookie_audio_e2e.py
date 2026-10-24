"""酷狗 Cookie 取流验收：VIP 匿名／账号对照、试听限制、免费整曲及真实下载。"""
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
    cookie = cookie_file.read_text(encoding="utf-8-sig").strip()
    # Only log whitelisted evidence; never print request URLs, Cookie or upstream responses.
    secret_values = [cookie] + [part.strip().split("=", 1)[1] for part in cookie.split(";")
                               if part.strip().startswith("t=")]
    report = {"source": "kugou", "samples": [], "runs": []}
    service = OwnService(binary, 0)
    try:
        config = Path(service.config)
        settings = json.loads(config.read_text(encoding="utf-8"))
        settings["features"] = {"media_proxy": True}
        config.write_text(json.dumps(settings, ensure_ascii=False), encoding="utf-8")
        service.restart()

        def api(method, route, data=None, headers=None):
            parsed = urllib.parse.urlsplit(service.api)
            conn = http.client.HTTPConnection(parsed.hostname, parsed.port, timeout=35)
            try:
                request_headers = {"Authorization": "Bearer " + service.token, **(headers or {})}
                if data is not None:
                    request_headers["Content-Type"] = "application/json"
                conn.request(method, route, body=None if data is None else json.dumps(data).encode(), headers=request_headers)
                response = conn.getresponse()
                raw = response.read(2 * 1024 * 1024 + 1)
                assert len(raw) <= 2 * 1024 * 1024, "API response limit"
                assert all(value.encode() not in raw for value in secret_values), "secret in API response"
                return response.status, json.loads(raw)
            finally:
                conn.close()

        def ok(method, route, data=None):
            status, doc = api(method, route, data)
            assert status in (200, 201, 202), "API rejected: " + str((doc.get("error") or {}).get("code"))
            return doc["data"]

        def read_media(kind, identity, prefix=False):
            access = ok("POST", "/api/v1/media-access", {"target": {"kind": kind, "id": identity}, "mode": "proxy", "auth": "bearer"})
            url = urllib.parse.urlsplit(access["url"])
            assert url.hostname in ("127.0.0.1", "localhost", "::1")
            conn = http.client.HTTPConnection(url.hostname, url.port, timeout=35)
            try:
                headers = {"Authorization": "Bearer " + service.token}
                if prefix:
                    headers["Range"] = "bytes=0-65535"
                conn.request("GET", url.path + ("?" + url.query if url.query else ""), headers=headers)
                response = conn.getresponse()
                assert response.status == (206 if prefix else 200), "media read failed"
                total, digest = 0, hashlib.sha256()
                deadline = time.monotonic() + 120
                while chunk := response.read(65536):
                    total += len(chunk)
                    assert total <= (65536 if prefix else 64 * 1024 * 1024), "media byte limit"
                    assert time.monotonic() < deadline, "media time limit"
                    digest.update(chunk)
                assert total > 10000
                return {"bytes": total, "sha256": digest.hexdigest()}
            finally:
                conn.close()

        def download(track, allow_preview=False):
            status, doc = api("POST", "/api/v1/downloads", {
                "track": track, "quality": "standard", "allow_preview": allow_preview, "metadata_policy": "skip"},
                {"Idempotency-Key": "kg-cookie-" + uuid.uuid4().hex})
            assert status in (200, 202), "download acceptance failed"
            created = doc["data"]
            assert not created["reused"], "cross-account or preview reuse"
            deadline = time.monotonic() + 180
            while True:
                task = ok("GET", "/api/v1/downloads/" + created["task_id"])
                if task["status"] in ("completed", "failed", "canceled"):
                    break
                assert time.monotonic() < deadline, "download timeout"
                time.sleep(0.25)
            result = {"status": task["status"]}
            if task["status"] == "completed":
                identity = task["result"]["file_id"]
                file = ok("GET", "/api/v1/files/" + identity)
                result.update(read_media("file", identity))
                assert result["bytes"] == file["size_bytes"]
                result["completeness"] = task["result"]["processing"]["completeness"]
                result["duration_ms"] = task["result"]["processing"]["duration_ms"]
            else:
                result["error"] = (task.get("error") or {}).get("code")
                result["message"] = (task.get("error") or {}).get("message")
            return result

        for keyword, title in (("林俊杰 江南", "江南"), ("林俊杰 美人鱼", "美人鱼"), ("两只老虎", "两只老虎")):
            page = ok("GET", "/api/v1/search/tracks?" + urllib.parse.urlencode({"sources": "kugou", "q": keyword, "limit": 20}))
            artist = "贝瓦儿歌" if title == "两只老虎" else "林俊杰"
            song = next(x for x in page["items"] if x["title"] == title and any(a["name"] == artist for a in x["artists"]))
            vip = title != "两只老虎"
            assert not vip or song["pay"]["pay_play"] == 1, "VIP evidence missing"
            report["samples"].append({"track": song["ref"], "title": title, "pay": song["pay"], "vip": vip})

        for mode in ("anonymous", "cookie"):
            if mode == "cookie":
                created = ok("POST", "/api/v1/credentials", {"source": "kugou", "kind": "cookie",
                    "auth_profile": "web_cookie", "secret": {"cookie": cookie}})
                identity = created["credential_id"]
                selected = ok("GET", "/api/v1/credential-defaults/kugou")
                assert selected["credential_id"] == identity, "new enabled Cookie must be selected"
                report["login_status"] = created["login_status"]
                report["membership_status"] = created["membership"]["status"]
            for sample in report["samples"]:
                request = {"track": sample["track"], "quality": "standard"}
                status, doc = api("POST", "/api/v1/assets/resolve", request)
                record = {"mode": mode, "title": sample["title"]}
                report["runs"].append(record)
                if status == 200:
                    asset = doc["data"]
                    assert asset["completeness"] == "full"
                    record["full_resolve"] = "full"
                    record["playback"] = read_media("asset", asset["asset_id"], True)
                else:
                    record["full_resolve"] = doc["error"]["code"]
                    assert sample["vip"] and record["full_resolve"] in ("CREDENTIALS_REQUIRED", "PREVIEW_REJECTED")
                record["full_download"] = download(sample["track"])
                if record["full_resolve"] == "full":
                    assert record["full_download"]["status"] == "completed"
                    assert record["full_download"]["completeness"] == "full"
                else:
                    assert record["full_download"]["status"] == "failed"
                    assert record["full_download"]["error"] == record["full_resolve"]
                if record["full_resolve"] == "PREVIEW_REJECTED":
                    preview = ok("POST", "/api/v1/assets/resolve", {**request, "allow_preview": True})
                    assert preview["completeness"] == "preview"
                    record["preview_playback"] = read_media("asset", preview["asset_id"], True)
                    record["preview_download"] = download(sample["track"], True)
                    if record["preview_download"]["status"] == "completed":
                        assert record["preview_download"]["completeness"] == "preview"
                    else:
                        # An upstream preview may retain the full-track frame count. Preserve the
                        # strict validator's failure and report it, never call it playable full audio.
                        assert record["preview_download"]["error"] == "AUDIO_INVALID", "unexpected preview download failure"
                print(json.dumps(record, ensure_ascii=True), flush=True)
        ok("DELETE", "/api/v1/credential-defaults/kugou")
        assert ok("GET", "/api/v1/credential-defaults/kugou")["credential_id"] is None
        status, cleared = api("POST", "/api/v1/assets/resolve", {"track": report["samples"][0]["track"]})
        assert status != 200 and cleared["error"]["code"] == report["runs"][0]["full_resolve"]
        report["cleared_default_returns_to_anonymous"] = True
        for name in ("stdout.txt", "stderr.txt"):
            with (Path(service.work) / name).open("r", encoding="utf-8") as stream:
                for line in stream:
                    assert all(value not in line for value in secret_values), "secret in logs"
        report["api_and_logs_secret_free"] = True
        return report
    finally:
        report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        work = Path(service.work).resolve()
        assert work.parent == Path(tempfile.gettempdir()).resolve() and work.name.startswith("music-service-e2e-")
        service.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--cookie-file", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    run(args.binary, args.cookie_file, args.report)
