# 后端服务功能模块

前后端职责见[模块边界](module-boundary-review.md)，接口与字段见 [API](api-contract.md)，线程和状态机见[运行时](runtime-and-tasks.md)。

## 模块总览

| 模块 | 职责 | 状态归属 |
| --- | --- | --- |
| ProcessHost／CompositionRoot | 生命周期、依赖组装 | 配置、进程状态 |
| Catalog／Collection／Discovery／Lyrics | 搜索、详情、分类、榜单、歌词 | 缓存、分页游标 |
| ProviderRegistry／Provider | 平台能力、请求构造、签名、解析 | 不可变配置、请求局部上下文 |
| Transport | HTTP、TLS、重定向、流式接收、取消 | 独占连接租约 |
| Resolve／Probe／Match | 音源解析、探测、候选匹配 | 有限期音源与匹配句柄 |
| DownloadCommand／TaskQuery | 创建、批量、取消、重试、查询 | SQLite 任务快照 |
| Scheduler／Runner／Recovery | 调度、执行、重试、恢复 | 执行句柄、工作区 |
| UpstreamAdmission | 上游操作的联合额度、冷却与入场 | 进程内唯一额度账本与独占租约 |
| AudioProcessor／Finalizer／FileAccess | 解密、验证、标签、发布、文件读取 | 临时产物、发布意图、文件资产 |
| MediaAccess／MediaTicket／StreamBridge | 访问描述、授权、可选代理 | asset／file 引用、票据、传输缓冲 |
| TaskEvents／SseResponder | 按身份推送已提交进度 | 有界订阅队列 |
| Principal／Credential／SecretStore | 服务身份、平台账号、秘密 | 身份表、凭证版本、秘密引用 |

这些名称表示职责，不要求一名一类、一模块一线程池。仅在独立生命周期、替换点或验证边界处增加抽象；目录和目标见[模块与目录](modules-and-layout.md)。

## 1. 生命周期与入口

ProcessHost 管理 initializing → recovering → ready → draining → stopped；必需初始化失败进入 failed 并退出。CompositionRoot 选择具体依赖，业务层通过端口调用。

启动顺序和停机规则见[配置与部署](configuration-and-deployment.md)。可选平台网络故障不阻止启动；数据库或已启用监听初始化失败阻止 readiness。配置变更通过重启生效，管理接口只支持暂停领取新任务。

控制入口执行：解析与大小约束 → request_id → CORS／预检 → 身份与 scope → DTO 校验 → 资源归属 → 入场额度 → 用例 → 响应映射。入口集中转换 HTTP 和业务类型，后台工作不持有 Request／Response。

## 2. 身份与凭证

PrincipalService 识别服务调用方；CredentialService 选择该 principal 对应平台的账号。默认凭证由 SQLite 的 credential_defaults 唯一管理，经 API 原子设置／清除，规则见[账号模型](data-models.md#账号绑定与授权身份)。本机 principal／token 管理由应用用例处理，CLI 只做参数与一次性交付，见[管理命令](configuration-and-deployment.md#本机身份管理命令)。

凭证模型支持 Cookie、OAuth2 和 API key，实际类型按平台声明逐步启用。CredentialService 统一生成稳定 AccountBinding 与带版本的 AuthIdentityKey；缓存、游标和租约使用完整身份键。应用身份与用户身份分开；平台包注册验证／刷新协议，application/accounts 合并刷新并条件提交，不由 Provider 直接改写秘密或数据库；详见[多平台扩展](provider-extensibility.md#6-凭证与账号)。

凭证更新生成新版本，新请求使用新版本，在途请求持有独立旧快照。删除阻止后续租用；已交付给请求的秘密不能立即收回。校验失败只记录状态、时间和原因，不覆盖秘密。存储协调见[内部契约](internal-contracts.md)。

## 3. 平台适配

每个平台包含能力描述、协议构造器、能力实现、响应解析器和上下文处理器。Registry 启动时注册并冻结，分别表达禁用、不支持和临时不可用。

平台标识与清单开放扩展，ProviderBundle 组合小能力接口；目录型、音源型及仅链接解析型均可接入。全部为原生实现，按需实例化；不接入外部 helper 或第三方下载工具。具体注册、兼容和验收见[多平台适配与扩展](provider-extensibility.md)。

Provider 完成单平台用例，共享调用者截止时间与连接额度；不执行跨平台匹配、下载调度或 SQL。配置不可变，Cookie、参数和解析缓冲按请求隔离。

平台 JSON 和签名留在适配器内。必要附加数据通过 owner 绑定的 context_ref 引用，入队前保存非秘密上下文。平台密码协议与 HTTPS 分开实现，时间与随机输入可注入以验证签名向量。端点迁移记录研究版本、去凭证样本、账号条件和错误分类。

## 4. 上游传输

Transport 提供有大小上限的 JSON／文本请求和逐块音频接收，共用 TLS、重定向、超时和取消策略。每跳校验目标范围及敏感头传播；不接受任意 URL 下载。默认仅 HTTPS，明文端点须由平台包逐项登记且不携带秘密，规则见[运行时](runtime-and-tasks.md#httplib-适配规则)。

ClientLease 按 origin、完整 AuthIdentityKey、代理和 TLS 策略隔离。runtime/admission 为每个具体上游操作发放 UpstreamLease，Transport 核对后执行；实际 I/O 结束后才回收连接和额度。无法入场返回 Deferred，由外层有界协调或持久延后；不在 Transport 中阻塞等待平台 semaphore。短请求进入上游池，下载写独占工作文件，代理写有界 StreamBridge。

Transport 处理允许的瞬时网络重试；Runner 决定任务重试和重新解析；创建任务由幂等接口防重。各层共同受总时间预算约束。

## 5. 搜索

SearchService 编排歌曲、歌单、专辑和歌手搜索：

1. 校验来源与能力，建立含凭证快照、分页和截止时间的逐源计划。
2. 处理空白但保留版本词，检查身份隔离缓存与游标。
3. 公平派发，限制聚合和平台并发。
4. Provider 返回统一条目、平台顺序、分页和错误。
5. 截止时间内汇总，生成每个来源自己的下一页游标。

首版按配置的稳定平台顺序分组，组内保留上游顺序，不跨源合并 ID 或比较平台评分。默认查询配置的 default_sources；平台可持续增加，单次来源数和并发仍受 limits 限制。其他版本查询交给 MatchService。失败不缓存为空结果，取消或超时后的迟到结果丢弃。

## 6. 集合与发现

CollectionService 分开查询详情和曲目页，保留曲目顺序。平台集合可能在分页期间改变；识别到版本失效则重新读取，不承诺跨页事务快照。

客户端选定 TrackRef 后分批提交下载；已接收任务不随歌单变动。跨批提交不原子，失败后使用各批原幂等 key 恢复。完整后台歌单枚举尚未纳入 v1，见[范围限制](module-boundary-review.md)。

DiscoveryService 提供分类、榜单和推荐，保留 source 内的 ID、平台排序及 catalog_kind。更新时间未知为 null，推荐缓存按完整 AuthIdentityKey 隔离。

LyricsService 统一普通、翻译与逐字歌词；LinkParseService 有限展开允许的平台短链。歌词滚动和展示归前端。封面首版只返回公开 URL，需要秘密头且无专用代理时返回 null／警告。

## 7. 音源解析与探测

ResolveService 按“实际曲目 → 账号可用资源 → 品质 → 试听策略”选择资源，生成内部 ResolvedAsset 与公开 AssetDescriptor。strict 不降级，allow_lower 才允许较低品质；格式扩展名不能独立证明品质。

ProbeService 使用 HEAD 或受字节／时间限制的 GET 获取可访问性、格式、大小和身份依据。Range 被忽略时提前停止，不为探测下载整首歌。开始下载或新媒体请求仍需验证实际响应。

资源句柄过期后重新解析；下载执行不依赖客户端提前创建的 asset_id。直链外发条件由 MediaAccessService 检查。

## 8. 匹配与换源

| 阶段 | 规则 |
| --- | --- |
| 查询 | 使用后端取得的标题、主要艺术家和必要专辑信息 |
| 归一 | 大小写、空白、有限标点；保留原文和版本标签 |
| 排除 | 明确的现场／录音室、翻唱／原唱、伴奏／有唱冲突 |
| 排序 | 标题、艺术家、时长、专辑和版本证据；未知不算完全相同 |
| 探测 | 仅解析有限候选 |
| 输出 | 候选、分数、理由、可用性和期限 |

评分不是概率；自动通过阈值须经标注样本验证。自动换源默认关闭，需服务开关、请求允许和匹配条件同时满足。候选计划有固定集合与已尝试集合，按错误白名单触发，避免循环换源。

目录型来源缺少同源音源能力时，也可进入显式手动匹配或符合上述开关条件的自动匹配。original_track 与 actual_track 分别保留；ISRC 可作附加证据，不能代替版本核验。Provider 内不隐藏执行跨源匹配或把目标来源改写为原目录来源。

更换曲目须重新建立资源和工作文件。同一 asset_id 绑定固定录音与字节表示；无法确认刷新后身份时要求新解析，由前端决定替换地址。

## 9. 下载命令与执行

DownloadCommandService 先查已提交幂等，再在事务外补全策略、账号绑定、稳定匹配目标及上下文，形成 NormalizedDownloadIntent；领域规则计算指纹，Repository 在短事务内复核版本、权限和配额并完成幂等、去重与批次。结果复用核验用户的试听／标签等实际要求。提交后返回 ID 并唤醒调度，唤醒失败由扫描兜底。TaskQueryService 只读取持久公开快照，不联网。

Scheduler 按 principal 轮转，仅管理任务领取、到期时间和下载执行槽；领取生成 execution_id，不自动增加逻辑 attempt。平台、origin、共享应用配额由 runtime/admission 按实际操作统一管理。Runner 无法入场时持久保存 waiting_retry 与 wait_reason=admission，提交后释放 worker／执行槽，再领取沿用逻辑尝试；状态写入始终检查执行标识和版本。Runner 可在 processing 时占用下载执行槽，但释放上游连接额度。

Runner 执行解析 → 探测 → 接收 → 处理 → 发布，持有独占工作区和取消源。进度包括已验证续传字节，重下允许重置；速度或总量未知为 null。检查点提交后才更新公开版本和 SSE，发布成功后才 completed。

RetryPolicy 分类网络、资源、账号、协议和文件错误；ResumePolicy 校验资源及 partial；Recovery 先处理发布意图再恢复任务。取消竞争、重试预算和 Range 分支见[运行时](runtime-and-tasks.md)。

## 10. 音频处理与文件

AudioProcessor 执行必要解密、格式／长度验证和可选标签、歌词写入，返回产物与处理报告。默认保留原始编码。标签失败按 metadata_policy 处理；必要解密或验证失败始终阻止发布。

首版基线处理器 `audio.verify` v1 只按字节识别容器并校验 MP3 帧结构，不读 URL 后缀或声明格式：HTML／JSON／文本或空正文、无法识别或尚未支持的容器、被截断（标签或末帧被切断、少于传输声明的总字节或 Xing／VBRI 声明的帧数）及损坏过多的文件返回任务错误 `AUDIO_INVALID`（details 给出原因，上游错误正文与截断可重试）。时长由帧数实测；与平台声明长度一致才作为整曲证据，明显更短视为试听，没有声明长度则完整性维持原判定。T30 起为 `audio.verify` v2：另验证 FLAC 与 MP4／M4A 结构，并用 TagLib 把平台详情给出的标题、歌手、专辑（以及给出时的歌词与封面）写入工作文件旁的副本，副本的音频载荷须与原文件测量一致，发布的是副本；收到的文件保持不变，重处理从同一字节开始。skip 不写；best_effort 下没有可信元数据或写入失败时发布未加标签的文件并附 METADATA_NOT_WRITTEN，部分项无法写入附 METADATA_PARTIAL（metadata_status=partial）；required 下任一项无法写入返回 METADATA_FAILED，不发布。

FileWorkspace 位于目标文件系统；AssetStore 管理路径预留与不可变文件；Finalizer 协调发布意图、重命名和数据库。可选侧文件报告独立结果，必需产物纳入恢复集合，多文件重命名不视为原子事务。

客户端只选择 destination_id，其根绑定不可变；新增下载位置使用新 ID，文件读取和恢复不跟随当前默认目录，见[目录变更规则](configuration-and-deployment.md#目标目录绑定与变更)。已完成文件被外部移走时标记 missing，保留历史。发布规则见[内部契约](internal-contracts.md)。

## 11. 媒体访问

MediaAccessService 复用 asset_id／file_id，提供访问描述，不再解析歌曲或创建播放实体。在线流程为 resolve → media-access → 客户端播放；已保存文件直接使用 file_id。

直链受部署策略和平台资源条件限制；未知条件选择代理。客户端直读失败可请求 proxy。代理通过有界 StreamBridge 处理请求头、身份和 Range；尚不支持的实时解密／格式处理需先走后台下载处理。

读取只开放已发布文件；不共享下载中的 partial。asset 到期拒绝新读取，已入场响应按预算结束。票据不能延长 asset 寿命。下游断开取消本次代理，不取消独立下载。访问字段、授权和错误见 [API 媒体契约](api-contract.md#10-文件与媒体访问)。

## 12. 进度、存储与观测

TaskEventService 分发已提交快照，SseResponder 编码并发送。每前端一条连接，订阅按 owner 隔离；HTTP 查询独立可用。事件合并、重连同步和慢消费者处理见[运行时](runtime-and-tasks.md#sse-事件传输)及 [API](api-contract.md#94-sse-下载事件)。

Repository 在查询和修改中带 owner 条件，通过单 writer 提交。数据库不可写时不发布新版本或 completed；Runner 在预算内暂停推进、保留恢复材料，并报告服务不健康。

日志与指标区分控制、平台、下载、处理、DB、媒体和 SSE，携带 request_id／task_id、来源、耗时与去敏错误。缓存和短期句柄的寿命见[配置](configuration-and-deployment.md)，实施顺序见[设计入口](README.md#功能分期)。
