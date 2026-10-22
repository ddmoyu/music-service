# 后端内部接口与数据一致性设计

内部操作、状态所有权和提交边界如下；对外契约见 [API](api-contract.md)。

## 1. 契约表达方式

内部操作以“输入 → 结果”描述。可能失败的操作返回项目自有 Result，包含结构化 DomainError；不跨模块抛出第三方异常作为常规错误。入口和执行器边界捕获意外异常，记录内部诊断，返回去敏错误。

操作分为纯计算、有界同步 I/O、调度与持久提交三类。纯计算不联网；同步操作可以阻塞其所属工作线程，但必须有截止时间；调度返回句柄不代表工作已完成；持久提交成功才允许返回已接受或发布新任务版本。

| 上下文 | 包含内容 | 使用范围 |
| --- | --- | --- |
| RequestContext | principal、scope、request_id、绝对截止时间、取消信号 | 一次控制请求 |
| ProviderContext | 完整 AuthIdentityKey、只读 CredentialLease、地区、稳定应用配额组、deadline、取消信号 | 一次平台操作；不向其他来源转交秘密，不代表已获得网络额度 |
| TaskExecutionContext | owner、task_id、execution_id、attempt、normalized_intent、配置快照、任务截止时间、取消源 | 一次任务执行，不持 HTTP Request；逐操作租用授权与上游额度 |
| MediaContext | owner、asset／file 身份、Range、传输预算、取消信号 | 一次媒体响应，无播放位置或播放器状态 |
| SubscriptionContext | principal、订阅 ID、授权版本、连接期限、关闭信号 | 一条 SSE 连接 |

截止时间用单调时钟控制进程内等待；持久 deadline 使用 UTC 时间保存，重启后计算剩余预算。系统时间大幅变化需记录诊断并保守处理，不把墙上时钟变化当成下载速度。底层不支持即时取消时，句柄和额度保留到真实退出。

## 2. 应用层接口目录

| 组件与操作 | 输入 → 输出 | 副作用与执行约束 |
| --- | --- | --- |
| SearchService.search | 类型、查询、来源、游标、上下文 → items + SourceResult | 有界上游查询与缓存更新 |
| CollectionService.detail/list_items | 集合 ref、分页、上下文 → 详情／页 | 不隐含递归拉取全部曲目 |
| DiscoveryService.list/get_items | source、目录类型／id、分页 → 目录／页 | 保留 catalog_kind 和平台顺序 |
| LyricsService.get | TrackRef、context_ref → 统一歌词 | 私有格式解析限定在 Provider 内 |
| LinkParseService.parse | 分享文本 → matched、kind、ref | 只展开允许平台的有限短链 |
| ResolveService.resolve | 原曲、策略、可选匹配选择 → 内部资源与公开描述 | 可保存有限期 asset 句柄，不启动下载 |
| ProbeService.probe | 已授权内部资源、预算 → 探测证据 | 有界读取，不修改已发布文件 |
| MatchService.find | 原曲、目标来源、限额 → 匹配上下文与候选 | 搜索／探测占共享上游预算 |
| DownloadCommandService.create/create_batch | 创建请求、幂等 key、上下文 → 任务／批次结果 | 一个创建事务，提交后调度唤醒 |
| DownloadCommandService.cancel | owner、task_id → 当前状态／接受结果 | 状态条件转换，提交后通知 Runner |
| DownloadCommandService.retry | owner、旧 task_id、新幂等 key → 新 task_id | 复制请求并重新校验，不重开旧终态 |
| TaskQueryService.get/list | owner、过滤／游标 → 公开持久快照 | 不联网，投影删除秘密 |
| TaskEventService.subscribe | SubscriptionContext → SubscriptionHandle | 注册有界订阅，按 owner 过滤 |
| MediaAccessService.describe | owner、asset_id／file_id、mode、auth、期限 → 访问描述 | 复用已解析资源，选择安全直链／代理，按需签发票据，不创建播放实体 |
| FileAccessService.describe/open | owner、file_id → 描述／只读文件租约 | 文件存在性、归属与 immutable 校验 |
| MediaTicketService.issue/authorize | owner、file／asset 目标、期限 → 票据／授权目标 | 媒体模块内部组件，不提供另一套重复公开签发接口；不能延长 asset 寿命 |
| CredentialService.put/verify/remove | owner、source、凭证输入 → 去敏描述 | 秘密存储与版本更新，不在验证中修改任务 |
| CredentialService.select/lease | owner、source、可选账号选择／固定 AccountBinding → 绑定／CredentialLease | 唯一身份键生成处；固定绑定不重新选择默认账号，需要刷新时由用例协调 |
| CredentialService.get_default/set_default/clear_default | owner、source、可选 credential_id → 默认引用与 selection_version | SQLite 单事务，验证 owner／source；不改其他账号，不自动选择替代凭证 |
| PrincipalService.create/set_enabled/list | 本机管理输入 → 身份描述 | 独占实例锁后调用 Repository；不通过普通 HTTP admin 暴露 |
| AccessTokenService.issue/revoke/list | principal、明确 scope／token_id → token 描述 | 明文只交付至受限新文件；DB 存哈希和撤销事实，规则见本机管理命令 |
| DownloadIntentNormalizer.prepare | 请求、有效策略、授权与上下文 → PreparedDownloadIntent | application/tasks 内部步骤；事务外校验候选并保存稳定目标，领域纯规则生成规范意图与指纹 |

入口通过应用层调用 Provider。Runner 使用 TaskExecutionContext 调用 ResolveService 等用例，不依赖 HTTP 请求。

## 3. Provider 与传输端口

### 3.1 能力接口

每种能力独立声明，如 ITrackSearch、ITrackDetail、IPlaylistSearch、IAlbumSearch、ICollectionItems、IDiscovery、ILinkParser、IAssetResolver、ILyrics。Registry 提供 Descriptor 与受类型约束的能力访问；不存在能力时返回 unsupported，不通过 dynamic_cast 失败或空指针推断业务错误。ProviderBundle 注册、按需实例化与新增平台流程见[多平台扩展](provider-extensibility.md)。

Provider 输入仅使用统一查询和自己的内部解析上下文；输出使用领域对象、SourcePage、内部 ResolvedAsset 或 ProviderError。ProviderError 保留平台码、HTTP 状态、可重试提示和去敏诊断，应用层决定它对当前用例意味着什么。

| 约束 | 设计 |
| --- | --- |
| 平台调用线程安全 | 长期 Provider 对象保存不可变配置；每次请求的 Cookie、参数和解析缓冲均局部持有 |
| 跨线程共享 | 显式声明线程安全的 Registry／Repository／EventHub；第三方 Client 不隐含线程安全 |
| 时间预算 | 内部所有请求共享调用者 deadline，分页补充请求不重置预算 |
| 账号选择 | 先由 CredentialService 建立 AccountBinding 和完整 AuthIdentityKey，再构建请求；任何缓存／租约不得只用版本标识账号 |
| 上游失败 | 不把响应解析错误当成零结果，不把所有 HTTP 403 当成永久账号失败 |
| 平台适配更换 | 换协议实现不修改 TrackRef 和已有任务身份 |

ProviderBundle 的声明必须与能力工厂一致；Registry 启动冻结。Context 与 task 私有快照记录 schema／implementation 版本；变更后通过受控迁移或重新解析恢复，不能盲读旧结构。Provider 不拥有任务队列、数据库或最终文件发布职责。

目录型平台的匹配由 MatchService／ResolveService 编排，不通过平台内部跨源递归调用。ResolveService 先处理合法的 match_selection，再检查实际目标 IAssetResolver；原曲无音源能力不妨碍手动选择目标。

ICredentialVerifier、ICredentialRefresher、IApplicationTokenProvider 是可选的小端口，以 source + auth_profile + kind 由 ProviderBundle 注册。输入为只读授权快照和操作上下文，输出为校验结果或待提交的秘密候选；平台工厂不得取得 Repository 或可写 SecretStore。CredentialService 负责刷新合并与条件提交，详见[授权注册与调用](provider-extensibility.md#授权能力注册与调用)。

### 3.2 HttpTransport

`request` 接收方法、受校验目标、头、body、响应上限、上下文与匹配的 UpstreamLease，返回有界响应；同步便捷入口可通过注入的入场端口非阻塞申请租约，但不得内部阻塞排队。`receive` 先调用响应头消费者，只有状态和资源校验接受后才调用正文消费者。消费者可以拒绝继续读取，并返回明确原因。

逐块数据在回调返回后不保证存活。写盘消费者必须当场写入或复制到受限缓冲，不能保存指向传输库内部缓冲的裸指针。回调不得持有调度锁或等待另一个会反向依赖本操作的任务。

取消句柄支持请求停止和等待最终结束，二者不是同一动作。ClientLease 析构或回收前确保请求结束；正常完成可以复用连接，取消、协议不完整或错误的连接按传输适配规则废弃。

ClientLease 按完整 AuthIdentityKey、origin、代理和 TLS 策略隔离。连接池复用不代表可以绕过入场，每跳与每次重试均检查实际操作租约；没有租约不开始网络请求。传输结束报告配额反馈，具体归还与重定向规则见[运行时](runtime-and-tasks.md#上游入场与租约)。

### 3.3 扩展传输与文件处理

ResolvedAsset 携带类型化传输计划和处理计划。TransferExecutor 按传输计划类型分派；AudioProcessor 按 processor_id 与版本调用已注册处理器，不按平台字符串分支。计划中不包含任何命令或外部程序。首版仅启用 HttpAsset，额外传输形态由服务自己实现并经独立验证后开放。

### 3.4 上游入场端口

| 操作 | 契约 |
| --- | --- |
| UpstreamAdmissionPort.try_acquire | AdmissionRequest → Granted(UpstreamLease)／Deferred／截止或取消错误；所有维度一起预留，不占 worker 等待 |
| UpstreamAdmissionPort.readiness | 操作维度 → 可运行提示／最早重试时刻；仅供公平派发，不能代替租约 |
| UpstreamAdmissionPort.feedback | 已执行操作、可信上游状态／Retry-After → 更新对应配额组冷却；不修改任务状态 |
| UpstreamLease.release | 实际操作结束后唯一归还；未开始且派发失败可以撤销，停止信号不等于结束 |

端口定义在 application/ports/runtime，唯一实现位于 runtime/admission；应用服务、Provider 传输适配和下载执行器通过依赖注入共用实例，不反向包含 runtime 的实现头。Deferred 是调度结果，不伪装成平台网络错误。其持久化及公平重派发由调用用例／Runner 决定，runtime 不读取歌曲或任务表。

## 4. 任务端口与状态写入

| 端口操作 | 条件／结果 |
| --- | --- |
| TaskRepository.create_transaction | 接收 PreparedDownloadIntent，在事务内复核绑定／配置版本、owner 配额与幂等竞争，按规范意图去重，写任务与批次；成功返回提交结果 |
| TaskRepository.claim | 仅领取合适的 queued／到期 waiting_retry；生成 execution_id 和新版本，关联恢复位置及当前尝试，不因领取自动递增 attempt |
| TaskRepository.begin_attempt | 带执行世代与版本；尚未开始新尝试时原子递增 attempt、写尝试开始时刻与尝试截止，首次调用时同时固定任务绝对截止；同一次调用重放不重复计数 |
| TaskRepository.defer_admission | 当前 I/O 已结束；条件保存 waiting_retry、wait_reason=admission、resume_stage、attempt_open、next_retry_at 和恢复证据；提交后才释放执行槽 |
| TaskRepository.transition | 带 task_id、execution_id、expected_version、合法迁移；冲突返回当前快照 |
| TaskRepository.checkpoint | 只写当前执行允许状态下的进度与恢复证据；不覆盖 canceling 或终态 |
| TaskRepository.request_cancel | 按当前状态原子决定立即取消、进入 canceling 或拒绝 |
| TaskRepository.finish | 依赖发布意图和资产证据，事务写资产关系、completed 和新 version |
| DownloadQueuePort.wake | 提示扫描，不表示已领取；失败由定期扫描兜底 |
| TaskControlPort.request_stop | 向当前 execution_id 的 Runner 发信号，不把信号成功等同 canceled |

version 用于状态与进度并发控制，execution_id 用于区分执行世代。二者同时检查，避免旧尝试刚好遇到可写状态而错误更新新尝试。取消操作不携带执行凭证，只通过应用授权和 Repository 条件更新作用于当前任务。

下载进行中按配置的进度发布间隔更新公开版本；数据库繁忙时合并尚未提交的普通进度，只保留当前执行的最新计数。状态转换与终态不能当作普通进度合并掉；若提交失败，保留未发布状态并走错误／恢复处理。

## 5. 对象所有权与锁规则

| 对象 | 创建者／所有者 | 结束条件 |
| --- | --- | --- |
| ServiceContainer | ProcessHost 唯一持有 | 所有入口、执行器和工作线程结束后销毁 |
| ProviderRegistry | CompositionRoot 构造的不可变对象 | 进程收尾 |
| OperationState | 应用用例与执行器共享 | HTTP 返回后，迟到工作退出才释放最后引用 |
| ClientLease | 执行实际 I/O 的 worker 独占 | 请求真实结束并回收／关闭连接 |
| UpstreamLease | 入场管理者发放，实际操作独占 | I/O 真实结束后归还；未派发可以撤销 |
| TaskRunner | 调度器管理句柄，Runner 持自身执行状态 | 进入终态、等待重试或安全停机 |
| FileWorkspace | 当前 Runner 独占 | 成功发布／用户取消清理／交给恢复流程 |
| StreamBridge | 上游 worker 与媒体 responder 共享 | 双方退出，剩余缓冲释放 |
| Subscription | EventHub 注册表与 responder 共享 | 标记关闭、从注册表移除、发送线程退出 |
| CredentialLease | 单次平台操作独占秘密快照 | 操作结束，不作为全局当前账号 |

默认值传递与唯一所有权；跨线程共享生命周期时使用共享所有权。

锁规则：短时间修改内存状态，释放锁后再做网络、磁盘或数据库等待；注册表锁只保护索引，不在持锁期间向所有订阅写 socket。数据库事务也不跨越网络与文件处理。

上游 worker 不能向同一个已满的上游池提交子任务后同步等待。Provider 已在上游 worker 执行时，串行 HTTP 可直接消费该 worker，但每个操作仍须非阻塞取得匹配租约；Deferred 必须退出到外层协调，不能原地等待。刷新等待者也不占短请求 worker 等刷新工作；需要并发时由外层规划后派发。processing worker 不等待下载 worker，数据库 writer 不等待任何业务 worker。

## 6. 持久实体与索引补充

表目录见[数据模型](data-models.md)，索引和事务规则如下。

| 实体／关系 | 关键唯一性与索引 | 目的 |
| --- | --- | --- |
| download_tasks | id 唯一；owner/status/created_at/id；status/next_retry_at | 查询分页、公平调度与到期领取 |
| 活动任务指纹 | owner + fingerprint 在活动状态内唯一 | 并发创建不能绕过去重检查 |
| batch_items | batch_id + ordinal 唯一；task_id 有反查索引 | 保留输入顺序，允许多批引用同一任务 |
| task_attempts | task_id + attempt_no 唯一，last_execution_id 记录该尝试的最后世代 | 入场暂停不新增逻辑尝试；旧 worker 拒写以任务当前 execution_id/version 为准，历次世代由日志追踪 |
| idempotency_records | owner + method + route + key 唯一 | 并发提交只有一个结果 |
| file_assets | file_id 唯一；owner + created_at；目标路径唯一 | 文件获取、资产追踪和路径冲突处理 |
| finalization_intents | task_id 唯一；发布状态索引 | 重启优先恢复未完成发布 |
| credentials | id 唯一；owner + source；秘密引用及版本 | 平台账号描述，不另存 is_default 与默认选择表竞争 |
| credential_defaults | owner + source 主键；credential_id 可空、selection_version | 默认选择唯一且清除后仍保留世代；关联凭证必须同 owner/source |
| destinations | id 主键；canonical_root 不可变 | FileAsset、工作区与发布意图解析同一持久位置，不跟随默认目录变化 |

所有任务所属批次关系以 batch_items 为准；task 不使用一个可变 batch_id 列来表达多对多关系。批次统计实时按其条目关联任务快照计算；条目重复时计数按条目，取消时按唯一 task_id 执行。

fingerprint 在同一事务中检查与插入，不能只先 SELECT 再在另一个事务 INSERT。规范化字段与完成结果适配规则以[下载意图](data-models.md#幂等与下载去重)为准；Repository 不自行补默认值或解释 match_selection。已完成结果复用须在事务外核验文件、权限与策略证据，事务内再次核对相关任务／资产版本；不能把一次旧检查当成永久存在保证。未通过复用检查的完成任务不占活动指纹，新任务可以正常创建。

### 6.1 事务边界

| 事务 | 必须一起提交 | 提交外的工作 |
| --- | --- | --- |
| 创建任务／批次 | 幂等响应、normalized_intent、请求快照、任务、批次条目、配额和版本复核 | 查已有幂等、准备意图与账号绑定、上下文补全／候选验证、文件复用检查；提交后调度唤醒 |
| 领取任务 | execution_id、当前尝试关联、状态、版本 | 入场申请、平台请求、文件准备 |
| 开始逻辑尝试 | attempt 递增、attempt_open、尝试开始／截止、首次尝试时的任务绝对 deadline、task_attempts | 已获准的业务阶段；提交失败则不开始 I/O |
| 延后入场 | waiting_retry、wait_reason、恢复位置、尝试开闭、next_retry_at、检查点 | 先结束旧 I/O；提交后释放执行槽，按预算重新派发 |
| 取消请求 | cancel_requested、状态、版本 | 提交后停止句柄与事件通知 |
| 进度检查点 | 当前公开进度、版本、可恢复证据 | 字节接收、文件刷写策略 |
| 开始发布 | 状态 finalizing、目标预留、发布意图 | 文件重命名、验证 |
| 完成发布 | file_assets、task_files、completed、意图完成标记 | 提交后 SSE 通知 |
| 默认凭证选择 | 默认引用与 selection_version；核对 owner/source、凭证可选状态与 profile | 提交后新请求生效，旧选择句柄失效；重复设置不新增版本 |
| 撤销平台凭证 | 撤销状态、凭证 version、清除其默认引用并推进 selection_version | 提交后失效缓存／新租约，清理秘密；不改已持久任务的绑定 |
| 服务 token 签发／撤销 | token 描述、哈希与撤销事实 | 离线独占锁；签发前安全创建并刷写明文交付文件，失败残留不导入 |

检查点中的 received_bytes 是公开传输进度，不等同于断电后必定存在的持久字节。恢复时以工作文件实际长度和资源证据重新验证；需要强恢复点时文件刷写先于相应恢复证据提交。

使用默认账号创建任务时，PreparedDownloadIntent 另带准备阶段读取的 selection_version；提交事务复核它及相关凭证版本，变化则重新准备或明确冲突，不提交旧默认选择。该提交校验信息不加入下载指纹；已受理任务执行与 retry 使用固定绑定，不再依赖默认选择世代。目录注册和检查发生在任务恢复前，根绑定冲突不得进入恢复流程，见[目标目录规则](configuration-and-deployment.md#目标目录绑定与变更)。

### 6.2 凭证和秘密提交

先把新秘密写到新的不可变 secret_ref，再事务更新凭证描述与版本，最后使相关缓存失效并清理不再引用的旧秘密。DB 提交失败时新秘密成为待回收孤立项，旧有效凭证仍可使用；不能先覆盖旧秘密后让 DB 失败。

删除先使数据库凭证不可新租用，再清理秘密文件。清理失败记录待重试，不重新暴露已删除的凭证。日志和任务快照只记录 credential_id/version，不记录秘密正文。

用户刷新提交还检查原 credential version、所依赖应用配置版本和撤销标记；成功才发布新身份键。应用 token 使用受限内存缓存，按 app_profile、scope 与配置版本合并获取；条目有唯一 app_token_id，条件替换递增 app_token_version，淘汰后重建使用新 ID。重启失效后重新获取，不为此新增平台专表或回写管理配置。应用私钥／client_secret 仍从 SecretStore 只读租用。

## 7. 文件发布与崩溃矩阵

Finalizer 的输入是通过验证的临时产物和已确认策略，输出是不可变资产。发布意图记录 task_id、execution_id、工作区相对标识、预留目标、大小／内容证据和必需产物集合。

| 中断位置 | 重启可见状态 | 恢复动作 |
| --- | --- | --- |
| 写发布意图前 | processing 或旧检查点 | 校验工作区，重新处理或下载，不猜测已发布 |
| 意图已提交，尚未移动 | finalizing + 临时产物 | 校验后继续发布，核对目标预留 |
| 文件已移动，资产事务未提交 | finalizing + 目标文件 | 用意图证据确认文件，补提交资产与终态 |
| 资产事务已提交，SSE 未发出 | completed + file_id | 返回已完成事实，重连查询恢复显示 |
| 临时与目标同时存在 | 有意图，位置冲突 | 根据证据确认归属，不覆盖不属于本任务的文件 |
| 临时与目标都不存在 | 有意图，无产物 | 预算与尝试次数允许时转 waiting_retry 重做下载；否则转 failed（FINALIZATION_FAILED）；不得标 completed 或停留在 finalizing |
| 重命名／目标写入本身失败 | finalizing + 错误 | 释放目标预留，保留工作产物与诊断，转 failed；磁盘或权限问题由用户处理后创建重试任务 |

路径预留和实际“禁止覆盖”的文件操作都要实施，数据库唯一约束不能防止外部程序在同一路径创建文件。重命名后验证失败时保留诊断与恢复材料，不把未知文件当成合法下载结果。

数据迁移按递增版本和校验和执行，遇到比程序更新的 schema 拒绝启动写入。首版不承诺自动降级数据库；升级前一致性备份与恢复验证属于发布步骤。

## 8. SSE 与 HTTP 快照一致性

公开事件携带 task_id、version、status、progress、updated_at，均从已提交快照生成；不从网络回调直接拼一个独立“百分比状态”。HTTP get/list 返回同一事实来源的投影。

订阅注册必须与 EventHub 的发布顺序协调：注册后才发 stream.ready，之后已提交的变化进入该订阅或显式使其失效。提交后的发布通过有序短回调完成，回调只入队内存事件，不进行客户端 I/O。

即使入队设计有序，客户端仍按版本处理迟到事件。队列合并删除旧进度后，将新投影放到适当待发位置；不得让低版本进度排在同任务终态之后被当成新状态。

无法发出通知时关闭相应订阅，强制重新同步。进程在 DB 提交后、通知前崩溃会导致连接断开，客户端重连查到提交结果。此设计保证恢复当前状态，不保证每次中间进度都有可重放历史。

客户端同步期间维护受限的 task_id → 最新投影映射。HTTP 当前列表之外仍在页面追踪的任务按 ID 刷新，避免仅查询“活动任务”而漏掉断线期间完成的任务。同步查询失败不宣布恢复完成；继续退避重试或使用 HTTP 兜底。

## 9. 错误、取消与降级责任

| 场景 | 决策模块 | 外部表现 |
| --- | --- | --- |
| 搜索单源超时 | SearchService | 按逐源结果返回部分成功／整体超时 |
| Provider JSON 变化 | Provider 解析器 | 上游协议错误，不伪造空数据 |
| 同曲 URL 失效 | Resolve／Resume 策略 | 重新解析并核对资源，不盲目追加 |
| 跨平台候选选择 | Match／Resolve 策略 | 必须满足明确换源策略，记录 actual_track |
| 用户取消与 finalizing 竞争 | TaskRepository | 事务决定接受取消或 409 |
| 标签 best_effort 失败 | AudioProcessor + Runner | 音频可完成，结果保留 warnings |
| SQLite 提交失败 | Repository + Runner | 不发布假版本；保留材料，进入恢复处理 |
| SSE 慢消费者 | EventHub／SseResponder | 关闭该订阅，任务继续，客户端重连同步 |
| 媒体消费者断开 | StreamBridge | 取消该次上游流，不取消持久下载 |

日志保留内部原因，对外错误稳定且去敏。状态、重试和错误码变更同步更新规范及验收。
