"""Cookie 自动校验在线验收；只输出脱敏会员结果，使用独立临时数据目录。"""
import argparse
import datetime
import json
import pathlib
import tempfile
import time

from kuwo_e2e import OwnService, Http, check


def run(binary, secret_dir):
    service = OwnService(binary, 0)
    report = {"platforms": {}, "invalid": {}, "unsupported": None, "restart": False}
    try:
        api = Http(service.api, service.token)
        saved = []
        for source in ("netease", "qq", "kuwo"):
            path = secret_dir / f"{source}-cookie.txt"
            if not path.is_file():
                report["platforms"][source] = {"tested": False, "reason": "no_local_cookie"}
                continue
            cookie = path.read_text(encoding="utf-8-sig").strip()
            before = time.monotonic()
            created = api.json("POST", "/api/v1/credentials", {
                "source": source, "kind": "cookie", "auth_profile": "web_cookie",
                "secret": {"cookie": cookie}}, expect=(201,))[2]
            check(created["verification"]["id"] is not None, source + " save automatically creates a check")
            result = api.wait_credential(created["credential_id"])
            check(result["login_status"] == "valid", source + " session is valid")
            check(result["membership"]["status"] in ("none", "active", "expired"), source + " membership is known")
            check(result["membership"]["checked_at"] is not None, source + " membership has a check timestamp")
            membership = result["membership"]
            precision = membership["expiry_precision"]
            if precision == "date":
                check(membership["expires_at"] is None and membership["expires_on"] ==
                      datetime.date.fromisoformat(membership["expires_on"]).isoformat(), source + " preserves date precision")
            elif precision == "datetime":
                check(membership["expires_on"] is None and membership["expires_at"].endswith("Z"),
                      source + " preserves UTC timestamp precision")
            else:
                check(precision == "unknown" and membership["expires_at"] is None and membership["expires_on"] is None,
                      source + " unknown expiry has no invented date")
            check(cookie not in json.dumps(result), source + " response contains no Cookie")
            report["platforms"][source] = {"tested": True, "login_status": result["login_status"],
                "membership": result["membership"], "elapsed_seconds": round(time.monotonic() - before, 3)}
            saved.append((created["credential_id"], result["membership"]))
        # 下方无效 Cookie 测试会自动选中同平台的新账号并清空旧账号证据，先验证重启回读。
        service.restart()
        api = Http(service.api, service.token)
        for credential_id, membership in saved:
            result = api.json("GET", f"/api/v1/credentials/{credential_id}")[2]
            check(result["membership"] == membership, "membership survives restart exactly")
        report["restart"] = True
        for source in ("netease", "qq", "kuwo", "bilibili"):
            created = api.json("POST", "/api/v1/credentials", {
                "source": source, "kind": "cookie", "auth_profile": "web_cookie",
                "secret": {"cookie": "missing_session=synthetic"}}, expect=(201,))[2]
            result = api.wait_credential(created["credential_id"])
            check(result["login_status"] == "invalid", source + " missing login is invalid")
            check(result["membership"]["status"] == "unknown", source + " invalid login does not imply nonmember")
            report["invalid"][source] = result["login_status"]
        created = api.json("POST", "/api/v1/credentials", {
            "source": "migu", "kind": "cookie", "auth_profile": "web_cookie",
            "secret": {"cookie": "session=synthetic"}}, expect=(201,))[2]
        result = api.wait_credential(created["credential_id"])
        check(result["login_status"] == "unknown", "unsupported verification is unknown")
        check(result["membership"]["status"] == "unknown", "unsupported membership is unknown")
        report["unsupported"] = result["verification"]["error_code"]
        return report
    finally:
        # close 只删除本脚本创建的明确临时目录，不触及用户的服务数据。
        work = pathlib.Path(service.work).resolve()
        assert work.parent == pathlib.Path(tempfile.gettempdir()).resolve() and work.name.startswith("music-service-e2e-")
        service.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--secret-dir", type=pathlib.Path, required=True)
    parser.add_argument("--report", type=pathlib.Path, required=True)
    args = parser.parse_args()
    result = run(args.binary, args.secret_dir)
    args.report.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(result, ensure_ascii=True))
