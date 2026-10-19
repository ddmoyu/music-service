# 后端内部接口与数据一致性设计

内部操作、状态所有权和提交边界如下；对外契约见 [API](api-contract.md)。

## 1. 契约表达方式

内部操作以“输入 → 结果”描述。可能失败的操作返回项目自有 Result，包含结构化 DomainError；不跨模块抛出第三方异常作为常规错误。入口和执行器边界捕获意外异常，记录内部诊断，返回去敏错误。

操作分为纯计算、有界同步 I/O、调度与持久提交三类。纯计算不联网；同步操作可以阻塞其所属工作线程，但必须有截止时间；调度返回句柄不代表工作已完成；持久提交成功才允许返回已接受或发布新任务版本。

| 上下文 | 包含内容 | 使用范围 |
| --- | --- | --- |
| RequestContext | principal、scope、request_id、绝对截止时间、取消信号 | 一次控制请求 |
| ProviderContext | principal、source、凭证版本／只读秘密租约、截止时间、取消信号 | 一次平台操作 |
| TaskExecutionContext | task_id、execution_id、attempt、配置快照、任务截止时间、取消源 | 一次任务执行，不持 HTTP Request |
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

入口通过应用层调用 Provider。Runner 使用 TaskExecutionContext 调用 ResolveService 等用例，不依赖 HTTP 请求。

## 3. Provider 与传输端口

### 3.1 能力接口

每种能力独立声明，如 ITrackSearch、IPlaylistSearch、IAlbumSearch、ICollectionItems、IDiscovery、IAssetResolver、ILyrics。Registry 提供 Descriptor 与受类型约束的能力访问；不存在能力时返回 unsupported，不通过 dynamic_cast 失败或空指针推断业务错误。

Provider 输入仅使用统一查询和自己的内部解析上下文；输出使用领域对象、SourcePage、内部 ResolvedAsset 或 ProviderError。ProviderError 保留平台码、HTTP 状态、可重试提示和去敏诊断，应用层决定它对当前用例意味着什么。

| 约束 | 设计 |
| --- | --- |
| 平台调用线程安全 | 长期 Provider 对象保存不可变配置；每次请求的 Cookie、参数和解析缓冲均局部持有 |
| 跨线程共享 | 显式声明线程安全的 Registry／Repository／EventHub；第三方 Client 不隐含线程安全 |
| 时间预算 | 内部所有请求共享调用者 deadline，分页补充请求不重置预算 |
| 账号选择 | 先绑定 owner + source + credential version，再构建请求 |
| 上游失败 | 不把响应解析错误当成零结果，不把所有 HTTP 403 当成永久账号失败 |
| 平台适配更换 | 换协议实现不修改 TrackRef 和已有任务身份 |

### 3.2 HttpTransport

`request` 接收方法、受校验目标、头、body、响应上限与上下文，返回有界响应。`receive` 先调用响应头消费者，只有状态和资源校验接受后才调用正文消费者。消费者可以拒绝继续读取，并返回明确原因。

逐块数据在回调返回后不保证存活。写盘消费者必须当场写入或复制到受限缓冲，不能保存指向传输库内部缓冲的裸指针。回调不得持有调度锁或等待另一个会反向依赖本操作的任务。

取消句柄支持请求停止和等待最终结束，二者不是同一动作。ClientLease 析构或回收前确保请求结束；正常完成可以复用连接，取消、协议不完整或错误的连接按传输适配规则废弃。

## 4. 任务端口与状态写入

| 端口操作 | 条件／结果 |
| --- | --- |
| TaskRepository.create_transaction | 校验 owner 配额，写幂等、任务、批次关系；成功返回提交结果 |
| TaskRepository.claim | 仅领取合适的 queued／到期 waiting_retry；返回 execution_id 和新版本 |
| TaskRepository.transition | 带 task_id、execution_id、expected_version、合法迁移；冲突返回当前快照 |
| TaskRepository.checkpoint | 只写当前执行允许状态下的进度与恢复证据；不覆盖 canceling 或终态 |
| TaskRepository.request_cancel | 按当前状态原子决定立即取消、进入 canceling 或拒绝 |
| TaskRepository.finish | 依赖发布意图和资产证据，事务写资产关系、completed 和新 version |
| DownloadQueuePort.wake | 提示扫描，不表示已领取；失败由定期扫描兜底 |
| TaskControlPort.request_stop | 向当前 execution_id 的 Runner 发信号，不把信号成功等同 canceled |

version 用于状态与进度并发控制，execution_id 用于区分执行世代。二者同时检查，避免旧尝试刚好遇到可写状态而错误更新新尝试。取消操作不携带执行凭证，只通过应用授权和 Repository 条件更新作用于当前任务。

下载进行中可以每秒更新公开版本；数据库繁忙时合并尚未提交的普通进度，只保留当前执行的最新计数。状态转换与终态不能当作普通进度合并掉；若提交失败，保留未发布状态并走错误／恢复处理。

## 5. 对象所有权与锁规则

| 对象 | 创建者／所有者 | 结束条件 |
| --- | --- | --- |
| ServiceContainer | ProcessHost 唯一持有 | 所有入口、执行器和工作线程结束后销毁 |
| ProviderRegistry | CompositionRoot 构造的不可变对象 | 进程收尾 |
| OperationState | 应用用例与执行器共享 | HTTP 返回后，迟到工作退出才释放最后引用 |
| ClientLease | 执行实际 I/O 的 worker 独占 | 请求真实结束并回收／关闭连接 |
| TaskRunner | 调度器管理句柄，Runner 持自身执行状态 | 进入终态、等待重试或安全停机 |
| FileWorkspace | 当前 Runner 独占 | 成功发布／用户取消清理／交给恢复流程 |
| StreamBridge | 上游 worker 与媒体 responder 共享 | 双方退出，剩余缓冲释放 |
| Subscription | EventHub 注册表与 responder 共享 | 标记关闭、从注册表移除、发送线程退出 |
| CredentialLease | 单次平台操作独占秘密快照 | 操作结束，不作为全局当前账号 |

默认值传递与唯一所有权；跨线程共享生命周期时使用共享所有权。

锁规则：短时间修改内存状态，释放锁后再做网络、磁盘或数据库等待；注册表锁只保护索引，不在持锁期间向所有订阅写 socket。数据库事务也不跨越网络与文件处理。

上游 worker 不能向同一个已满的上游池提交子任务后同步等待。Provider 已在上游 worker 执行时，其内部串行 HTTP 调用直接消费该 worker 和相应连接额度；需要并发时由外层规划后派发。processing worker 不等待下载 worker，数据库 writer 不等待任何业务 worker。

## 6. 持久实体与索引补充

表目录见[数据模型](data-models.md)，索引和事务规则如下。

| 实体／关系 | 关键唯一性与索引 | 目的 |
| --- | --- | --- |
| download_tasks | id 唯一；owner/status/created_at/id；status/next_retry_at | 查询分页、公平调度与到期领取 |
| 活动任务指纹 | owner + fingerprint 在活动状态内唯一 | 并发创建不能绕过去重检查 |
| batch_items | batch_id + ordinal 唯一；task_id 有反查索引 | 保留输入顺序，允许多批引用同一任务 |
| task_attempts | task_id + attempt_no 唯一，execution_id 唯一 | 执行追踪与旧 worker 拒写 |
| idempotency_records | owner + method + route + key 唯一 | 并发提交只有一个结果 |
| file_assets | file_id 唯一；owner + created_at；目标路径唯一 | 文件获取、资产追踪和路径冲突处理 |
| finalization_intents | task_id 唯一；发布状态索引 | 重启优先恢复未完成发布 |
| credentials | id 唯一；owner + source；每 owner/source 最多一个默认账号 | 账号选择稳定且可验证 |

所有任务所属批次关系以 batch_items 为准；task 不使用一个可变 batch_id 列来表达多对多关系。批次统计实时按其条目关联任务快照计算；条目重复时计数按条目，取消时按唯一 task_id 执行。

fingerprint 在同一事务中检查与插入，不能只先 SELECT 再在另一个事务 INSERT。已完成结果复用还需校验资产存在性；文件系统校验不持长事务，提交前再次核对相关任务／资产版本，不能把一次旧检查当成永久存在保证。

### 6.1 事务边界

| 事务 | 必须一起提交 | 提交外的工作 |
| --- | --- | --- |
| 创建任务／批次 | 幂等响应、请求快照、任务、批次条目、配额检查 | 参数解析、必要上下文补全；提交后调度唤醒 |
| 领取任务 | execution_id、attempt、状态、版本和领取记录 | 平台请求、文件准备 |
| 取消请求 | cancel_requested、状态、版本 | 提交后停止句柄与事件通知 |
| 进度检查点 | 当前公开进度、版本、可恢复证据 | 字节接收、文件刷写策略 |
| 开始发布 | 状态 finalizing、目标预留、发布意图 | 文件重命名、验证 |
| 完成发布 | file_assets、task_files、completed、意图完成标记 | 提交后 SSE 通知 |

检查点中的 received_bytes 是公开传输进度，不等同于断电后必定存在的持久字节。恢复时以工作文件实际长度和资源证据重新验证；需要强恢复点时文件刷写先于相应恢复证据提交。

### 6.2 凭证和秘密提交

先把新秘密写到新的不可变 secret_ref，再事务更新凭证描述与版本，最后使相关缓存失效并清理不再引用的旧秘密。DB 提交失败时新秘密成为待回收孤立项，旧有效凭证仍可使用；不能先覆盖旧秘密后让 DB 失败。

删除先使数据库凭证不可新租用，再清理秘密文件。清理失败记录待重试，不重新暴露已删除的凭证。日志和任务快照只记录 credential_id/version，不记录秘密正文。

## 7. 文件发布与崩溃矩阵

Finalizer 的输入是通过验证的临时产物和已确认策略，输出是不可变资产。发布意图记录 task_id、execution_id、工作区相对标识、预留目标、大小／内容证据和必需产物集合。

| 中断位置 | 重启可见状态 | 恢复动作 |
| --- | --- | --- |
| 写发布意图前 | processing 或旧检查点 | 校验工作区，重新处理或下载，不猜测已发布 |
| 意图已提交，尚未移动 | finalizing + 临时产物 | 校验后继续发布，核对目标预留 |
| 文件已移动，资产事务未提交 | finalizing + 目标文件 | 用意图证据确认文件，补提交资产与终态 |
| 资产事务已提交，SSE 未发出 | completed + file_id | 返回已完成事实，重连查询恢复显示 |
| 临时与目标同时存在 | 有意图，位置冲突 | 根据证据确认归属，不覆盖不属于本任务的文件 |
| 临时与目标都不存在 | 有意图，无产物 | 报告恢复错误，按允许策略重做；不得标 completed |

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
