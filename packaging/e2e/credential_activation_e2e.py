"""账号启停进程验收：合成 Cookie、独立临时目录，不需要真实平台账号。"""
import argparse
import json
import pathlib
import sqlite3
import tempfile

from kuwo_e2e import OwnService, Http, check


def run(binary):
    service = OwnService(binary, 0)
    work = pathlib.Path(service.work).resolve()
    check(work.parent == pathlib.Path(tempfile.gettempdir()).resolve()
          and work.name.startswith("music-service-e2e-"), "isolated temporary data directory")
    try:
        api = Http(service.api, service.token)
        secret_dir = work / "data" / "secrets" / "credentials"

        def secrets():
            return len(list(secret_dir.glob("*.secret")))

        def get(account):
            return api.json("GET", f"/api/v1/credentials/{account}")[2]

        def selected():
            return api.json("GET", "/api/v1/credential-defaults/qq")[2]["credential_id"]

        first = api.json("POST", "/api/v1/credentials", {
            "source": "qq", "kind": "cookie", "auth_profile": "web_cookie", "label": "first",
            "secret": {"cookie": "missing_session=synthetic_first"}}, expect=(201,))[2]["credential_id"]
        second = api.json("POST", "/api/v1/credentials", {
            "source": "qq", "kind": "cookie", "auth_profile": "web_cookie", "label": "second",
            "enabled": False}, expect=(201,))[2]["credential_id"]
        check(selected() == first and secrets() == 1, "create selects only the enabled account")
        check(not get(second)["enabled"], "disabled metadata record has no Cookie")
        status, _, _ = api.call("PUT", "/api/v1/credential-defaults/qq", {"credential_id": second})
        check(status == 422, "reactivation without a fresh Cookie is rejected")
        api.json("PUT", "/api/v1/credential-defaults/qq", {
            "credential_id": second, "secret": {"cookie": "missing_session=synthetic_second"}})
        check(selected() == second and secrets() == 1, "switch keeps exactly one Cookie file")
        check(not get(first)["enabled"] and get(second)["enabled"], "switch disables the previous account")
        check(get(first)["membership"]["status"] == "unknown", "disabled account forgets membership evidence")
        status, _, _ = api.call("POST", f"/api/v1/credentials/{first}/verify", {})
        check(status == 400, "disabled account cannot verify a deleted Cookie")
        stopped = api.json("POST", f"/api/v1/credentials/{second}/disable", {})[2]
        check(not stopped["enabled"] and not stopped["secret_cleanup_pending"], "single account disable completes cleanup")
        check(selected() is None and secrets() == 0, "single account disable clears selection and files")
        again = api.json("POST", f"/api/v1/credentials/{second}/disable", {})[2]
        check(again["version"] == stopped["version"], "repeated disable is idempotent")
        api.json("PUT", "/api/v1/credential-defaults/qq", {
            "credential_id": first, "secret": {"cookie": "missing_session=synthetic_renewed"}})
        check(get(first)["enabled"] and secrets() == 1, "fresh Cookie reactivates retained account ID")
        api.json("DELETE", "/api/v1/credential-defaults/qq")
        check(selected() is None and secrets() == 0, "disable all removes every QQ Cookie")
        service.restart()
        api = Http(service.api, service.token)
        rows = api.json("GET", "/api/v1/credentials?source=qq")[2]["items"]
        check(len(rows) == 2 and all(not r["enabled"] and not r["secret_cleanup_pending"] for r in rows),
              "restart preserves both disabled accounts")
        check(selected() is None and secrets() == 0, "restart never restores deleted Cookies")
        api.json("DELETE", f"/api/v1/credentials/{first}")
        status, _, _ = api.call("PUT", "/api/v1/credential-defaults/qq", {
            "credential_id": first, "secret": {"cookie": "missing_session=synthetic_revoked"}})
        check(status == 422, "revocation is final even with a new Cookie")
        # 只核对引用与开关，不读取秘密正文。
        db_path = next((work / "data").glob("*.sqlite3"))
        with sqlite3.connect(db_path) as db:
            check(db.execute("SELECT count(*) FROM credentials WHERE enabled != 0 OR secret_ref IS NOT NULL").fetchone()[0] == 0,
                  "SQLite contains no enabled account or Cookie reference")
            check(db.execute("SELECT count(*) FROM credential_secret_deletions").fetchone()[0] == 0,
                  "durable cleanup queue is empty")
        return {"account_count": 2, "enabled_count": 0, "cookie_files": 0,
                "switch": True, "disable": True, "disable_all": True,
                "reactivate_requires_cookie": True, "restart": True, "real_cookie_used": False}
    finally:
        service.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--report", type=pathlib.Path, required=True)
    args = parser.parse_args()
    result = run(args.binary)
    args.report.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(result, ensure_ascii=True))
