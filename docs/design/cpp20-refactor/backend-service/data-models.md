# 公共模型与存储设计

本文件定义[API](api-contract.md)与[任务运行时](runtime-and-tasks.md)共享的数据语义。内部领域对象不要求逐字段等同 JSON，但协议投影必须遵守这里的单位、身份和可见性约定。

## 基础类型

| 类型 | 约定 |
| --- | --- |
| 平台 `source` | 稳定的小写标识，如 `netease`、`qq`；显示名称可改变 |
| 平台 `id` | UTF-8 不透明字符串，长度 1～1024 字节；禁止转成整数后再序列化 |
| `TrackRef`／`CollectionRef` | `{source, id}`；平台身份不等于服务文件或任务身份 |
| 服务 ID | 服务生成的不透明随机 ID，带类型前缀，如 `tsk_`、`bat_`、`fil_`、`ast_`、`pbs_`、`cre_`、`dst_` |
| 时间 | UTC RFC 3339 字符串，示例 `2026-10-19T08:00:00.000Z` |
| 时长与耗时 | 整数毫秒，字段后缀 `_ms`；未知时为 null |
| 大小与速度 | 整数字节／字节每秒，后缀 `_bytes`／`_bps`；非负 JSON 整数限制在 0～9007199254740991 的精确范围内 |
| 比例 | 0～1；未知为 null，不把阶段完成比例等同文件完整性 |
| 缺省与 null | 请求缺省表示使用文档默认；响应 null 表示未知／不适用；空数组表示已知为空 |
| 枚举 | 字符串，新增值可能随功能扩展；客户端对未知值显示通用状态，不崩溃 |

平台 ID 通过查询参数或 JSON 传递，使用正常 URL 编码；路径中的 ID 仅使用服务生成的安全 ID，不要求前端把平台 ID 塞进路径。

## 平台与能力

`ProviderDescriptor` 包含 source、name、enabled、能力列表、品质提示、凭证需求和目录来源说明。支持程度与当前健康分开：`supported` 表示代码能力，`availability` 表示最近已知状态；不能用一次探测失败永久撤掉能力。

| 能力键 | 含义 |
| --- | --- |
| `search_tracks`、`track_detail` | 单曲搜索与元信息 |
| `search_playlists`、`playlist_detail`、`playlist_tracks` | 歌单搜索、信息、分页曲目 |
| `search_albums`、`album_detail`、`album_tracks` | 专辑能力 |
| `search_artists`、`artist_tracks`、`artist_albums` | 歌手发现与作品 |
| `categories`、`category_playlists` | 分类目录与分类歌单 |
| `charts`、`chart_detail`、`chart_tracks` | 榜单目录、信息与曲目 |
| `recommend_playlists` | 推荐歌单 |
| `resolve_audio`、`lyrics`、`verify_credentials` | 音源、歌词与凭证校验 |

每项能力说明 `supported`、`requires_credentials`、`pagination`，需要时补充 `catalog_kind`：`live`、`built_in`、`search_derived`。品质提示是平台实现可能提供的档位，不承诺当前账号和每首歌曲可用。

## 歌曲与集合

| 模型 | 字段 |
| --- | --- |
| `Track` | ref、title、artists、album、duration_ms、version、artwork、context_ref、links |
| `ArtistSummary` | name，ref 可为 null；多个艺术家保留数组，不只拼成展示字符串 |
| `AlbumSummary` | title、ref，可为空；保留与歌单不同的语义 |
| `VersionInfo` | kind：studio／live／remix／cover／instrumental／unknown，label、evidence |
| `Playlist` | ref、name、creator、description、artwork、track_count、play_count、updated_at |
| `Album` | ref、title、artists、release_date、track_count、artwork |
| `Category` | source、id、name、group、catalog_kind |
| `Chart` | ref、name、description、update_hint、updated_at、track_count、catalog_kind |

缩略图只暴露可公开访问的展示 URL 或受授权的本地 asset 链接。需要秘密请求头的封面不直接塞给浏览器；按媒体资源规则缓存或代理。`links` 是服务自身的资源链接，不包含带账号秘密的上游 URL。

`context_ref` 是平台 hash、资源参数等附加上下文的服务端句柄，按 principal、source、实体与有效期绑定。客户端可以原样回传，不能自行构造平台扩展 JSON。句柄过期时先按 ref 重新取详情；只有确实无法恢复所需上下文时返回 `422 CONTEXT_REQUIRED`。下载被接收前，将必要的非秘密上下文保存到任务私有快照，不能让排队任务依赖一条稍后过期的缓存。

## 品质、解析和探测

| 字段 | 约定 |
| --- | --- |
| `quality` | `standard`、`high`、`lossless`、`hires`；默认 standard |
| `quality_policy` | `strict` 或 `allow_lower`，默认 strict |
| `allow_preview` | 默认 false；为 true 时允许明确的试听资源，但结果仍须标记 preview |
| `fallback.mode` | `disabled` 或 `auto`，默认 disabled；手动换源通过候选接口选择实体 |
| `container` | mp3、flac、m4a、ogg 等，未知为 null |
| 音频属性 | codec、bitrate_bps、sample_rate_hz、channels、bit_depth，均允许未知 |
| `completeness` | full／preview／unknown；不能仅因为拿到 URL 就填 full |

`ResolvedAsset` 内部保存 URL、必需请求头、解密信息、实际来源、有效期和资源指纹。公开 `AssetDescriptor` 只返回 asset_id、原曲／实际曲目、品质属性、access_state、completeness、expires_at、warnings 与证据摘要。

资源描述与上游地址可能过期。公开 asset_id 默认最多有效 5 分钟，不能晚于已知上游有效期减安全余量；过期返回 `410 RESOURCE_EXPIRED`。下载任务执行时自行解析，不要求客户端先取得 asset_id。播放会话也在必要时重新解析，不持久保存签名 URL 当成歌曲身份。

探测报告分开表达 HTTP 可访问性、格式迹象、大小、时长证据及整曲判断。未知完整性的资源不因 `allow_preview=false` 被假装认证为整曲：可按现有证据继续下载并保留 unknown 警告；若已明确为 preview，则按选项拒绝。要求严格整曲验证的扩展策略另行实现，首版不承诺完整解码证明。

## 分页与部分成功

`PageInfo`：limit、returned、total、has_more、next_cursor。total 与 has_more 可以为 null；next_cursor 不存在时为 null。next_cursor 是服务生成的游标，绑定 principal、source、操作、关键词／过滤条件及凭证版本，默认有效 10 分钟。

第一次搜索允许多个 source；返回平铺 items 以及逐源 `SourceResult` 元信息，每个来源有自己的 PageInfo。**没有跨平台统一下一页或准确总数**。继续某一来源时，带该来源和它的 next_cursor；cursor 请求仅允许一个来源，不再同时带 page。页面不能用一次全平台结果的条数推断全平台已到末页。

集合曲目和分类歌单使用同一游标约定。底层页码／偏移由 Provider 转换，服务不暴露 SDK 私有游标给客户端解释。游标失效返回 `410 CURSOR_EXPIRED`，来源或查询不匹配返回 `400 CURSOR_MISMATCH`。

`SourceResult` 包含 source、status（ok／failed／unsupported／disabled）、elapsed_ms、page、error、cache。ok 且 returned=0 才是已知无结果；超时必须为 failed。混合结果使用 HTTP 200，meta.partial=true；所有已执行来源失败时按 API 错误规则返回，仍附逐源明细。

## 下载任务快照

| 字段组 | 字段与语义 |
| --- | --- |
| 身份 | task_id、owner_id（普通查询不返回其他用户）、retry_of、version；批次关系由 batch_items 表维护 |
| 请求 | original_track、context_snapshot、credential_id、requested_quality、quality_policy、allow_preview、fallback、可选 match_selection、destination_id、metadata_policy |
| 实际执行 | actual_track、actual_quality、container、completeness、attempt、stage、warnings |
| 状态 | queued／resolving／probing／downloading／processing／finalizing／waiting_retry／canceling／completed／failed／canceled |
| 进度 | received_bytes、total_bytes、speed_bps、eta_ms、stage_progress；未知为 null |
| 时刻 | created_at、updated_at、started_at、finished_at、next_retry_at |
| 终态 | result.file_id、result.metadata_status、error、cancellation_requested_at |

phase 用 status 表达，`stage` 用于 processing 内部的 decrypt／validate／tags 等细分，不能另维护一套冲突的状态。`version` 每次持久状态转换递增，供并发更新与客户端刷新使用。

公开进度检查点提交也递增 version；HTTP 与 SSE 使用同一版本。每次领取额外生成私有 execution_id，状态写入同时检查执行世代与预期版本，旧尝试的迟到回调不能覆盖新执行。execution_id、租约和秘密引用不在普通 Task 投影中返回。续传进度包含已确认的既有字节；重新下载时进度可重置，不保证跨尝试单调递增。

`metadata_policy`：`best_effort`（默认）、`required`、`skip`。best_effort 允许标签失败而音频 completed，metadata_status 为 partial／failed 并列出 warnings；required 失败则不发布最终资产；skip 明确不写标签。

`attempt` 统计任务级资源解析与传输尝试，默认上限 3 次，包含首次；单次传输内部网络重试另计、默认最多额外 2 次。两层同时受任务时间预算约束，避免形成无限乘法重试。具体边界见[运行时](runtime-and-tasks.md)。

### 幂等与下载去重

幂等记录以 principal、方法、路径、Idempotency-Key 为键，保存原始请求体规范化哈希、首次提交结果与创建时间，至少保留 24 小时。同键同请求重放原结果；同键不同请求返回 409。哈希基于请求输入，而非重试时可能已变化的配置默认值；首次执行的配置快照随记录保存。

下载指纹另外包含 owner、歌曲 ref、所选 credential_id、品质及降级／换源策略、目标目录、命名与元数据配置版本。默认 duplicate_policy=reuse：命中活动任务或文件仍存在的已完成任务，返回其 task_id；允许强制重下已完成文件，但相同活动指纹仍返回 409，避免重复工作。

不同任务可能得到相同展示文件名，输出路径必须单独预留并自动加确定性后缀；不覆盖另一个任务的临时文件。终态任务历史至少在首版保留，暂不开放删除任务接口；后续清理策略不能破坏文件资产索引。

## 批次、文件、播放与媒体票据

| 模型 | 关键规则 |
| --- | --- |
| `DownloadBatch` | batch_id、有序 task_ids、创建计数与复用计数、状态统计；items 最多 100，创建事务全有或全无，执行结果各自独立 |
| `FileAsset` | file_id、owner、track、实际品质、相对目标路径、大小、容器、ETag、available／missing、创建任务；对外不返回服务器绝对路径 |
| `PlaybackSession` | session_id、owner、原曲与固定实际曲目、品质、expires_at；默认 30 分钟绝对有效期，不是持久下载任务 |
| `MediaTicket` | 随机高熵 token 的哈希、owner、目标 kind/id、到期时刻；默认 60 秒，可申请 15～300 秒 |
| `Destination` | destination_id、名称、可用性与允许的 principal；实际根目录由管理配置定义 |
| `CredentialDescriptor` | credential_id、source、label、version、状态、验证时间；不包含 Cookie 内容 |

媒体票据在新请求入场时检查到期，允许有效期内多次 Range 请求，不是一次性票据。入场成功的单次传输可继续到完成，但受传输时间／空闲限制及会话撤销约束。票据不得超过播放会话有效期；播放会话到期不延长，新建会话明确重新选择资源。

在线播放确定实际录音后不再自动换到另一版本；刷新过期地址必须验证资源身份。文件资产发布后视为不可变；需要重写标签时生成新资产，不在客户端 Range 读取期间原地改变文件。

## SQLite 表设计

| 表 | 核心列与索引 |
| --- | --- |
| `schema_migrations` | version、checksum、applied_at；与 HTTP API 版本独立 |
| `principals` | id、display_name、role、enabled |
| `access_tokens` | token_id、principal_id、token_hash、scopes、revoked_at；不存可直接使用的明文 token |
| `credentials` | id、owner_id、source、secret_ref、version、status；秘密由 SecretStore 管理 |
| `download_tasks` | id、owner_id、status、version、execution_id、指纹、请求／执行快照、租约、重试时刻；索引 owner/status/created_at/id 与 status/next_retry_at |
| `download_batches`、`batch_items` | 批次信息与有序 task_id，支持一批里复用既有任务 |
| `task_attempts` | task_id、attempt_no、起止时刻、错误、资源证据；不保存敏感 URL |
| `file_assets` | file_id、owner_id、destination_id、相对路径、属性、ETag、状态；路径预留唯一约束 |
| `task_files` | task_id、file_id、关系；允许复用资产但不混淆创建任务 |
| `idempotency_records` | owner/method/path/key 唯一、request_hash、响应快照、expires_at |
| `finalization_intents` | task_id、临时路径标识、预留目标、校验证据、阶段，供崩溃恢复 |

批次与任务为多对多关系；同一批次也可按输入顺序出现重复 task_id。batch_items 使用 batch_id + ordinal 唯一约束，批次统计按条目计算，取消按唯一任务执行。任务表不以单个 batch_id 表达所属批次；列表 batch_id 过滤通过关系表查询。

活动任务指纹、幂等 key、尝试号、默认凭证和发布路径的唯一性，以及事务与崩溃矩阵见[内部契约](internal-contracts.md)。首版任务历史保留，短期 SSE 通知不落为独立事件历史。

解析缓存、游标状态、播放会话和媒体票据首版可仅保存在有上限的内存中；进程重启后失效，客户端按 410 重新建立。下载任务与批次必须落库，不能用同样的内存策略。

单进程持有 data_dir 实例锁；SQLite 采用 WAL、单写入执行器和少量读取连接。文件系统操作不在长数据库事务内执行，网络请求也不持数据库锁。所有表操作通过 repository，业务用例不散落 SQL。

## 文件与数据库的协调

文件重命名和 SQLite 提交不能组成同一个原子事务。任务发布流程记录 finalization_intent、完成同文件系统重命名、再事务写入 file_asset 和 completed；启动恢复根据 intent、实际文件和校验证据补齐或标为需重新执行。

文件强 ETag 从最终发布字节生成并存储，标签处理后再计算，读取接口不因一次请求临时重写文件。任务轮询使用弱版本 ETag，二者不能混用于 If-Range。

普通查询发现文件被用户移走时，将 asset 标为 missing，文件获取返回 410；历史仍保留。DB 成功但文件丢失、文件存在但 DB 尚未提交、取消与发布竞争都必须按此协调流程处理，不能靠“最后写一次 done”掩盖不一致。
