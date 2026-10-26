"""Generate the core frontend contract; no schema-generator dependency."""
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def document():
    version = re.search(r"VERSION\s+(\d+\.\d+\.\d+)", (ROOT / "CMakeLists.txt").read_text())[1]
    ref = lambda name: {"$ref": "#/components/schemas/" + name}
    string = {"type": "string"}
    nullable_string = {"type": ["string", "null"]}
    number = {"type": "integer", "minimum": 0}
    nullable_number = {"type": ["integer", "null"], "minimum": 0}
    boolean = {"type": "boolean"}

    def obj(properties=None, required=(), closed=False):
        result = {"type": "object", "properties": properties or {}}
        if required:
            result["required"] = list(required)
        if closed:
            result["additionalProperties"] = False
        return result

    def array(item):
        return {"type": "array", "items": item}

    schemas = {}
    schemas["TrackRef"] = obj({"source": {**string, "minLength": 1}, "id": {**string, "minLength": 1, "maxLength": 1024}},
                              ("source", "id"), True)
    schemas["Error"] = obj({"code": string, "message": string, "retryable": boolean,
                            "details": array(obj())}, ("code", "message", "retryable", "details"))
    schemas["Meta"] = obj({"request_id": string, "partial": boolean, "source_results": array(obj())},
                          ("request_id", "partial"))
    schemas["Page"] = obj({"limit": number, "next_cursor": nullable_string}, ("limit", "next_cursor"))
    schemas["Info"] = obj({"service": {"const": "music-service"}, "version": string, "api_version": {"const": "v1"},
                           "instance_id": string, "media_base_url": nullable_string, "events_base_url": nullable_string,
                           "features": {"type": "object", "additionalProperties": boolean}},
                          ("service", "version", "api_version", "instance_id", "features", "media_base_url", "events_base_url"))
    schemas["Track"] = obj({"ref": ref("TrackRef"), "title": string, "artists": array(obj({"name": string})),
                            "album": {}, "duration_ms": nullable_number, "context_ref": nullable_string,
                            "version": obj(), "artwork": {}, "pay": obj({
                                key: {"enum": [0, 1, None]} for key in ("pay_play", "pay_down", "pay_month")})},
                           ("ref", "title", "artists", "duration_ms", "context_ref", "pay"))
    quality = {"type": "string", "enum": ["standard", "high", "lossless", "hires"], "default": "standard"}
    requirement = {"track": ref("TrackRef"), "context_ref": string, "quality": quality,
                   "quality_policy": {"enum": ["strict", "allow_lower"], "default": "strict"},
                   "allow_preview": {"type": "boolean", "default": False}}
    schemas["ResolveRequest"] = obj(requirement, ("track",), True)
    schemas["SwitchRequest"] = obj({**requirement, "attempted_sources": array(string)},
                                   ("track", "attempted_sources"), True)
    schemas["Asset"] = obj({"asset_id": string, "original_track": ref("TrackRef"), "actual_track": ref("TrackRef"),
                            "source_switched": boolean, "source_retry": obj(), "requested_quality": quality,
                            "actual_quality": nullable_string, "container": nullable_string,
                            "bitrate_bps": nullable_number, "completeness": string, "expires_at": string,
                            "warnings": array(obj())}, ("asset_id", "original_track", "actual_track", "expires_at"))
    schemas["DownloadRequest"] = obj({**requirement, "credential_id": string, "destination_id": string,
        "metadata_policy": {"enum": ["best_effort", "required", "skip"], "default": "best_effort"},
        "duplicate_policy": {"enum": ["reuse", "force"], "default": "reuse"}}, ("track",), True)
    schemas["AcceptedTask"] = obj({"task_id": string, "status": string, "reused": boolean, "links": obj()},
                                  ("task_id", "status", "reused", "links"))
    schemas["Task"] = obj({"task_id": string, "version": number, "status": {"enum": [
        "queued", "resolving", "probing", "downloading", "processing", "finalizing",
        "waiting_retry", "canceling", "completed", "failed", "canceled"]},
        "original_track": ref("TrackRef"), "actual_track": {"anyOf": [ref("TrackRef"), {"type": "null"}]},
        "progress": {"type": ["object", "null"]}, "result": {"type": ["object", "null"]},
        "error": {"type": ["object", "null"]}, "source_retry": {"type": ["object", "null"]},
        "created_at": string, "updated_at": string}, ("task_id", "version", "status", "progress", "result", "error"))
    schemas["MediaTarget"] = obj({"kind": {"enum": ["asset", "file"]}, "id": string}, ("kind", "id"), True)
    schemas["MediaRequest"] = obj({"target": ref("MediaTarget"), "mode": {"enum": ["auto", "direct", "proxy"], "default": "auto"},
        "auth": {"enum": ["ticket", "bearer"], "default": "ticket"},
        "ttl_seconds": {"type": "integer", "minimum": 15, "maximum": 300}}, ("target",), True)
    schemas["MediaAccess"] = obj({"target": ref("MediaTarget"), "delivery": {"enum": ["direct", "proxy"]},
        "url": {"type": "string", "format": "uri"}, "auth": {"enum": ["url", "bearer"]},
        "expires_at": nullable_string, "content_type": nullable_string,
        "range_supported": {"type": ["boolean", "null"]}, "actual_track": {}, "actual_quality": nullable_string},
        ("target", "delivery", "url", "auth", "expires_at"))

    def envelope(data):
        return obj({"data": data, "meta": ref("Meta"), "error": {"type": "null"}}, ("data", "meta", "error"))

    schemas["ErrorEnvelope"] = obj({"data": {"type": "null"}, "meta": ref("Meta"), "error": ref("Error")},
                                   ("data", "meta", "error"))
    paths = {}
    error = {"description": "按 HTTP 状态处理错误；429/503 可带 Retry-After。",
             "headers": {"Retry-After": {"schema": string}, "X-Request-Id": {"schema": string}},
             "content": {"application/json": {"schema": ref("ErrorEnvelope")}}}

    def param(name, schema=None, required=False, location="query", description=""):
        return {"name": name, "in": location, "required": required,
                "schema": schema or string, "description": description}

    def op(path, method, title, data=None, scope="catalog:read", body=None, parameters=(), codes=("200",), description=""):
        operation = {"operationId": method + "_" + re.sub(r"[^a-zA-Z0-9]+", "_", path).strip("_"),
                     "summary": title, "description": description, "x-required-scopes": [scope] if scope else [],
                     "parameters": list(parameters),
                     "responses": {code: {"description": "成功；202 仅代表已受理。",
                         "content": {"application/json": {"schema": envelope(data or obj())}}} for code in codes}}
        operation["responses"]["default"] = error
        if scope is None:
            operation["security"] = []
        if body is not None:
            operation["requestBody"] = {"required": True, "content": {"application/json": {"schema": body}}}
        paths.setdefault(path, {})[method] = operation
        return operation

    paginated = lambda item: obj({"items": array(item), "page": ref("Page")}, ("items", "page"))
    paging = [param("limit", {"type": "integer", "minimum": 1, "maximum": 100, "default": 50}), param("cursor")]
    idem = param("Idempotency-Key", {"type": "string", "minLength": 16, "maxLength": 128, "pattern": "^[!-~]+$"}, True, "header",
                 "一次提交使用一个键；网络不确定时沿用。手动重试任务生成新键。")
    for name in ("live", "ready"):
        op("/health/" + name, "get", "健康检查", obj({"status": string}), None)
    op("/api/v1/info", "get", "实例、版本与功能发现", ref("Info"), "任一有效 scope")
    op("/api/v1/providers", "get", "查询实际平台能力", paginated(obj()), parameters=[
        param("capability"), param("enabled", boolean), *paging])
    op("/api/v1/limits", "get", "调用方容量与预算")
    op("/api/v1/destinations", "get", "有权使用的下载目录", obj({"items": array(obj({
        "destination_id": string, "name": string, "available": boolean, "accept_new_tasks": boolean}))}),
        "downloads:read")
    for kind in ("tracks", "playlists", "albums", "artists"):
        op("/api/v1/search/" + kind, "get", "搜索 " + kind, obj({"items": array(ref("Track") if kind == "tracks" else obj())}),
           parameters=[param("q", {**string, "minLength": 1, "maxLength": 200}, True),
                       param("sources", string, description="逗号分隔或 all；平台是开放字符串。"),
                       param("limit", {"type": "integer", "minimum": 1, "maximum": 50, "default": 20}),
                       param("cursor", string, description="逐源游标使用契约定义的编码，不能当全局页码。"),
                       param("timeout_ms", {"type": "integer", "minimum": 1000})],
           description="200 可能部分成功，必须保留 meta.partial/source_results；当前实现不支持 page 跳页。")
    op("/api/v1/tracks/detail", "get", "歌曲详情", ref("Track"), parameters=[
        param("source", required=True), param("id", required=True), param("context_ref")])
    op("/api/v1/assets/resolve", "post", "解析指定平台歌曲", ref("Asset"), body=ref("ResolveRequest"))
    op("/api/v1/assets/switch-source", "post", "严格匹配并显式换源", ref("Asset"), body=ref("SwitchRequest"),
       description="始终传最初 track 与累积 attempted_sources；不要将 actual_track 当作原曲。")
    op("/api/v1/assets/probe", "post", "探测资产", body=obj({"asset_id": string}, ("asset_id",), True))
    op("/api/v1/source-statistics", "get", "当前调用方平台成功率")
    op("/api/v1/downloads", "post", "受理后台下载", ref("AcceptedTask"), "downloads:write",
       ref("DownloadRequest"), [idem], ("200", "202"))
    op("/api/v1/downloads", "get", "分页查询任务", paginated(ref("Task")), "downloads:read",
       parameters=[*paging, param("status"), param("source"), param("batch_id")])
    task_id = param("task_id", required=True, location="path")
    task = op("/api/v1/downloads/{task_id}", "get", "查询任务快照", ref("Task"), "downloads:read",
              parameters=[task_id, param("If-None-Match", location="header")])
    task["responses"]["304"] = {"description": "未变化，无响应体；沿用本地快照。"}
    op("/api/v1/downloads/{task_id}/cancel", "post", "取消任务", ref("Task"), "downloads:write",
       parameters=[task_id], codes=("200", "202"))
    op("/api/v1/downloads/{task_id}/retry", "post", "创建换源重试任务", ref("AcceptedTask"), "downloads:write",
       parameters=[task_id, idem], codes=("202",), description="failed/canceled 可重试；沿用已尝试平台，返回新 task_id。")
    op("/api/v1/download-batches", "post", "批量提交", scope="downloads:write",
       body=obj({"items": {"type": "array", "items": ref("DownloadRequest"), "minItems": 1, "maxItems": 100}},
                ("items",), True), parameters=[idem], codes=("200", "202"))
    batch_id = param("batch_id", required=True, location="path")
    op("/api/v1/download-batches/{batch_id}", "get", "查询批次", scope="downloads:read", parameters=[batch_id])
    op("/api/v1/download-batches/{batch_id}/cancel", "post", "取消批次", scope="downloads:write",
       parameters=[batch_id], codes=("200", "202"))
    file_id = param("file_id", required=True, location="path")
    file = op("/api/v1/files/{file_id}", "get", "读取文件描述", scope="files:read",
              parameters=[file_id, param("If-None-Match", location="header")])
    file["responses"]["304"] = {"description": "未变化，无响应体。"}
    op("/api/v1/media-access", "post", "获取浏览器可用的资源票据 URL", ref("MediaAccess"),
       "file 需要 files:read；asset 需要 media:read", ref("MediaRequest"),
       description="在线播放需先 resolve。auth=ticket 返回 auth=url，长期服务 Token 不进入 URL。")
    stream = op("/events/v1/downloads", "get", "订阅本人全部任务", scope="downloads:read",
                description="fetch + ReadableStream，带 Bearer；stream.ready 后重新查询状态，task.updated 按 version 合并。断开不取消任务。")
    stream["servers"] = [{"url": "http://127.0.0.1:17634", "description": "必须替换为 info.events_base_url"}]
    stream["responses"]["200"] = {"description": "UTF-8 SSE，可能跨网络块；心跳不是业务事件。",
        "content": {"text/event-stream": {"schema": string, "example":
            'event: stream.ready\ndata: {"instance_id":"inst_demo","sync_required":true}\n\n'}}}
    for route, identifier, scope in (("files", "file_id", "files:read"), ("assets", "asset_id", "media:read"),
                                      ("tickets", "token", None)):
        for method in ("get", "head"):
            media = op("/media/v1/" + route + "/{" + identifier + "}", method, "读取音频或文件",
                scope=scope, parameters=[param(identifier, required=True, location="path"),
                    param("Range", location="header"), param("If-Range", location="header"),
                    param("If-None-Match", location="header")])
            if route != "assets":
                media["parameters"].append(param("part", {"enum": ["audio", "lyrics", "cover"]}))
            media["servers"] = [{"url": "http://127.0.0.1:17633", "description": "使用 info.media_base_url 或 media-access 返回的完整 URL"}]
            if route == "tickets":
                media["description"] = "token 是已授权的资源专用票据，不是服务 Bearer；不要记录此 URL。"
            for status in ("200", "206"):
                media["responses"][status] = {"description": "二进制；HEAD 无响应体。", "headers": {
                    key: {"schema": string} for key in ("Content-Type", "Content-Length", "Content-Range", "Accept-Ranges", "ETag")}}
                if method == "get":
                    media["responses"][status]["content"] = {"application/octet-stream": {"schema": {"type": "string", "format": "binary"}}}
            media["responses"]["304"] = {"description": "未变化，无响应体。"}
    return {
        "openapi": "3.1.1",
        "info": {"title": "music-service 核心前端接口", "version": version,
                 "description": "覆盖能力发现、搜索、解析播放、下载与 SSE。账号管理及完整目录接口见 API 契约；升级要求见对应版本发行说明。"},
        "servers": [{"url": "http://127.0.0.1:17632", "description": "替换为部署地址或启动器实际地址"}],
        "security": [{"bearerAuth": []}],
        "paths": paths,
        "components": {"securitySchemes": {"bearerAuth": {"type": "http", "scheme": "bearer"}}, "schemas": schemas},
    }


def serialized():
    return json.dumps(document(), ensure_ascii=False, indent=2) + "\n"


if __name__ == "__main__":
    output = ROOT / "docs/api/openapi.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(serialized(), encoding="utf-8")
    print(output)
