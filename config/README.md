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
| 顶层 | `config_version`（只能为 1）、`data_dir`、`destinations`、`providers`、`app_profiles`（当前只接受空对象，T24 启用） |
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
| `lifetimes` | `asset_seconds`、`cursor_seconds`、`match_seconds`、`media_access_seconds` |
| `storage` | `writer_queue`、`read_connections`、`wait_budget_ms` |
| `cache` | `catalog_entries`、`catalog_bytes`、`catalog_ttl_seconds`、`failure_suppress_seconds`、`app_token_entries` |
| `handles` | `contexts`、`cursors`、`assets`、`matches`、`media_tickets`、`total_bytes` |
| `shutdown` | `soft_seconds` |
| `filesystem` | `failed_work_retention_hours` |
| `features` | `file_access`（未设置时跟随 media.enabled）、`media_proxy`、`direct_media_urls`、`automatic_fallback`、`transcoding` |
| `logging` | `format`（json／text）、`level`（trace／debug／info／warn／error） |
| `destinations[]` | `id`、`name`、`root`、`principals`、`accept_new_tasks` |

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
| `embed_cover`、`embed_lyrics` | 布尔值，独立控制嵌入；任务 `metadata_policy=skip` 时均不获取、不写入 |
| `cover_max_bytes` | 1～33554432 字节，默认 8 MiB；仅接收通过结构检查的 JPEG／PNG，不缩放、不转码 |
| `metadata_timeout_seconds` | 1～60 秒；每次封面或歌词获取预算，且不超过当前任务／尝试剩余期限 |

缺失歌手／专辑目录分别用 `未知歌手`／`未知专辑`；歌名为空时用 source-id。每段限制为 60 个 UTF-8 字节并清理路径字符与 Windows 保留名，防止元数据带入目录跳转；同名文件沿用原有编号避让，不覆盖。目标根仍需满足系统路径约束。

网易云从详情取得可信封面及专辑曲号；总曲数无可信字段则不写。5sing 使用已有歌词能力，不把上传者头像当作歌曲封面；酷我尚无歌词能力。本阶段不为缺失封面另发专辑查询。

`best_effort`（默认）下获取失败允许发布音频，结果列出 `METADATA_PARTIAL` 与 `COVER_FETCH_FAILED`／`LYRICS_FETCH_FAILED`；`required` 下获取／写入失败按错误可重试性重试或失败，失败不发布。平台没有提供可信封面、明确没有歌词或不支持歌词时不会使 required 失败。原文 LRC 保持不变，译文以空行、`[translation]`、译文 LRC 独立附加，不按行强行合并。

这些选项全部进入持久下载意图和复用指纹。升级前的 schema 1 任务保留旧命名及基础标签行为；新任务使用 schema 2 快照。幂等重放仍返回最初任务；新配置下的新请求不会复用旧配置产物。旧版程序不支持 schema 2 意图，回退程序前应恢复对应旧版备份。

## 启动前的组合检查

- 构建中没有的功能（媒体／事件入口、在线代理、直链、自动换源、转码、外部工具、未编入的平台）不能启用。
- 已启用的监听不能端口冲突；端口 0 只允许 loopback；监听非 loopback 地址必须配置对应 `public_urls`，远程 http 地址需显式 `security.allow_insecure_remote`。
- 事件订阅与媒体响应的全局额度至少比各自 workers 少 2；每 principal 额度不超过全局额度。
- `catalog.default_sources` 必须是已配置且启用的平台，数量不超过 `max_sources_per_request`。
