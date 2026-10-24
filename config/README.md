# 配置资源

配置由 `infrastructure/config` 加载：内置缺省 → JSON 文件 → 已列明的 `MUSIC_SERVICE_*` 环境变量 → 命令行 `--config`／`--data-dir`。默认值的规范位置是[配置与部署](../docs/design/cpp20-refactor/backend-service/configuration-and-deployment.md#配置来源与生效时机)，本页只列 JSON 键名。未知键、类型错误和越界值都会让启动失败，一次报告全部问题，字段路径形如 `/control/port`。

真实 token、Cookie、API key、OAuth 秘密和本机目录留在源码树之外。

## 路径规则

- 文件中的相对路径相对于配置文件所在目录；环境变量和命令行给出的相对路径相对于进程工作目录。
- 未指定 data_dir 时为 `<配置文件目录或工作目录>/data`。
- `MUSIC_SERVICE_DOWNLOAD_DIR` 只替换 id 为 `dst_default` 的目标根目录，未配置该目标时报错。

## 键名

时长与大小的单位写在键名后缀里：`_ms`、`_seconds`、`_hours`、`_bytes`。

| 组 | 键 |
| --- | --- |
| 顶层 | `config_version`（只能为 1）、`data_dir`、`destinations`、`providers`、`app_profiles`（OAuth 应用配置，仅对实际绑定 OAuth 工厂的平台有效） |
| `control` | `host`、`port`、`workers`、`connection_queue` |
| `media`、`events` | `enabled`、`host`、`port`、`workers`、`connection_queue`（构建包含媒体与 SSE 模块时两者默认启用，分别监听 17633 与 17634；info 声明 file_access／media_base_url 与 progress_sse／events_base_url，关闭时如实声明降级） |
| `event_limits` | `global_subscriptions`、`per_principal_subscriptions`、`max_queued_events`、`max_queued_bytes`、`max_event_bytes` |
| `event_stream` | `progress_interval_ms`、`heartbeat_seconds`、`write_idle_seconds`、`max_connection_seconds` |
| `public_urls` | `api_base`、`media_base`、`events_base` |
| `security` | `allow_insecure_remote` |
| `http` | `body_limit_bytes`、`read_idle_seconds`、`write_idle_seconds`、`keep_alive_seconds`、`max_requests_per_connection` |
| `cors` | `allowed_origins` |
| `auth` | `mode`（只能为 bearer） |
| `admission` | `global`、`per_principal` |
| `upstream` | `short_workers`、`queue`、`per_source_short`、`per_principal`、`per_origin`、`max_total_connections`、`max_response_bytes`、`redirect_max`、`connect_timeout_seconds` |
| `catalog` | `total_budget_ms`、`max_budget_ms`、`per_source_budget_ms`、`fanout`、`max_sources_per_request`、`default_sources` |
| `providers.<source>` | `enabled`、`implementation`（只能为 native：所有平台由服务原生实现，不调用外部工具）、`options`（对象，由平台包校验；没有校验器的平台只接受空对象）、`app_profile_id`、`rate_limit_group` |
| `downloads` | `workers`、`per_source`、`max_attempts`、`transport_retries`、`max_file_bytes` |
| `retry` | `initial_backoff_ms`、`multiplier` |
| `download_limits` | `per_principal_active`、`global_active`、`max_batch_items` |
| `download_timeouts` | `connect_seconds`、`read_idle_seconds`、`attempt_seconds`、`absolute_seconds` |
| `idempotency` | `retention_hours` |
| `processing` | `workers`、`queue` |
| `media_limits` | `global`、`per_principal`、`upstream`、`local_files` |
| `media_upstream` | `workers` |
| `media_stream` | `bridge_buffer_bytes`、`connect_timeout_seconds`、`idle_seconds`、`max_stream_seconds` |
| `lifetimes` | `asset_seconds`、`cursor_seconds`、`media_access_seconds` |
| `storage` | `writer_queue`、`read_connections`、`wait_budget_ms` |
| `cache` | `catalog_entries`、`catalog_bytes`、`catalog_ttl_seconds` |
| `handles` | `contexts`、`cursors`、`assets`、`media_tickets`、`total_bytes` |
| `shutdown` | `soft_seconds` |
| `filesystem` | `failed_work_retention_hours` |
| `features` | `file_access`（未设置时跟随 media.enabled）、`media_proxy`、`direct_media_urls`、`transcoding` |
| `logging` | `format`（json／text）、`level`（trace／debug／info／warn／error）、`file_enabled`、`max_file_bytes`、`retained_files` |
| `destinations[]` | `id`、`name`、`root`、`principals`、`accept_new_tasks` |

## 运维观测与日志轮转

`GET /api/v1/admin/metrics` 提供 JSON 指标，`?format=prometheus` 导出 Prometheus 文本，均需 admin scope。`admin/status` 同时返回线程池、事件订阅和异步日志队列状态。计数和延迟覆盖控制请求、上游、下载执行、音频处理、存储读写、媒体及事件，详细口径见[运维观测](../docs/operations-observability.md)。

`logging.file_enabled` 默认 false（前台输出到 stderr），Windows 服务模式自动开启文件日志。文件为 `data_dir/logs/music-service.log`；`max_file_bytes` 默认 10 MiB，允许 64 KiB～1 GiB；`retained_files` 默认 5，允许 0～20。默认最多保留当前文件与 5 个编号归档。写入、刷新和轮转由有界后台队列执行。

跨平台匹配、手动与自动换源已取消。旧配置的 `features.automatic_fallback=false` 兼容忽略，true 拒绝启动；`lifetimes.match_seconds` 和 `handles.matches` 仅校验旧值范围后忽略，已无对应运行对象。

## 容量与超时的实际行为

- `admission` 控制搜索、详情、解析、探测、凭证验证和媒体访问描述等上游请求；每 principal 超额返回 429，全局超额返回 503，均带 `Retry-After: 1`。实际全局额度取 `min(admission.global, control.workers - 1)`，`control.workers` 至少为 2；`GET /api/v1/limits` 返回实际额度。上游请求完成或拒绝后关闭该 HTTP 连接，防止空闲 keep-alive 占住保留线程。
- 默认单次最多选择 16 个来源，覆盖当前 9 个内置平台；同时执行的来源仍由 `catalog.fanout`（默认 4）限制。
- 搜索与列表游标共用 `handles.cursors` 数量上限。游标、音源句柄、媒体票据及过期记录共用 `handles.total_bytes` 估算预算，计入字符串／容器容量；每秒清理到期内容，新增句柄遇到容量不足时跨表回收过期内容，仍超限则返回 `SERVICE_BUSY`。它不是整个进程的 RSS 限额。
- 下载传输与媒体代理分别使用 `download_timeouts.connect_seconds/read_idle_seconds` 和 `media_stream.connect_timeout_seconds/idle_seconds`；每个重定向沿用这些设置，且受操作剩余期限约束。
- 未实现的 `cache.failure_suppress_seconds`、`cache.app_token_entries` 不再接受；旧配置中若包含它们，请删除后启动。失败结果不缓存。
- 默认日志异步输出，最多排队 1024 条／1 MiB（含正在输出的记录），超额丢弃并计数；停机最多等待 500 ms 排空。阻塞的输出只保留自己拥有的输出句柄和状态，整个进程最多 4 个日志工作线程。`admin/status.logging` 提供丢弃数、输出失败数与占用字节。

## 下载文件输出（0.1.1）

在现有配置中添加 `downloads.output`；省略时使用下列默认值。修改后重启服务，仅影响新受理任务。

```json
{
  "downloads": {
    "output": {
      "name_format": "artist_title",
      "folder_layout": "flat",
      "embed_cover": true,
      "embed_lyrics": true,
      "cover_max_bytes": 8388608,
      "metadata_timeout_seconds": 10
    }
  }
}
```

| 配置 | 允许值和含义 |
| --- | --- |
| `name_format` | `title` 歌名；`artist_title` 歌手 - 歌名；`title_artist` 歌名 - 歌手 |
| `folder_layout` | `flat` 平铺；`artist` 歌手目录；`artist_album` 歌手／专辑目录 |
| `embed_cover`、`embed_lyrics` | 布尔值，独立控制嵌入；任务 `metadata_policy=skip` 时不嵌入；若开启侧文件，仍独立获取对应内容 |
| `export_lyrics`、`export_cover` | 默认 false；独立导出 `.lrc`、JPEG／PNG，与嵌入选项互不依赖 |
| `sidecars_required` | 默认 false；开启后所有已选择的侧文件必须可用，否则任务失败；至少启用一项导出 |
| `lyrics_variant` | 默认 `lrc`；`verbatim` 优先逐字时间；`romanized` 优先罗马音；缺失回退可用原文并返回警告 |
| `cover_max_bytes` | 1～33554432 字节，默认 8 MiB；仅接收通过结构检查的 JPEG／PNG，不缩放、不转码 |
| `metadata_timeout_seconds` | 1～60 秒；每次封面或歌词获取预算，且不超过当前任务／尝试剩余期限 |

缺失歌手／专辑目录分别用 `未知歌手`／`未知专辑`；歌名为空时用 source-id。每段限制为 60 个 UTF-8 字节并清理路径字符与 Windows 保留名，防止元数据带入目录跳转；同名文件沿用原有编号避让，不覆盖。目标根仍需满足系统路径约束。

网易云从详情取得可信封面及专辑曲号；总曲数无可信字段则不写。5sing 使用已有歌词能力，不把上传者头像当作歌曲封面；酷我已接入原生行级歌词。本阶段不为缺失封面另发专辑查询。

`best_effort`（默认）下获取失败允许发布音频，结果列出 `METADATA_PARTIAL` 与 `COVER_FETCH_FAILED`／`LYRICS_FETCH_FAILED`；`required` 下获取／写入失败按错误可重试性重试或失败，失败不发布。平台没有提供可信封面、明确没有歌词或不支持歌词时不会使 required 失败。原文 LRC 保持不变，译文以空行、`[translation]`、译文 LRC 独立附加，不按行强行合并。

这些选项全部进入持久下载意图和复用指纹。schema 1 任务保留旧命名及基础标签行为，schema 2 的歌词变体固定为 lrc；非默认歌词变体使用 schema 3 快照。开启侧文件使用 schema 4；新程序兼容 schema 1～4，默认未导出且 lrc 不改变已有 schema 2 指纹。幂等重放仍返回最初任务，新的歌词选择不复用另一变体产物。旧程序不识别新 schema，回退时恢复对应版本的一致备份。

verbatim 生成增强 LRC：`[mm:ss.mmm]<mm:ss.mmm>词…<行结束时间>`。译文按精确毫秒时间匹配到行后，不按行号拼接；不能匹配的译文原样保留在独立 `[translation]` 段。romanized 使用其自身时间轴，译文仍独立。未提供所选变体时返回 `LYRICS_VARIANT_FALLBACK`；原始 LRC、翻译、罗马音和结构化逐字仍可从歌词 API 分别读取。

## NCM 本地导入与侧文件

普通下载完成后，默认音频处理器会根据文件头自动识别 NCM（不依赖 URL、扩展名或 Content-Type），用本项目 C++ 实现解密为原编码 MP3／FLAC，再校验、写标签和发布。该步骤运行在独立的 `processing` 线程池，默认 2 个工作线程、队列 16；HTTP 控制与搜索使用其他线程，取消通过任务的停止令牌传入，原生解密每 64 KiB 检查一次。处理池忙时任务等待，不能保证无限并发下载立即完成。

下载结果的 `processing.processor` 为 `audio.ncm`，`decrypt` 阶段完成；加密传输长度、明文结构与实际音质分别检查，未知加密格式或损坏内容不会作为成功音频发布。NCM 内嵌元数据补充平台未提供的字段，并遵守标签、封面和歌词开关；仅开启导出时不会因此强制内嵌。重启后重新解密未发布的工作副本。

单文件测试仍可使用以下离线命令；它等待该文件完成并打印结果，不是运行中服务的导入 API。

```powershell
# 停止使用同一 data_dir 的服务后执行；输入文件保留。
music-service --config config.json import-ncm --input "D:\音乐\歌曲.ncm" --principal prn_local --destination dst_default
```

`--metadata-policy` 支持 `best_effort`（默认）、`required`、`skip`，沿用音频标签语义。目标目录必须配置给该 principal 且接收新任务。NCM 只读指定本地文件，不扫描目录、不提供远程任意路径接口。输入上限取 `downloads.max_file_bytes` 与 2 GiB 的较小值，MP3／FLAC 按解密后的内容识别；不转码、不提升品质。文件内已有歌曲／专辑／歌手／歌词／封面用于标签和侧文件，不额外联网补全本地文件元数据。

开启导出示例：

```json
{"downloads":{"output":{"export_lyrics":true,"export_cover":true,"sidecars_required":false,"embed_lyrics":false,"embed_cover":false}}}
```

开启任一导出后，产物为 `歌手 - 歌名.files/歌手 - 歌名.mp3`（或 FLAC），同目录放同基名 `.lrc` 和 `.jpg`／`.png`；目录重名自动编号，整组不覆盖已有内容。未开启时仍保持原单音频路径。纯文本歌词保留原文写入 `.lrc`，不伪造时间戳；`lyrics_variant` 对下载侧文件同样有效。

可选内容不可用时记录 `metadata_fields.lyrics_file/cover_file=unavailable` 与 `SIDECAR_*_UNAVAILABLE`；写盘或发布错误使任务失败，不发布不完整文件。required 侧文件不可用则报 METADATA_FAILED。所有文件先在工作目录中写入、刷新，保存完整清单后单次移动目录，最后事务提交资产；恢复逐个核验 SHA-256。读文件与复用也检查整组文件，任一删除或改写即视为残缺。服务当前没有删除成品 HTTP 接口；本机删除时以 `.files` 目录为单位，历史保留。

命令输出 task_id、file_id 与相对路径。相同输入、账号归属、目标和选项下的完整成品可复用；Ctrl+C 在发布前取消，进程异常退出后尚未发布的本地任务标为 LOCAL_IMPORT_INTERRUPTED，需重新执行命令。已经保存发布意图的任务由启动恢复继续完成。离线命令期间没有在线 SSE，但启动后的 HTTP 任务及文件查询可见其结果。

文件描述的 `artifacts` 列出实际产物；在 `/media/v1/files/{file_id}` 或该文件的票据 URL 上加 `?part=lyrics`／`?part=cover` 取回侧文件，省略时返回音频。权限、范围请求和票据撤销沿用整个文件资产的规则；音源 asset 票据不接受 part。

## OAuth 应用配置与秘密

0.1.25 已接入通用流程，配置只用于实际实现 IOAuth2 的平台 profile。当前内置 9 个平台均未绑定 OAuth 工厂，不要给匿名平台添加虚构 profile；Jamendo 获取歌曲和下载无需此配置。下面的 `example_source` 是接入说明占位符。

先停止服务，将 client_secret 的精确字节放入源码树之外、仅当前用户可读的文件，再执行：

```powershell
music-service --config C:\music-service\config.json secret-import --input C:\private\client-secret.txt
```

命令持有实例锁，不覆盖已有秘密，只输出 `sec_...` 引用，不输出秘密内容，不自动删除输入文件，也不裁剪换行。将返回引用写入配置后再启动服务；未被凭证或配置引用的秘密会被启动清理。

```json
{
  "app_profiles": {
    "my_app": {
      "source": "example_source",
      "auth_profile": "web_oauth",
      "version": 1,
      "client_id": "your-client-id",
      "client_secret_ref": "sec_returned_by_import",
      "redirect_uri": "https://music.example/api/v1/oauth/callback/my_app"
    }
  },
  "providers": {"example_source": {"app_profile_id": "my_app"}}
}
```

redirect_uri 必须与平台登记值一致，路径固定为 `/api/v1/oauth/callback/{application_id}`，不带查询或片段；远程只允许 HTTPS，本机允许 `http://127.0.0.1:端口` 或 `http://[::1]:端口`。HTTPS 由现有反向代理转发控制入口，浏览器入口与回调需保持相同来源。不要在代理访问日志中记录这些路由的查询参数或 Cookie。普通 API 请求仍使用服务 Bearer，与平台的 access_token 相互独立。

应用 ID 只允许 1～64 个英文字母、数字、下划线、短横线。client_secret_ref 由 SecretStore 加载，配置文件不放明文 client_secret。变更应用身份时增加 version 并重新授权；原应用凭证不会使用新应用秘密尝试刷新。应用 client credentials token 缓存尚未接入。

本版本启动会将数据库迁移到 schema 3，并把上次中断的 pending 刷新标记为需要重新授权。升级前保留数据库及 secrets 配套备份；旧版本不能直接打开升级后的数据库。使用步骤见 [API 契约](../docs/design/cpp20-refactor/backend-service/api-contract.md#oauth-登录会话)。

## 启动前的组合检查

- 构建中没有的功能（媒体／事件入口、在线代理、直链、转码、外部工具、未编入的平台）不能启用。
- 已启用的监听不能端口冲突；端口 0 只允许 loopback；监听非 loopback 地址必须配置对应 `public_urls`，远程 http 地址需显式 `security.allow_insecure_remote`。
- 事件订阅与媒体响应的全局额度至少比各自 workers 少 2；每 principal 额度不超过全局额度。
- `catalog.default_sources` 必须是已配置且启用的平台，数量不超过 `max_sources_per_request`。
