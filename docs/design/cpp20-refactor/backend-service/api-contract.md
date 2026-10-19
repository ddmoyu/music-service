# 对外 HTTP API 详细契约

状态：目标 v1 设计，尚未实现。实体语义见[模型与存储](data-models.md)，执行与取消见[运行时](runtime-and-tasks.md)。本契约取代旧总览中的候选路由；不提供原 Go 页面 `/music` 的隐含兼容。

## 1. 地址、版本与功能发现

控制 base URL 初始为 `http://127.0.0.1:17632`，JSON 业务前缀 `/api/v1`。媒体 base URL 初始为 `http://127.0.0.1:17633`，前缀 `/media/v1`。部署可通过反向代理统一 origin；客户端应使用 info 与资源 links 返回的地址，不能拼出一个固定媒体端口。

事件 base URL 初始为 `http://127.0.0.1:17634`，前缀 `/events/v1`，独立监听与工作池。进度默认使用 SSE，HTTP 查询用于初始加载、重连同步和兼容兜底。

`GET /api/v1/info` 声明已启用的服务级功能；`GET /api/v1/providers` 声明各平台能力。文档列出目标接口不等于所有平台都支持，也不等于 S0 就实现所有功能。

v1 允许新增可选响应字段；删除、改义、改变单位或必填参数属于破坏性变更，应进入新版本。客户端忽略未知字段；枚举未知值按通用展示处理。请求未知字段返回 400，避免拼错配置被静默忽略。

## 2. 身份、权限与请求约定

控制 API 使用 `Authorization: Bearer <access-token>`，token 对应 principal。首版由管理配置创建 token，不设计注册登录网站；同一个 principal 的多个前端可查看同一组任务，独立 principal 的凭证和文件相互隔离。

| scope | 允许操作 |
| --- | --- |
| `catalog:read` | 来源、搜索、集合、歌词、解析、探测、匹配 |
| `downloads:read` | 自己的下载任务、批次、结果与目标目录列表 |
| `downloads:write` | 创建、取消、重试自己的任务与批次 |
| `files:read` | 自己的文件元数据、媒体会话、媒体票据及音频读取 |
| `credentials:write` | 自己的凭证列表、创建、更新、校验与删除 |
| `admin` | 运行状态与全局下载调度开关；不隐含向普通响应泄漏秘密 |

缺 token／无效 token 返回 401，scope 不足返回 403。资源不属于调用方时按 404 处理；管理员操作其他用户资源需使用明确管理用例，首版普通路由不接受 owner_id 越权参数。

存在 JSON body 时要求 `Content-Type: application/json`，默认 body 上限 256 KiB；非此类型返回 415，超限 413。文档注明无 body 的操作允许空请求体，不因此要求 Content-Type。查询文本 UTF-8，关键词最长 200 字符；平台 ID 最长 1024 字节；枚举大小写敏感。秒／字节单位不能通过前端语言自行猜测。

响应带 `X-Request-Id`。客户端可提供长度不超过 64、仅字母数字与 `._-` 的 request ID，服务仍校验并必要时生成新值。认证信息、Cookie、媒体票据与带签名 URL 不写入普通日志。

受身份影响的响应使用 Cache-Control: no-store；服务内部缓存与浏览器／代理缓存是两件事。CORS 按允许的 origin 配置，OPTIONS 预检不调用音乐平台；允许 Authorization、Content-Type、Idempotency-Key、If-None-Match 等实际使用的头，并暴露 ETag、Retry-After、X-Request-Id。

默认不把 Origin:null 作为通用白名单；本地网页优先由本机 HTTP 服务托管，桌面 WebView 按实际来源配置。原生 HTTP 客户端仍须通过同一身份认证。

## 3. JSON 响应与错误

成功示例：

```json
{
  "data": {"task_id": "tsk_demo", "status": "queued"},
  "meta": {"request_id": "req_demo", "partial": false},
  "error": null
}
```

失败示例：

```json
{
  "data": null,
  "meta": {"request_id": "req_demo", "partial": false},
  "error": {
    "code": "VALIDATION_FAILED",
    "message": "请求参数无效",
    "retryable": false,
    "details": [{"field": "quality", "reason": "不支持的品质名称"}]
  }
}
```

| HTTP | 典型错误码／用途 |
| --- | --- |
| 200 | 查询成功、允许的部分成功、下载去重命中、幂等取消已结束的取消动作 |
| 201 | 创建凭证、播放会话、媒体票据 |
| 202 | 已持久接受后台任务或异步取消，尚未完成 |
| 304 | 任务／文件条件查询未变化，无 JSON body |
| 400 | VALIDATION_FAILED、UNKNOWN_PROVIDER、CURSOR_MISMATCH、未知参数 |
| 401／403 | UNAUTHENTICATED／FORBIDDEN |
| 404 | NOT_FOUND，包含无权查看的资源 |
| 409 | IDEMPOTENCY_CONFLICT、TASK_FINALIZING、TASK_TERMINAL、ACTIVE_DUPLICATE、RESOURCE_CHANGED |
| 410 | RESOURCE_EXPIRED、CURSOR_EXPIRED、FILE_MISSING、MEDIA_TICKET_EXPIRED |
| 413／415 | PAYLOAD_TOO_LARGE／UNSUPPORTED_CONTENT_TYPE |
| 422 | CAPABILITY_UNSUPPORTED、PROVIDER_DISABLED、CONTEXT_REQUIRED、QUALITY_UNAVAILABLE、PREVIEW_REJECTED、CREDENTIALS_REQUIRED、RESOURCE_RESTRICTED |
| 429 | CALLER_LIMIT_EXCEEDED／QUEUE_LIMIT_EXCEEDED，服务方对该调用者的额度限制 |
| 500 | INTERNAL_ERROR，仅显示去敏错误与 request_id |
| 501 | FEATURE_UNAVAILABLE，服务级功能尚未启用 |
| 502 | UPSTREAM_FAILED／UPSTREAM_ALL_FAILED，上游接口或协议失败 |
| 503 | SERVICE_BUSY／STORAGE_UNAVAILABLE，整体容量、存储或停机状态 |
| 504 | UPSTREAM_TIMEOUT／REQUEST_DEADLINE_EXCEEDED |

业务错误中保留 source、operation、上游状态的去敏信息。上游 429 不直接等于调用者超出本服务额度；可作为逐源错误 `UPSTREAM_RATE_LIMITED`，附 retryable 与等待提示。

媒体正常响应是二进制，不套 JSON 信封；HEAD、304、416 等遵守媒体部分。开始发送二进制后发生错误，关闭流并记录，不追加 JSON。

## 4. 幂等、分页和超时

`POST /downloads`、`POST /download-batches`、任务 retry 必须提供 Idempotency-Key，长度 16～128 的可打印安全 ASCII。作用域和至少 24 小时保留规则见[模型](data-models.md)。重放返回首次响应及原状态码，增加 `Idempotency-Replayed: true`；客户端随后查询 task_id 得到最新进度。

连接断开或提交等待超时，不能证明事务一定没有提交；客户端保留原 key 重试，由幂等记录消除结果不确定造成的重复任务。

首次搜索可传多个来源，limit 是**每来源**数量，默认 20、范围 1～50；sources 默认 all（当前启用来源），或逗号分隔的来源 ID，初始上限 8 个，以 limits 公布的配置为准。all 超出配置上限时要求显式缩小来源或调整配置，不静默截断。服务结果顺序固定按平台注册顺序分组，来源内部保留平台顺序，不声称统一相关度排名。

后续页使用该来源的 next_cursor：只传一个 source 和相同查询条件。初次单来源可传 page（默认 1），仅在 Provider 支持页码跳转时允许；多来源不接受 page，cursor 与 page 互斥。没有全局 next_cursor 或全局精确 total。

搜索、匹配、解析等 timeout_ms 默认 10000，范围 1000～15000，入队等待计入预算。平台超时与整体截止分别执行。持久下载任务不继承这个 HTTP 请求的预算。

## 5. 系统与能力接口

| 方法与路径 | 权限 | 返回 |
| --- | --- | --- |
| `GET /health/live` | 无 | 进程可响应时 200，最小状态，不探测平台 |
| `GET /health/ready` | 无 | 可接收工作时 200；启动／迁移／停机或关键存储不可用时 503 |
| `GET /api/v1/info` | 任一有效 scope | 版本、API 版本、features、media_base_url、events_base_url、实例启动标识；支持仅有下载权限的客户端发现事件入口 |
| `GET /api/v1/providers` | catalog:read | source、能力、enabled、品质提示、目录来源类型与凭证状态摘要 |
| `GET /api/v1/limits` | catalog:read | 当前调用者的查询、批次、任务与媒体额度 |
| `GET /api/v1/destinations` | downloads:read | 有权使用的 destination_id、名称与可用状态，不返回实际目录 |

info.data 示例：

```json
{
  "service": "music-dl",
  "version": "0.1.0-design-example",
  "api_version": "v1",
  "instance_id": "inst_demo",
  "media_base_url": "http://127.0.0.1:17633",
  "events_base_url": "http://127.0.0.1:17634",
  "features": {
    "search": true,
    "downloads": true,
    "playback": false,
    "automatic_fallback": false,
    "progress_polling": true,
    "progress_sse": true
  }
}
```

features 反映实际构建及运行配置；客户端不能把该示例的布尔值当作固定能力。

## 6. 搜索

| 方法与路径 | 结果条目 | 公共参数 |
| --- | --- | --- |
| `GET /api/v1/search/tracks` | Track | q、sources、limit、cursor／单源 page、timeout_ms |
| `GET /api/v1/search/playlists` | Playlist | 同上 |
| `GET /api/v1/search/albums` | Album | 同上 |
| `GET /api/v1/search/artists` | ArtistSummary | 同上 |

这些接口需要 catalog:read。q 必填，空白无效；明确给出未知 source 是整体 400，不能悄悄忽略拼错的平台名。

示例请求：`GET /api/v1/search/tracks?q=示例歌曲&sources=netease,qq&limit=20`。

```json
{
  "data": {
    "items": [{
      "ref": {"source": "netease", "id": "demo-track-1"},
      "title": "示例歌曲",
      "artists": [{"name": "示例歌手", "ref": null}],
      "album": {"title": "示例专辑", "ref": null},
      "duration_ms": 210000,
      "version": {"kind": "studio", "label": null, "evidence": []},
      "artwork": null,
      "context_ref": "ctx_demo"
    }]
  },
  "meta": {
    "request_id": "req_demo",
    "partial": true,
    "source_results": [
      {
        "source": "netease", "status": "ok", "elapsed_ms": 140,
        "page": {"limit": 20, "returned": 1, "total": null, "has_more": true, "next_cursor": "cur_demo"},
        "error": null,
        "cache": {"hit": false, "created_at": "2026-10-19T08:00:00.000Z"}
      },
      {
        "source": "qq", "status": "failed", "elapsed_ms": 5000,
        "page": null,
        "error": {"code": "UPSTREAM_TIMEOUT", "message": "来源请求超时", "retryable": true},
        "cache": null
      }
    ]
  },
  "error": null
}
```

至少一个来源 ok，包含已知空列表，返回 200；有未成功来源则 partial=true。全部超时返回 504；其余全部执行失败返回 502，并保留 meta.source_results。全部所选来源均不支持／被禁用时返回 422；混合支持与不支持可返回 200 partial。

继续网易云用相同 q、`sources=netease` 和 `cursor=cur_demo`。QQ 失败项没有下一页游标，需要重试它当前那一页，不能直接跳过。

## 7. 单曲、集合、分类与发现

以下均需要 catalog:read，source 与平台 id 位于查询参数，避免平台保留字符与 URI 路径匹配冲突。

| 方法与路径 | 必填参数 | 可选参数／结果 |
| --- | --- | --- |
| `GET /api/v1/tracks/detail` | source、id | context_ref；Track |
| `GET /api/v1/playlists/detail` | source、id | Playlist，不隐含全部曲目 |
| `GET /api/v1/playlists/tracks` | source、id | limit=50（1～100）、cursor；Track 分页 |
| `GET /api/v1/albums/detail` | source、id | Album |
| `GET /api/v1/albums/tracks` | source、id | limit=50（1～100）、cursor；Track 分页 |
| `GET /api/v1/artists/tracks` | source、id | limit=50（1～100）、cursor |
| `GET /api/v1/artists/albums` | source、id | limit=20（1～50）、cursor |
| `GET /api/v1/categories` | source | Category 列表或分组，标记 catalog_kind |
| `GET /api/v1/categories/playlists` | source、category_id | limit=20（1～50）、cursor |
| `GET /api/v1/charts` | source | Chart 目录，标记 catalog_kind |
| `GET /api/v1/charts/detail` | source、id | Chart 信息 |
| `GET /api/v1/charts/tracks` | source、id | limit=50（1～100）、cursor |
| `GET /api/v1/recommendations/playlists` | source | limit=20（1～50）、cursor（平台支持时） |
| `GET /api/v1/lyrics` | source、id | context_ref；lrc、translated_lrc、karaoke、warnings |
| `POST /api/v1/links/parse` | body.text | 解析实体类型与 ref；分享文本最多 4096 字符 |

曲目／歌单列表 data 为 `{items, page}`，详情 data 为实体对象。集合的 track_count 允许未知，返回的当前页大小不等于集合完整大小。不支持某种详情时明确返回 CAPABILITY_UNSUPPORTED，而不是用搜索结果伪装完整详情。

links/parse 返回 `{matched, kind, ref}`，kind 为 track／playlist／album／artist。未匹配时 200 matched=false；支持的短链可有限展开，目标仍须属于平台允许范围，不实现任意地址跳转代理。

v1 为单实体解析契约；文本含多个不同的已识别实体时返回 400 VALIDATION_FAILED，要求缩小输入范围。重复出现同一实体不视为多个目标。

歌词无内容时返回 200，lrc 为 null 并说明 unavailable；平台不支持歌词属于 422，与一首歌恰好没有歌词不同。karaoke 使用结构化行／词时间，明确毫秒单位，不要求前端识别私有加密格式。

## 8. 音源解析、探测与换源候选

| 方法与路径 | 输入 | 成功结果 |
| --- | --- | --- |
| `POST /api/v1/assets/resolve` | track、context_ref、quality、quality_policy、allow_preview、fallback、可选 match_selection | 200 AssetDescriptor |
| `POST /api/v1/assets/probe` | asset_id | 200 探测报告及证据，过期 410 |
| `POST /api/v1/tracks/matches` | track、context_ref、sources、limit、timeout_ms | 200 match_id、expires_at、candidates、逐源状态 |

均需要 catalog:read，属于有额度的昂贵控制操作。resolve 中的 track 为 TrackRef，客户端提供 title 等展示字段不作为平台身份或匹配证据；后端先读取可信元信息。默认 standard、strict、allow_preview=false、fallback.mode=disabled。

match 请求默认最多返回 5 个候选、上限 10 个，候选包含 candidate_id、Track、score、reasons、availability 和 completeness；score 是排序分数，不宣称统计概率。匹配上下文默认保留 5 分钟。

手动选择时提交 `match_selection: {match_id, candidate_id}`，track 仍是原曲；服务校验匹配上下文的 owner 与原曲，再以候选作为实际歌曲。match_selection 与 auto fallback 互斥。上下文过期返回 410，不能信任客户端自行填的 matchedFrom 字段。

自动换源必须同时被服务启用、请求明确允许，且满足最低匹配条件。网络失败不会任意触发替曲；返回 actual_track、quality 与 warnings。解析结果不给普通客户端原始 CDN URL、秘密请求头或解密密钥；播放与文件获取使用后面的媒体契约。

示例 resolve.data：

```json
{
  "asset_id": "ast_demo",
  "original_track": {"source": "netease", "id": "demo-track-1"},
  "actual_track": {"source": "netease", "id": "demo-track-1"},
  "requested_quality": "high",
  "actual_quality": "high",
  "container": "mp3",
  "bitrate_bps": 320000,
  "size_bytes": 8400000,
  "access_state": "available",
  "completeness": "unknown",
  "expires_at": "2026-10-19T08:05:00.000Z",
  "warnings": [{"code": "FULL_LENGTH_UNVERIFIED", "message": "尚无充分整曲证据"}]
}
```

## 9. 下载任务

### 9.1 创建单个任务

`POST /api/v1/downloads`，downloads:write，必须有 Idempotency-Key。

```json
{
  "track": {"source": "netease", "id": "demo-track-1"},
  "context_ref": "ctx_demo",
  "quality": "high",
  "quality_policy": "allow_lower",
  "allow_preview": false,
  "fallback": {"mode": "disabled", "sources": []},
  "destination_id": "dst_default",
  "metadata_policy": "best_effort",
  "duplicate_policy": "reuse"
}
```

可选 credential_id 选择自己对应 source 的账号；未提供时用自己的默认凭证或平台允许的匿名访问。auto fallback 对其他来源只使用调用方自己的默认凭证。destination_id 缺省为调用方默认目标；不接受任意绝对路径、文件名模板或完整歌曲私有扩展。

quality／策略默认见模型，duplicate_policy 为 reuse（默认）或 force；force 仅允许绕过已完成结果，相同活动任务仍 409。match_selection 可按候选接口的规则附加。

新任务事务提交后返回 202，Location 指向任务查询；去重命中返回 200 和既有 task_id，data.reused=true。返回不等待上游解析与整首下载。

```json
{
  "data": {
    "task_id": "tsk_demo", "status": "queued", "reused": false,
    "links": {"self": "/api/v1/downloads/tsk_demo"}
  },
  "meta": {"request_id": "req_demo", "partial": false},
  "error": null
}
```

### 9.2 查询、取消与重试

| 方法与路径 | 权限 | 语义 |
| --- | --- | --- |
| `GET /api/v1/downloads` | downloads:read | 自己的任务；status、source、batch_id、limit（默认 50，最大 100）、cursor |
| `GET /api/v1/downloads/{task_id}` | downloads:read | 公开 Task 快照，支持 If-None-Match |
| `POST /api/v1/downloads/{task_id}/cancel` | downloads:write | queued 立即取消返回 200；运行中接受取消返回 202 |
| `POST /api/v1/downloads/{task_id}/retry` | downloads:write | failed／canceled 创建新任务；必须有新 Idempotency-Key |

任务列表稳定按 created_at、task_id 倒序分页，游标绑定过滤条件；不接受随意 SQL 排序字段。默认只看自己的任务，关闭前端不会触发 cancel。

batch_id 过滤通过批次条目关系查询，列表中的同一 task_id 只返回一次；批次详情保留输入条目顺序和重复引用。批次统计按条目计算，批次取消按唯一 task_id 发起。

HTTP 进度查询为独立保留的正式接口，不依赖 SSE 开启或已有订阅。列表返回各任务公开快照（包含 progress），单任务接口返回完整快照；客户端可以始终仅使用 HTTP 查询。SSE 与查询使用同一任务模型、权限和版本号。

task.data 示例：

```json
{
  "task_id": "tsk_demo", "version": 8, "status": "downloading", "stage": null,
  "original_track": {"source": "netease", "id": "demo-track-1"},
  "actual_track": {"source": "netease", "id": "demo-track-1"},
  "requested_quality": "high", "actual_quality": "high",
  "attempt": 1, "destination_id": "dst_default",
  "progress": {
    "received_bytes": 4200000, "total_bytes": 8400000,
    "speed_bps": 700000, "eta_ms": 6000, "stage_progress": 0.5
  },
  "created_at": "2026-10-19T08:00:00.000Z",
  "updated_at": "2026-10-19T08:00:06.000Z",
  "next_retry_at": null, "result": null, "error": null, "warnings": []
}
```

查询响应使用基于公开快照版本的弱 ETag，例如 `W/"task-tsk_demo-v8"`；忽略 request_id 等追踪差异，资源语义未变返回 304，客户端保留原快照。默认通过 SSE 更新进度，不同时持续轮询。客户端不支持 SSE 或暂时无法订阅时，可活动任务 1 秒、后台列表 5 秒查询，终态停止；服务可返回 Retry-After。

再次取消 canceling／canceled 返回当前状态，不重复施加副作用。finalizing 返回 409 TASK_FINALIZING；completed／failed 返回 409 TASK_TERMINAL。取消不会隐含删除已发布文件。

retry body 可为空，沿用原请求快照，但需重新校验当前权限与目标可用性；若要改变品质／目标则创建普通新任务。返回 202 新 task_id 和 retry_of，不改变旧任务终态。原任务未终态返回 409。

### 9.3 批量任务

`POST /api/v1/download-batches`，downloads:write，必须有 Idempotency-Key。body 为 `{items: [单任务请求, ...]}`，1～100 项，总 body 仍受 256 KiB 限制。

结构、权限、额度和入队上下文校验全有或全无：任意项无效返回 422／相应身份或额度错误，details 标出 `items[index]`，不创建其他任务。上游能否实际取流在各任务执行时判定，不为验证批次而预先下载全部音源。

事务内按输入顺序解析去重，重复项可关联同一既有 task。至少创建一条新任务返回 202；全部复用返回 200；响应包含 batch_id、created_count、reused_count 和 item 到 task_id 的映射。执行阶段各任务独立成功／失败，不因其中一首失败回滚已完成文件。

| 方法与路径 | 结果 |
| --- | --- |
| `GET /api/v1/download-batches/{batch_id}` | downloads:read；批次条目、任务引用、按状态统计，最多 100 项 |
| `POST /api/v1/download-batches/{batch_id}/cancel` | downloads:write；逐任务请求取消，返回已接受项与不能取消项，存在异步取消时 202 |

一个任务可能被多个批次复用，取消批次按任务执行实际取消，不只是隐藏批次条目；响应和前端需说明这一点。未执行任何异步取消时返回 200，包括全部已终态的批次。

### 9.4 SSE 下载事件

`GET /events/v1/downloads` 位于 events_base_url，要求 `downloads:read` 与 `Authorization: Bearer <access-token>`，无 body、无任务过滤参数。一条连接订阅当前 principal 的全部任务变化；不同身份完全隔离。创建、取消、重试仍用控制 API，断开订阅不影响任务执行。

鉴权、限额和订阅注册在发头前完成；失败返回普通 JSON 错误。成功返回 200、`Content-Type: text/event-stream`、`Cache-Control: no-store`，可设置 `X-Accel-Buffering: no` 提示代理。流采用 UTF-8 SSE 帧，不套通用 JSON 信封，不设置有限 Content-Length；代理必须关闭事件缓冲和影响及时刷新的压缩。配置允许的 Origin 和 Authorization 预检，不依赖媒体票据。

| 事件 | data 内容与用途 |
| --- | --- |
| `stream.ready` | `{instance_id, sync_required: true}`；订阅已注册，首次连接和每次重连都重新获取 HTTP 状态 |
| `task.updated` | task_id、version、status、progress、updated_at；创建、进度和状态变化共用此类型 |
| 注释心跳 | `: heartbeat` 加空行，不是业务事件，默认每 15 秒发送 |

示例帧（空行结束一条消息）：

```text
event: task.updated
data: {"task_id":"tsk_demo","version":8,"status":"downloading","progress":{"received_bytes":4200000,"total_bytes":8400000,"speed_bps":700000,"eta_ms":6000,"stage_progress":0.5},"updated_at":"2026-10-19T08:00:06.000Z"}

```

progress 使用任务快照相同字段和未知值规则；消息是进度投影，不覆盖整个 Task。终态事件到达后通过任务查询获取结果文件、完整错误和 warnings；其他任务详情视为已失效，需要展示时查询。客户端按 task_id/version 合并，只接受较新版本，不能以网络到达顺序覆盖新状态。

首次与重连同步顺序：先建立订阅，收到 stream.ready 后暂存事件，再分页查询当前列表，并单独刷新已有页面追踪的任务（包括断线期间已完成的任务），最后按版本合并暂存事件。这样避免“先查询、后订阅”之间遗漏更新。同步期间事件缓存也必须有界；溢出则重建订阅并重新同步，不继续展示自称完整的状态。

首版不发送 SSE `id`，不提供事件日志重放；收到 Last-Event-ID 也不把它当作恢复成功。每次重新连接都执行上述同步。服务端注册完成后的提交必须发布到该订阅；提交成功但发布失败时关闭受影响流，迫使客户端重新同步。服务重启关闭所有旧流，instance_id 变化后同样刷新状态。

浏览器首版采用 `fetch` + ReadableStream 读取标准 SSE，并带 Bearer 头。原生 EventSource 构造器不能指定任意 Authorization 头，因此不能直接替换该流程；不把长期 token 放入 URL。fetch 客户端需处理跨网络分块的 UTF-8／SSE 帧、AbortController、重连与状态同步，使用协议解析器而非把每个网络块当作一条消息。[EventSource 标准](https://html.spec.whatwg.org/multipage/server-sent-events.html)

断线默认从 1 秒开始带抖动退避，最大 30 秒；429／503 遵守 Retry-After，401／403 停止自动重试并处理身份问题。持续收到心跳／事件满 30 秒后重置退避，避免连接刚建立就失败导致快速循环。读流超过 45 秒无任何字节则主动重连。客户端恢复网络或重新打开页面时不重新创建下载任务。

### 9.5 SSE 验证要求

验证并行搜索／取消不受满额 SSE 订阅占用控制线程；多任务共用连接；跨 principal 无事件泄漏；终态即时通知；网络分块和 UTF-8 边界；查询与事件竞争的版本合并；断线期间完成／服务重启恢复；慢客户端队列有界；凭证撤销；代理无缓冲；连接与内存全部释放。尚未编写实现或运行这些集成验证。

## 10. 文件与在线播放

### 10.1 文件元数据与下载结果

`GET /api/v1/files/{file_id}`，files:read，返回 file_id、display_name、size_bytes、container、actual_track、quality、metadata_status、ETag、media_url。无服务器绝对路径；文件缺失返回 410 FILE_MISSING。

completed 任务的 result 至少为：

```json
{
  "file_id": "fil_demo",
  "metadata_status": "complete",
  "links": {"file": "/api/v1/files/fil_demo"}
}
```

媒体 URL 指向已保存文件，不会重新调用音乐平台。客户端是否保存到自己本地，由客户端对媒体响应执行写盘决定。

### 10.2 播放会话

`POST /api/v1/playback-sessions`，files:read，输入 track、context_ref、quality 与解析策略，成功 201 返回 session_id、actual_track、音频描述、expires_at 和 media_url。会话默认 30 分钟绝对有效，创建受解析额度约束；服务未启用 playback 返回 501。

`DELETE /api/v1/playback-sessions/{session_id}`，files:read，撤销会话、关联票据并请求停止活动流；返回 200 closed=true，重复关闭仍成功。到期会话不能再发起新的 Range 请求，新建会话会重新解析。

session 代表固定实际录音。URL 过期可以重取相同资源；不能在 seek 或中途错误时静默替换成别的演唱版本。

### 10.3 浏览器媒体票据

普通 audio 元素不便设置 Authorization，使用 `POST /api/v1/media-tickets`，files:read：

```json
{
  "target": {"kind": "file", "id": "fil_demo"},
  "ttl_seconds": 60
}
```

kind 可为 file／playback_session，TTL 范围 15～300 秒，默认 60。返回 201，data 包含一次返回的随机票据 URL 与 expires_at；秘密 token 此后不通过查询接口再展示，响应 Cache-Control: no-store。

票据仅允许对应媒体资源读取，可在有效期内多次 Range；不能拿来查询任务、调用搜索或读取其他文件。客户端在需要新 seek 请求且票据到期时重新申请。关闭会话或撤销身份后拒绝新的票据请求和媒体请求。

### 10.4 媒体路由

以下位于媒体监听，正常返回二进制。原生客户端可用 Bearer，浏览器可用已授权票据：

| 方法与路径 | 授权 | 内容 |
| --- | --- | --- |
| `GET/HEAD /media/v1/files/{file_id}` | files:read Bearer | 已完成、不可变文件 |
| `GET/HEAD /media/v1/playback/{session_id}` | files:read Bearer | 对应播放会话的上游音频 |
| `GET/HEAD /media/v1/tickets/{token}` | 资源专用票据 | 映射到固定 file 或 playback_session |

媒体 CORS 允许配置的前端来源与 Range／条件请求，暴露 Content-Length、Content-Range、Accept-Ranges、ETag、Content-Disposition。票据不得写入 access log、错误回显或 Referer 外发。

### 10.5 Range、缓存与错误

- 文件完整 GET 返回 200、Content-Type、已知 Content-Length、强 ETag 与 Accept-Ranges；HEAD 返回完整资源元信息且无 body，忽略 Range。
- 支持单段 `bytes=start-end`、`bytes=start-`、`bytes=-suffix`；合法部分文件返回 206 及 Content-Range。
- 有效字节范围与资源无交集返回 416，并带 `Content-Range: bytes */总长度`；没有 JSON 音频 body。
- 首版不实现 multipart/byteranges；多段请求明确返回 400 MULTIPLE_RANGES_UNSUPPORTED。未知 Range 单位按 HTTP 语义忽略，按完整请求处理。
- 文件 If-None-Match 命中时返回 304；If-Range 不匹配时返回完整 200，不能把新文件片段追加到旧资源。
- 文件默认附件 disposition，播放流为 inline；展示文件名提供编码形式，不使用内部路径。
- 代理播放仅转发校验过的范围，200／206／416 依据上游实际结果；上游忽略 Range 就返回 200，不伪造 206。
- 上游内容类型／长度不足以安全处理时明确失败或降级完整流，不伪造总长。发头前错误可返回去敏错误，发头后失败终止传输。

范围与条件请求基于 [HTTP Semantics](https://www.rfc-editor.org/rfc/rfc9110.html#name-range)。媒体票据响应为私有资源，默认 no-store；ETag 用于条件请求与资源一致性，不表示允许共享代理缓存。

## 11. 凭证管理

以下需要 credentials:write，所有秘密只在写入请求出现，响应仅有 CredentialDescriptor：

| 方法与路径 | 输入／语义 |
| --- | --- |
| `GET /api/v1/credentials` | 可选 source，列出自己的描述和状态 |
| `POST /api/v1/credentials` | source、label、cookie；201，新 credential_id；Cookie 最多 64 KiB |
| `PUT /api/v1/credentials/{credential_id}` | 更新 cookie／label；200，凭证版本递增并使对应缓存失效 |
| `POST /api/v1/credentials/{credential_id}/verify` | 无秘密 body；有限平台校验，200 返回 valid／invalid／unknown 与原因 |
| `DELETE /api/v1/credentials/{credential_id}` | 200，撤销未来使用；重复撤销自己的已知记录仍成功 |

校验接口不替代账号登录。暂不设计平台扫码／短信登录流程；未来按平台增加独立能力。删除凭证会阻止后续读取和重新解析，但已经交给在途请求的凭证不保证可被“收回”；需停止下载时调用任务取消。

默认凭证选择由每用户配置决定；写入不存在或不属于自己的 credential_id 返回 404。不能将任意他人的 Cookie 作为 query 参数覆盖整个服务的当前账号。

## 12. 管理接口

| 方法与路径 | 权限 | 内容 |
| --- | --- | --- |
| `GET /api/v1/admin/status` | admin | 工作池占用、排队、任务统计、存储与启动状态；无秘密 |
| `GET /api/v1/admin/download-policy` | admin | 当前是否暂停启动新任务 |
| `PATCH /api/v1/admin/download-policy` | admin | body `{paused: true/false}`，200 返回新策略 |

paused 只停止领取新任务，已有任务继续；取消活动任务仍走明确任务接口。首版没有任意远程修改根目录、监听端口、TLS 后端或执行系统命令的管理路由，相关配置在启动时加载。

## 13. 前端最小接入流程

1. 配置服务地址和 token，调用 info／providers／destinations，显示真实能力。
2. 搜索或进入集合详情，保留 TrackRef 与 context_ref；部分来源失败时展示明细。
3. 创建任务前生成新的 Idempotency-Key；网络不确定时用同一个 key 重试提交。
4. 建立 SSE 订阅后查询当前下载状态，按版本合并事件；重连时重新同步，无需重提下载。不支持订阅时条件轮询兜底。
5. completed 后按 file_id 取描述；原生客户端用 Bearer 获取媒体，网页需要时申请票据。
6. 用户点取消则调用取消接口并等终态；关闭 SSE、停止轮询或关闭窗口不等于取消。

## 14. 契约验证清单

重点验证 JSON 示例可解析、单位一致、逐源分页不重复第一页、所有错误有 request_id、下载提交可靠持久化、同键幂等、批次创建原子性、资源 owner 隔离、状态转换竞争、媒体票据范围与到期、Range／If-Range／304／416，以及关闭前端后任务生命周期独立。

后续实现阶段再按本契约编写 OpenAPI 文档和契约验证；本轮不生成服务代码或客户端 SDK。
