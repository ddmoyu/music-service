# 配置资源

配置由 `infrastructure/config` 加载：内置缺省 → JSON 文件 → 已列明的 `MUSIC_DL_*` 环境变量 → 命令行 `--config`／`--data-dir`。默认值的规范位置是[配置与部署](../../../docs/design/cpp20-refactor/backend-service/configuration-and-deployment.md#配置来源与生效时机)，本页只列 JSON 键名。未知键、类型错误和越界值都会让启动失败，一次报告全部问题，字段路径形如 `/control/port`。

真实 token、Cookie、API key、OAuth 秘密和本机目录留在源码树之外。

## 路径规则

- 文件中的相对路径相对于配置文件所在目录；环境变量和命令行给出的相对路径相对于进程工作目录。
- 未指定 data_dir 时为 `<配置文件目录或工作目录>/data`。
- `MUSIC_DL_DOWNLOAD_DIR` 只替换 id 为 `dst_default` 的目标根目录，未配置该目标时报错。

## 键名

时长与大小的单位写在键名后缀里：`_ms`、`_seconds`、`_hours`、`_bytes`。

| 组 | 键 |
| --- | --- |
| 顶层 | `config_version`（只能为 1）、`data_dir`、`destinations`、`providers`、`app_profiles`（当前只接受空对象，T24 启用） |
| `control` | `host`、`port`、`workers`、`connection_queue` |
| `media`、`events` | `enabled`、`host`、`port`、`workers`、`connection_queue` |
| `event_limits` | `global_subscriptions`、`per_principal_subscriptions`、`max_queued_events`、`max_queued_bytes`、`max_event_bytes` |
| `event_stream` | `progress_interval_ms`、`heartbeat_seconds`、`write_idle_seconds`、`max_connection_seconds` |
| `public_urls` | `api_base`、`media_base`、`events_base` |
| `security` | `allow_insecure_remote` |
| `http` | `body_limit_bytes`、`read_idle_seconds`、`write_idle_seconds`、`keep_alive_seconds`、`max_requests_per_connection` |
| `cors` | `allowed_origins` |
| `auth` | `mode`（只能为 bearer） |
| `admission` | `global`、`per_principal` |
| `upstream` | `short_workers`、`queue`、`per_source_short`、`max_total_connections`、`max_response_bytes`、`redirect_max`、`connect_timeout_seconds` |
| `catalog` | `total_budget_ms`、`max_budget_ms`、`per_source_budget_ms`、`fanout`、`max_sources_per_request`、`default_sources` |
| `providers.<source>` | `enabled`、`implementation`（native／external）、`options`（对象，由平台包校验）、`app_profile_id`、`rate_limit_group` |
| `external_adapters` | `enabled`、`max_processes` |
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
| `features` | `file_access`、`media_proxy`、`direct_media_urls`、`automatic_fallback`、`transcoding` |
| `logging` | `format`（json／text）、`level`（trace／debug／info／warn／error） |
| `destinations[]` | `id`、`name`、`root`、`principals`、`accept_new_tasks` |

## 启动前的组合检查

- 构建中没有的功能（媒体／事件入口、在线代理、直链、自动换源、转码、外部工具、未编入的平台）不能启用。
- 已启用的监听不能端口冲突；端口 0 只允许 loopback；监听非 loopback 地址必须配置对应 `public_urls`，远程 http 地址需显式 `security.allow_insecure_remote`。
- 事件订阅与媒体响应的全局额度至少比各自 workers 少 2；每 principal 额度不超过全局额度。
- `catalog.default_sources` 必须是已配置且启用的平台，数量不超过 `max_sources_per_request`。
