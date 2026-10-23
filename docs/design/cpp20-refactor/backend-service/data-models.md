# 公共模型与存储设计

[API](api-contract.md)与[运行时](runtime-and-tasks.md)共用以下字段语义；内部对象可不同于 JSON 投影。

## 基础类型

| 类型 | 约定 |
| --- | --- |
| 平台 `source` | 稳定字符串，格式 `[a-z][a-z0-9_]{0,63}`，如 `kuwo`、`netease`；不使用封闭平台枚举，显示名称可改变 |
| 平台 `id` | UTF-8 不透明字符串，长度 1～1024 字节；禁止转成整数后再序列化 |
| `TrackRef`／`CollectionRef` | `{source, id}`；平台身份不等于服务文件或任务身份 |
| 服务 ID | 服务生成的不透明随机 ID，带类型前缀，如 `tsk_`、`bat_`、`fil_`、`ast_`、`cre_`、`dst_` |
| 时间 | UTC RFC 3339 字符串，示例 `2026-10-19T08:00:00.000Z` |
| 时长与耗时 | 整数毫秒，字段后缀 `_ms`；未知时为 null |
| 大小 | 整数字节，后缀 `_bytes`；非负 JSON 整数限制在 0～9007199254740991 的精确范围内 |
| 传输速度 | `speed_bytes_per_second`，单位 byte/s；未知为 null |
| 音频码率 | `bitrate_bps`，单位 bit/s；例如 320000 表示 320 kbit/s，与传输速度相差一个字节到比特的换算，不共用单位约定 |
| 比例 | 0～1；未知为 null，不把阶段完成比例等同文件完整性 |
| 缺省与 null | 请求缺省表示使用文档默认；响应 null 表示未知／不适用；空数组表示已知为空 |
| 枚举 | 字符串，新增值可能随功能扩展；客户端对未知值显示通用状态，不崩溃 |

平台 ID 通过查询参数或 JSON 传递，使用正常 URL 编码；路径中的 ID 仅使用服务生成的安全 ID，不要求前端把平台 ID 塞进路径。

## 平台与能力

`ProviderDescriptor` 包含 source、name、enabled、implementation_id／version、execution_mode（固定为 native）、能力列表、auth_methods、品质提示、目录来源说明，以及内部使用的明文 HTTP 端点登记（不对外返回）。支持程度与当前健康分开：`supported` 表示代码能力，`availability` 表示最近已知状态；不能用一次探测失败永久撤掉能力。注册和扩展规则见[多平台适配](provider-extensibility.md)。

| 能力键 | 含义 |
| --- | --- |
| `search_tracks`、`track_detail` | 单曲搜索与元信息 |
| `search_playlists`、`playlist_detail`、`playlist_tracks` | 歌单搜索、信息、分页曲目 |
| `search_albums`、`album_detail`、`album_tracks` | 专辑能力 |
| `search_artists`、`artist_detail`、`artist_tracks`、`artist_albums` | 歌手发现、资料与作品 |
| `categories`、`category_playlists` | 分类目录与分类歌单 |
| `charts`、`chart_detail`、`chart_tracks` | 榜单目录、信息与曲目 |
| `recommend_playlists` | 推荐歌单 |
| `parse_links` | 已知域名／URI 到平台实体的解析；不等于支持搜索或取流 |
| `resolve_audio`、`lyrics`、`verify_credentials` | 音源、歌词与凭证校验 |

每项能力说明 `supported`、`enabled`、`requires_credentials`、`pagination`、`requirements`（凭证 kind／auth_profile／scope、地区和依赖），以及 `availability`（available／unavailable／unknown、reason、checked_at）；需要时补充 `catalog_kind`：`live`、`built_in`、`search_derived`。品质提示是平台实现可能提供的档位，不承诺当前账号和每首歌曲可用。availability 和凭证状态按当前 principal 投影，不输出其他用户账号信息；读取列表不自动探测所有平台。

auth_methods 只描述已实现的 kind、auth_profile、输入字段 schema、scope 及 supports_verify／supports_refresh／supports_application_token，不包含秘密。声明必须有对应授权工厂绑定，不能由 kind=oauth2 推断支持刷新；注册契约见[授权扩展](provider-extensibility.md#6-凭证与账号)。支持程度以能力为单位，目录型来源可声明 resolve_audio=false。新增 source 不增加 SQLite 平台专表或公共路由。

## 账号绑定与授权身份

账号选择由 CredentialService 完成，业务代码和适配器不各自拼接身份键。

| 类型 | 字段与语义 |
| --- | --- |
| `AccountBinding` | principal、source、mode（anonymous／application／user）、auth_profile、credential_id（用户模式必需）、app_profile_id（使用应用身份时必需）；其余不适用项明确为 null |
| `AuthIdentityKey` | AccountBinding + credential_version、app_profile_version、app_token_id／app_token_version；包含本次操作实际使用的全部授权引用与版本，不适用项为 null |
| `CredentialLease` | AuthIdentityKey 与只读秘密快照；仅供当前操作使用，不放入公开 DTO 或持久任务输入 |

AccountBinding 表达稳定的账号选择，不含 token、Cookie、secret_ref 或短期版本。AuthIdentityKey 区分同一账号的授权快照；两个 credential_id 即使 version 相同也不相等。用户 OAuth 同时依赖应用配置时保留两种身份引用，不能丢掉其中一项。anonymous 使用明确的匿名 profile，不能与 application／user 共用空字符串键。

秘密、有效 scope 或其他授权条件变更必须推进对应版本；应用配置版本随管理配置变更，应用 token 版本随成功获取／刷新变更。不同 scope 的应用 token 缓存条目分配不同 app_token_id，重建已淘汰条目时使用新 ID，不能因两个条目都是 version=1 而混用。凭证和应用 profile 标识不可回收给另一条记录。即使使用共享应用身份，用户数据缓存仍保留 principal 分区；配额组另有稳定标识，不随 token 刷新重置。

创建下载时将各实际可用来源的默认选择解析为 AccountBinding 并保存；自动换源只使用提交时固定的有限候选来源与账号绑定。执行时可租用同一绑定的最新有效版本，不改用另一默认账号，也不把原来源凭证交给目标来源。账号撤销或权限不足按现有错误规则处理；应用模式与匿名模式必须由该能力明确允许。

默认用户凭证的唯一事实来源是 SQLite 的 `credential_defaults`，按 owner + source 保存 credential_id（可为 null）与单调递增的 selection_version。配置文件不保存另一份用户默认账号；设置与清除使用[默认凭证 API](api-contract.md#111-默认平台凭证)。新增凭证不自动成为默认，清除或撤销默认凭证也不自动改选另一用户账号；没有默认时仅可按能力使用 application／anonymous，否则返回 CREDENTIALS_REQUIRED。

默认选择提交后立即影响新请求。使用隐式默认选择的分页／匹配句柄还绑定 selection_version；即使默认账号从 A 改到 B 再改回 A，旧选择句柄仍失效。缓存内容继续按 AuthIdentityKey 分区，selection_version 不加入稳定 AccountBinding 或下载指纹；已受理任务使用原绑定。撤销凭证与清空其默认引用在同一事务提交。

## 歌曲与集合

| 模型 | 字段 |
| --- | --- |
| `Track` | ref、title、artists、album、duration_ms、version、artwork、track_number、track_total、context_ref、links、可选 external_ids（如 isrc，附证据来源） |
| `Artist` | ref、name、artwork；可选 description、alternate_name、country、birth_date、birthplace、gender、language、height、weight、constellation；可选非负计数 track_count、album_count、mv_count；未知字段为 null |
| `ArtistSummary` | name，ref 可为 null；多个艺术家保留数组，不只拼成展示字符串 |
| `AlbumSummary` | title、ref，可为空；保留与歌单不同的语义 |
| `VersionInfo` | kind：studio／live／remix／cover／instrumental／unknown，label、evidence |
| `Playlist` | ref、name、creator、description、artwork、track_count、play_count、updated_at、可选 relationship（created／subscribed） |
| `Podcast` | ref（电台 ID）、title、presenter、description、artwork、episode_count |
| `Episode` | ref（节目 ID）、podcast（电台引用）、title、podcast_title、presenter、description、artwork、duration_ms、published_at、audio_ref、requires_purchase |
| `TrackLyrics` | lrc、translated_lrc、romanized_lrc 独立文本；karaoke 为毫秒单位行／词；unavailable、warnings |
| `Album` | ref、title、artists、release_date、track_count、artwork、可选 description |
| `Category` | source、id、name、group、catalog_kind |
| `Chart` | ref、name、description、update_hint、updated_at、track_count、catalog_kind |

缩略图首版只暴露可公开访问的展示 URL；需要秘密头的封面在缺少专用图片代理时返回 null／警告，不复用音频 asset 路由伪装支持。`links` 是服务自身的资源链接，不包含带账号秘密的上游 URL。

`context_ref` 是平台 hash、资源参数等附加上下文的服务端句柄，按 AuthIdentityKey、实体、实现／schema 版本与有效期绑定。客户端可以原样回传，不能自行构造平台扩展 JSON。句柄过期或授权身份不匹配时不能复用其私有数据，先按 ref 在当前授权下重新取详情；只有确实无法恢复所需上下文时返回 `422 CONTEXT_REQUIRED`。下载被接收前，将必要的非秘密上下文保存到任务私有快照，记录生成它的授权身份和 schema；执行时版本改变则重新获取或由平台明确声明可安全迁移，不能盲用旧账号上下文。

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

`ResolvedAsset` 内部保存带类型的传输计划（首版 HttpAsset；新增传输形态由服务自己实现）、必需请求条件、处理计划、实际来源、有效期和资源指纹。HttpAsset 包含 URL 与秘密请求头；处理计划通过 processor_id、版本和私有上下文绑定。公开 `AssetDescriptor` 只返回 asset_id、原曲／实际曲目、品质属性、access_state、completeness、expires_at、warnings 与匹配／资源证据摘要，不返回命令或私有处理参数。

目录源与音频源可以不同，例如模拟组合 original_track.source=mock_catalog、actual_track.source=mock_audio；实现工具不作为 source。外部提取／传输不会改变实际音频来源或凭空提升品质。缺少可直接转发的 HttpAsset 时不提供在线代理或直链，可在下载完成后访问 file_id。

内部另保存平台可验证的传输条件：是否允许外发直链、必需请求头、是否需解密／处理、资源身份校验信息。条件未知不视为可直连；MediaAccessService 只读这些条件并叠加部署策略，不在访问接口中重新实现平台解析。

资源描述与上游地址可能过期。公开 asset_id 按 lifetimes 配置有效期，不能晚于已知上游有效期减安全余量；过期返回 `410 RESOURCE_EXPIRED`。下载任务执行时自行解析，不要求客户端先取得 asset_id。在线媒体读取复用 asset_id，不建立另一份播放会话；过期重新解析由客户端发起，播放位置仍由客户端保存。

媒体访问描述是临时响应，不是独立持久实体。包含 target、delivery、url、auth、expires_at、content_type、range_supported 和实际曲目／品质摘要。默认 AssetDescriptor 不公开 CDN URL；只有 media-access 在显式策略允许时可返回经筛选的限时直链。签名参数作为敏感访问授权处理，不与公共歌曲数据混存。

探测报告分开表达 HTTP 可访问性、格式迹象、大小、时长证据及整曲判断。未知完整性的资源不因 `allow_preview=false` 被假装认证为整曲：可按现有证据继续下载并保留 unknown 警告；若已明确为 preview，则按选项拒绝。要求严格整曲验证的扩展策略另行实现，首版不承诺完整解码证明。

品质判定同理：已知低于请求品质时，strict 拒绝（QUALITY_UNAVAILABLE），allow_lower 接受并附 QUALITY_LOWERED；实际品质未知时接受并附 QUALITY_UNVERIFIED，不当作已满足。选择品质档位时只在请求档位不可用且 allow_lower 时取较低档中最高者，从不自动升到更高档位。

## 分页与部分成功

`PageInfo`：limit、returned、total、has_more、next_cursor。total 与 has_more 可以为 null；next_cursor 不存在时为 null。next_cursor 是服务生成的游标，绑定完整 AuthIdentityKey、操作、关键词／过滤条件、实现版本及使用默认选择时的 selection_version，有效期使用 lifetimes 配置。后续页按当前选择的账号建立身份键后核对；默认账号切换、凭证撤销或授权版本变化使旧游标返回 `410 CURSOR_EXPIRED`，不能把旧页令牌交给新账号。

第一次搜索允许多个 source；返回平铺 items 以及逐源 `SourceResult` 元信息，每个来源有自己的 PageInfo。**没有跨平台统一下一页或准确总数**。继续某一来源时，带该来源和它的 next_cursor；cursor 请求仅允许一个来源，不再同时带 page。页面不能用一次全平台结果的条数推断全平台已到末页。

集合曲目和分类歌单使用同一游标约定。底层页码／偏移由 Provider 转换，服务不暴露 SDK 私有游标给客户端解释。游标失效返回 `410 CURSOR_EXPIRED`，来源或查询不匹配返回 `400 CURSOR_MISMATCH`。

0.1.2 集合分页增加 `listed_total`（可见清单长度）和 `unavailable_count`（本页有 ID 但缺详情的位置数），未知为 null；`total` 保留上游声明值。分页按原列表位置推进，缺详情不改变后续偏移；重复引用保留为重复位置。可检测到曲目顺序／数量／版本变更时使旧游标失效，服务页缓存和上游响应不构成原子快照。

`SourceResult` 包含 source、status（ok／failed／unsupported／disabled）、elapsed_ms、page、error、cache。ok 且 returned=0 才是已知无结果；超时必须为 failed。混合结果使用 HTTP 200，meta.partial=true；所有已执行来源失败时按 API 错误规则返回，仍附逐源明细。

## 下载任务快照

| 字段组 | 字段与语义 |
| --- | --- |
| 身份 | task_id、owner_id（普通查询不返回其他用户）、retry_of、version；批次关系由 batch_items 表维护 |
| 请求 | original_track、requested_quality、quality_policy、allow_preview、fallback、destination_id、metadata_policy；私有 normalized_intent 保存 AccountBinding、已验证手动目标与配置快照，context_snapshot／context_schema_version 保存必要的非秘密上下文 |
| 实际执行 | actual_track、actual_quality、container、completeness、attempt、stage、warnings、匹配证据摘要；私有快照另存 implementation_id／version 与处理计划版本 |
| 状态 | queued／resolving／probing／downloading／processing／finalizing／waiting_retry／canceling／completed／failed／canceled |
| 进度 | received_bytes、total_bytes、speed_bytes_per_second、eta_ms、stage_progress；未知为 null |
| 时刻 | created_at、updated_at、started_at、finished_at、next_retry_at |
| 等待 | wait_reason 在 waiting_retry 时为 admission／upstream_retry，其余为 null；私有 resume_stage 与 attempt_open 表达暂停的工作位置及是否沿用本次逻辑尝试 |
| 终态 | result.file_id、result.metadata_status、error、cancellation_requested_at |

phase 用 status 表达，`stage` 用于 processing 内部的 decrypt／validate／tags 等细分，不能另维护一套冲突的状态。`version` 每次持久状态转换递增，供并发更新与客户端刷新使用。

公开进度检查点提交也递增 version；HTTP 与 SSE 使用同一版本。每次领取额外生成私有 execution_id，状态写入同时检查执行世代与预期版本，旧尝试的迟到回调不能覆盖新执行。execution_id、租约和秘密引用不在普通 Task 投影中返回。续传进度包含已确认的既有字节；重新下载时进度可重置，不保证跨尝试单调递增。

`metadata_policy`：`best_effort`（默认）、`required`、`skip`。best_effort 允许标签失败而音频 completed，metadata_status 为 partial／failed 并列出 warnings；required 失败则不发布最终资产；skip 明确不写标签。

0.1.1 的新任务默认获取可用封面和歌词。`result.metadata_fields` 为字段到状态的对象：`written`、`unavailable`（平台未提供可信字段或明确无歌词）、`unsupported`（无能力）、`disabled`、`fetch_failed`、`write_failed`。当前报告字段为 title、artist、album、cover、lyrics、track_number、track_total；未提供的基础标签／曲号可以省略，客户端须兼容未知新字段。`required` 要求可提供的启用字段成功获取和写入，不把缺失／不支持当失败；获取失败不能冒充缺失。部分获取失败使 `metadata_status=partial`，警告包含 METADATA_PARTIAL 及字段原因。每项图片／歌词获取沿用任务取消、绝对期限和公共入场；子请求超时可在 best_effort 下继续，不会重置任务预算。

`track_number`、`track_total` 为正整数或 null，来自专辑元数据而非歌单位置；未知不生成 0/0。曲号与总数写入 MP3／FLAC／M4A；无曲号时不单独写总数。配置取值、原文／译文规则与升级兼容见 [输出配置](../../../../config/README.md#下载文件输出011)。

`attempt` 统计已开始的任务级资源解析与传输尝试，排队时为 0，上限使用 downloads.max_attempts，包含首次。领取产生 execution_id，但不自动消耗 attempt；首次获准执行该逻辑尝试时持久递增，admission 等待后的再领取沿用该 attempt。只有 RetryPolicy 判定结束旧尝试并开始新的上游重试时才再递增。单次传输内部网络重试另计，使用 downloads.transport_retries；任务绝对时间预算自首次逻辑尝试开始计时，此后的所有等待和重试均受其约束。具体边界见[运行时](runtime-and-tasks.md)。

### 幂等与下载去重

幂等记录以 principal、方法、路径、Idempotency-Key 为键，保存原始请求体规范化哈希、首次提交结果与创建时间，保留期使用配置 idempotency（默认至少 24 小时）。同键同请求重放原结果；同键不同请求返回 409。哈希基于请求输入，而非重试时可能已变化的配置默认值；首次执行的配置快照随记录保存。通过当前身份与 scope 校验后，先检查已有幂等记录，再读取临时上下文或重新选择默认账号；已提交请求不会因 match/context 过期而无法重放。事务内再次处理幂等竞争。

下载指纹基于持久 `NormalizedDownloadIntent`，由 application/tasks 补全默认、校验授权和选择后交给领域纯规则生成。指纹带 schema_version；规范字段以带长度前缀的固定规则编码（字段值中的分隔符不会产生歧义），摘要为该编码的 64 位 FNV-1a 十六进制值，仅用于索引。命中摘要后仍核对完整规范编码，不能只凭哈希认定相同。

| 指纹内容 | 归一规则 |
| --- | --- |
| owner 与 original_track | source + 不透明 id 保持原值，不对平台 ID 大小写折叠 |
| 音源选择 | 同源、手动目标或 auto 三种；手动保存验证后的目标 TrackRef；auto 保存有序候选来源及匹配策略版本，不以尚未确定的 actual_track 代替策略 |
| 账号 | 原曲及手动／auto 目标所需的 AccountBinding；来源映射按稳定键编码，默认选择在提交时固定 |
| 音质与内容 | requested_quality、quality_policy、allow_preview |
| 产物 | metadata_policy、destination_id 及目标配置版本、命名与元数据配置版本；包含封面／歌词等会影响产物的有效配置 |

auto 来源顺序有语义，保留首次出现顺序去重并移除原曲来源，去除后至少保留一个候选；关闭 auto 时归一为同源。手动目标与原曲相同时归一为同源。账号绑定只保留所选音源实际需要的来源，按来源排序，同一来源出现不同绑定即拒绝。客户端省略与显式提交相同默认值生成同一下载意图，但幂等请求哈希仍按各自输入判定。手动候选保存目标、证据摘要、必要上下文及验证版本；排队、恢复和 retry 不依赖短期 match_id／candidate_id。临时句柄、token／URL、授权版本、request_id、task_id、时间和 duplicate_policy 不进入下载指纹；凭证刷新不会单独制造一个新下载意图。

默认 duplicate_policy=reuse：只复用规范化意图相同的活动任务或合格的已完成结果，不跨不同策略“猜测兼容”。完成结果还要满足当前文件访问权限、文件存在性、原曲／手动目标、品质／试听限制与 metadata_policy；best_effort 可复用带相同报告的部分结果，required 不能复用未满足必需产物的文件。检查文件和结果证据后，在提交事务内重验相关版本；不合格完成结果不复用，正常创建新任务。force 可绕过完成结果，但相同活动意图仍返回 409 ACTIVE_DUPLICATE。

不同任务可能得到相同展示文件名，输出路径必须单独预留并自动加确定性后缀；不覆盖另一个任务的临时文件。终态任务历史至少在首版保留，暂不开放删除任务接口；后续清理策略不能破坏文件资产索引。

## 批次、文件、媒体访问与票据

| 模型 | 关键规则 |
| --- | --- |
| `DownloadBatch` | batch_id、有序 task_ids、创建计数与复用计数、状态统计；items 数量受 API 和 download_limits 约束，创建事务全有或全无，执行结果各自独立 |
| `FileAsset` | file_id、owner、track（实际音频曲目）、实际品质、相对目标路径、大小、容器、ETag、available／missing、创建任务；原曲及匹配链保留在各关联任务，复用文件不覆盖它们；对外不返回服务器绝对路径 |
| `MediaAccessDescriptor` | target（file／asset）、delivery、url、auth、expires_at 与媒体属性；不分配新的播放 ID，不保存播放器状态 |
| `MediaTicket` | 随机高熵 token 的哈希、owner、目标 kind/id、到期时刻；默认有效期使用 lifetimes，申请范围见 API |
| `Destination` | destination_id、名称、可用性、accept_new_tasks 与允许的 principal；根目录首次注册后与 ID 永久绑定，见[目标目录规则](configuration-and-deployment.md#目标目录绑定与变更) |
| `CredentialDescriptor` | credential_id、source、label、kind、auth_profile、scope 摘要、expires_at（可空）、version、状态、验证时间；不包含 Cookie、token、API key 或应用秘密 |

媒体票据在新请求入场时检查到期，允许有效期内多次 Range 请求，不是一次性票据。入场成功的单次传输可继续到完成，但受传输时间／空闲限制及身份撤销约束。asset 票据不得超过 asset 有效期；票据续发不能延长音源寿命。file 票据到期不删除文件，重新授权读取即可。

同一 asset 确定实际录音与字节表示后不再自动换到另一版本；刷新上游地址必须验证资源身份，否则要求重新解析并由客户端重新选择地址。文件资产发布后视为不可变；需要重写标签时生成新资产，不在客户端 Range 读取期间原地改变文件。

## SQLite 表设计

| 表 | 核心列与索引 |
| --- | --- |
| `schema_migrations` | version、checksum、applied_at；与 HTTP API 版本独立 |
| `principals` | id、display_name、role、enabled |
| `access_tokens` | token_id、principal_id、token_hash、scopes、created_at、revoked_at；不存可直接使用的明文 token |
| `credentials` | id、owner_id、source、label、kind、auth_profile、secret_ref、version、status、scope_summary、expires_at、last_verified_at、last_verify_result、revoked_at；与 CredentialDescriptor 字段对应，秘密由 SecretStore 管理 |
| `credential_defaults` | owner_id + source 为主键，credential_id 可空、selection_version；清除后保留版本行，不回收默认选择世代 |
| `destinations` | id、canonical_root；持久保存不可变位置绑定，名称、ACL 与是否接受新任务按启动配置校验应用 |
| `download_tasks` | id、owner_id、status、version、execution_id、指纹及 schema、normalized_intent、请求／执行快照、租约、next_retry_at、wait_reason、resume_stage、attempt_open；schema v2 增加 execution（公开执行事实：actual_track、actual_quality、容器、警告）、checkpoint（私有续传证据：已刷写字节、总长、校验器、传输是否完整）与 attempt_used_ms（未关闭尝试的已用运行时间，不含入场暂停）；索引 owner/status/created_at/id 与 status/next_retry_at |
| `download_batches`、`batch_items` | 批次信息与有序 task_id，支持一批里复用既有任务 |
| `task_attempts` | task_id、attempt_no、last_execution_id、起止时刻、错误、资源证据；每逻辑尝试一条，入场暂停后的领取更新最后执行世代，不新增尝试或保存敏感 URL |
| `file_assets` | file_id、owner_id、destination_id、相对路径、属性、ETag、状态、version；destination_id 外键指向不可变位置绑定，路径预留唯一约束 |
| `task_files` | task_id、file_id、关系；允许复用资产但不混淆创建任务 |
| `idempotency_records` | owner/method/path/key 唯一、request_hash、响应快照、expires_at |
| `finalization_intents` | task_id、临时路径标识、预留目标、校验证据、阶段，供崩溃恢复 |

批次与任务为多对多关系；同一批次也可按输入顺序出现重复 task_id。batch_items 使用 batch_id + ordinal 唯一约束，批次统计按条目计算，取消按唯一任务执行。任务表不以单个 batch_id 表达所属批次；列表 batch_id 过滤通过关系表查询。

活动任务指纹、幂等 key、尝试号、默认凭证和发布路径的唯一性，以及事务与崩溃矩阵见[内部契约](internal-contracts.md)。首版任务历史保留，短期 SSE 通知不落为独立事件历史。

解析缓存、游标状态和媒体票据首版可仅保存在有上限的内存中；进程重启后失效，客户端按 410 重新建立。下载任务与批次必须落库，不能用同样的内存策略。前端播放队列、当前播放位置和客户端保存进度不进入服务数据库。

单进程持有 data_dir 实例锁；SQLite 采用 WAL、单写入执行器和少量读取连接。文件系统操作不在长数据库事务内执行，网络请求也不持数据库锁。所有表操作通过 repository，业务用例不散落 SQL。

T05 的实现约定：表使用 STRICT 模式；库内时间存为 UTC 毫秒整数，对外 API 再格式化为 RFC 3339；WAL 配合 synchronous=FULL，提交返回即已落盘；唯一 writer 以有界等待（storage.wait_budget）和等待者上限（storage.writer_queue）实现，超出返回可重试的 STORAGE_UNAVAILABLE；读取连接以 query_only 打开并在读事务内取得一致快照。发布路径的唯一性按 ASCII 小写化的 path_key 比较（兼顾 Windows 与多数 NAS 的大小写不敏感），资产与发布意图共用同一预留空间。撤销凭证时同时清空其 secret_ref，旧引用交给调用方在提交后清理。

## 文件与数据库的协调

文件重命名和 SQLite 提交不能组成同一个原子事务。任务发布流程记录 finalization_intent、完成同文件系统重命名、再事务写入 file_asset 和 completed；启动恢复根据 intent、实际文件和校验证据补齐或标为需重新执行。

文件强 ETag 从最终发布字节生成并存储，标签处理后再计算，读取接口不因一次请求临时重写文件。任务轮询使用弱版本 ETag，二者不能混用于 If-Range。

文件被移走时将 asset 标为 missing，读取返回 410，历史保留。文件与 DB 不一致、取消与发布竞争按上述流程协调。

文件路径由资产的 destination_id 对应的持久根绑定与相对路径解析，不能使用当前“默认下载目录”替换根。根目录变更被启动校验拒绝；历史资产、工作区和发布意图沿用原位置，改用新目录时新建 destination_id。

## 0.1.6 本地产物集合扩展

- 开启 `export_lyrics` 或 `export_cover` 的下载意图使用 schema 4，包含两个导出标志、`sidecars_required` 与歌词选择，全部参与复用指纹；兼容 schema 1～3。未开启导出时维持旧编码和路径。SQLite 表结构未改变，回退旧程序前需使用匹配版本的备份。
- 开启导出时，finalization_intents 的 workspace_file／reserved_path 指向整个目录，evidence.properties.bundle 保存 audio／lyrics／cover 的文件名、大小、SHA-256。各文件在工作区写入并刷新后，保存意图，再独占重命名目录；SQLite 事务只在全部成员匹配时发布一个 file_id。目录是唯一预留键，资产 relative_path 指向目录内音频。恢复逐项比对，丢失或改写不完成。
- file_assets.properties.bundle 是同一资产的产物清单，不生成彼此独立的无主侧文件；成员共用 owner、destination ACL、生命周期与文件票据。读取／复用校验整个集合。历史 result 保留元数据及侧文件状态；原有单文件记录继续读写。
- import-ncm 任务使用内部来源 local_ncm、输入内容摘要作引用，保存的意图不含本机输入绝对路径。CLI 对输入复核哈希、保留原文件；相同输入与产品选项可复用完整资产。中断的未发布本地任务标记 LOCAL_IMPORT_INTERRUPTED，不交给在线平台调度；持久发布意图仍走通用恢复。
