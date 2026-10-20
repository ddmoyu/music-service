# C++20 后端详细架构图

更新：2026-10-19。总体评估与图 1 见[架构评估与总览](architecture.md)。本页依据当前工作区设计规范绘制；除图 2 的 CMake 依赖已有骨架外，业务组件均待实现，不代表已完成运行验证。

所有视图已导出为可缩放 SVG，见[图集](diagrams/README.md)。本页 Mermaid 是图 2～7 的编辑源，修改后按图集中的命令重新生成。

| 视图 | 主要问题 | 箭头语义 |
| --- | --- | --- |
| [图 2：编译依赖](#图-2编译依赖与依赖倒置) | 哪个模块依赖哪个模块？ | 实线为现有目标依赖，虚线为待实现组装 |
| [图 3：用例与平台](#图-3应用用例平台能力与授权) | 谁负责业务规则、平台协议和账号？ | 运行调用，虚线为可选扩展 |
| [图 4：下载时序](#图-4从接收下载到发布文件) | 任务如何持久化、执行和完成？ | 时间顺序、调用与返回 |
| [图 5：调度与资源](#图-5执行器上游额度与背压) | 执行槽和网络额度如何分开管理？ | 派发、申请、反馈与持久延后 |
| [图 6：发布与恢复](#图-6文件发布取消与崩溃恢复) | 文件与数据库之间崩溃怎么办？ | 正常步骤与分支，虚线为恢复入口 |
| [图 7：媒体与事件](#图-7文件在线媒体与任务事件) | 文件、音频和任务进度如何到客户端？ | 请求、字节与通知，按边标签区分 |

图按问题拆开；省略重复连线不表示绕过授权、端口或资源预算。Service 是进程内用例名称，不表示独立服务。可选能力必须按阶段验证后启用。

## 图 2：编译依赖与依赖倒置

[打开 SVG](diagrams/detail-1.svg)

```mermaid
flowchart TB
    composition["apps/server：CompositionRoot 与 ProcessHost，待实现"]
    subgraph adaptersLayer["外层模块：当前均为 INTERFACE 构建目标"]
        api["api / music_dl_http_api"]
        media["media_server"]
        events["event_server"]
        providers["providers"]
        downloads["downloads"]
        audio["audio_processing"]
        infrastructure["infrastructure"]
        runtime["runtime"]
    end
    application["application：用例契约、出站端口与上下文"]
    domain["domain：值对象、纯规则、状态迁移、意图指纹"]
    composition -.-> api
    composition -.-> media
    composition -.-> events
    composition -.-> providers
    composition -.-> downloads
    composition -.-> audio
    composition -.-> infrastructure
    composition -.-> runtime
    api --> application
    media --> application
    events --> application
    providers --> application
    downloads --> application
    audio --> application
    infrastructure --> application
    runtime --> application
    application --> domain
```

实线对应 `cpp/src/*/CMakeLists.txt` 的直接业务目标依赖，省略统一编译选项 target 及 domain 的传递依赖。组装层也负责构造应用用例，图中省略这条重复组装关系。`apps/server` 当前只有预留 CMake 文件，没有 `music-dl-server` 可执行目标。

**application 运行时调用 Provider，但不编译依赖具体 providers 实现。** 应用层声明端口，外层实现它，组装层注入具体对象。同理，任务创建通过 DownloadQueuePort 唤醒下载模块；Runner 通过应用契约调用解析用例，双方不包含对方的具体实现。

实际源码加入后，公共头只暴露项目类型，第三方依赖按 PUBLIC／PRIVATE 隔离。不能仅靠当前统一 include 根目录和空 INTERFACE target 证明这些约束已经生效。详见[模块与目录](backend-service/modules-and-layout.md)。

## 图 3：应用用例、平台能力与授权

[打开 SVG](diagrams/detail-2.svg)

```mermaid
flowchart TB
    control["控制入口：DTO、principal、scope、请求预算"]
    runner["后台 Runner：独立 TaskExecutionContext"]
    subgraph useCases["application：统一业务语义"]
        catalog["目录：搜索、集合、发现、歌词、链接解析"]
        assets["资源：Resolve、Probe、Match 与品质策略"]
        accounts["CredentialService：账号选择、租用、刷新合并、条件提交"]
    end
    subgraph providerAdapters["providers：启动校验、冻结注册、按需实例化"]
        registry["ProviderRegistry：描述与类型化能力选择"]
        native["原生 ProviderBundle：酷我优先，其他逐个接入"]
        auth["可选授权工厂：Verifier、Refresher、ApplicationTokenProvider"]
        helper["可选 external adapter：版本化 helper 协议"]
    end
    transport["网络与进程适配：每次操作获取匹配的上游租约"]
    secrets[("SecretStore：秘密快照与不可变 secret_ref")]
    credentials[("CredentialRepository：绑定、引用与版本")]
    upstream["平台 API、授权端点与 CDN"]
    control --> catalog
    control --> assets
    control --> accounts
    runner -->|"后台解析与探测"| assets
    catalog -->|"选择当前账号"| accounts
    assets -->|"按绑定租用"| accounts
    accounts --> secrets
    accounts --> credentials
    catalog -->|"按能力调用"| registry
    assets -->|"按实际目标能力调用"| registry
    registry --> native
    registry -.-> helper
    accounts -.->|"按注册的 auth_profile 选择"| auth
    native --> transport
    auth --> transport
    helper -.-> transport
    transport --> upstream
```

AccountBinding 固定账号选择；AuthIdentityKey 包含绑定与本次完整授权版本。缓存、游标、上下文、在途合并和连接隔离共用该身份，不能只用相同的 version 值区分不同账号。平台能力取得只读秘密快照，不能写凭证表或 SecretStore；CredentialService 负责刷新合并与条件提交。

Provider 只解释平台请求、签名、响应及能力，不拥有通用换源策略、任务队列或文件发布。目录源和实际音频源分开保存：未来目录型来源由 MatchService 明确选择目标后，再调用目标来源 IAssetResolver，不能在 Provider 内部偷偷跨源递归。

首版静态注册原生平台。helper 默认关闭，启用后也共用公共 Runner、临时工作区、取消、配额与 Finalizer；不引入第二套持久队列。授权工厂、处理器注册、扩展传输类型和新增平台验收详见[多平台扩展](backend-service/provider-extensibility.md)。

## 图 4：从接收下载到发布文件

[打开 SVG](diagrams/detail-3.svg)

```mermaid
sequenceDiagram
    participant client as 客户端
    participant command as 控制入口与命令用例
    participant repo as Repository / SQLite
    participant runner as Scheduler / Runner
    participant resolve as 解析用例与 Provider
    participant io as 入场 / 传输 / 工作文件
    participant finish as Processor / Finalizer
    participant events as EventHub / SSE
    client->>command: POST 下载 + Idempotency-Key
    command->>repo: 查询既有幂等结果
    command->>command: 新请求归一意图，固定账号与手动目标
    command->>repo: 短事务复核版本、配额、指纹并写任务
    repo-->>command: 提交结果
    command-->>client: 新建成功返回 202 与 task_id
    command->>runner: 提交后唤醒，扫描兜底
    runner->>repo: 条件领取，产生 execution_id 与 version
    runner->>io: 准备首个解析操作并申请租约
    alt 无法立即入场
        io-->>runner: Deferred 与恢复位置
        runner->>repo: 无旧 I/O 后提交 waiting_retry 与检查点
        runner->>runner: 释放执行槽，到期后重新领取
    else 获准开始新逻辑尝试
        runner->>repo: begin_attempt 提交计数与固定截止
        runner->>resolve: 按固定绑定解析，每个上游操作独立入场
        resolve-->>runner: 实际曲目、资源计划、品质与身份依据
        runner->>io: 另申请传输租约，核对响应头并逐块写盘
        loop 进度检查点或状态变化
            runner->>repo: execution_id + expected_version 条件提交
            repo-->>runner: 已提交公开快照
            runner->>events: 有序回调入队，不写客户端 socket
        end
        runner->>io: 等待 I/O 真正结束并归还网络额度
        runner->>finish: 解密、验证、按策略写标签
        finish-->>runner: 已验证产物与处理报告
        runner->>repo: 条件进入 finalizing，提交发布意图
        runner->>finish: 禁止覆盖的同文件系统发布
        finish-->>runner: 文件位置与验证证据
        runner->>repo: 提交资产关系、completed 与新版本
        runner->>events: 入队已提交终态
        events-->>client: task.updated；断线用 HTTP 同步
    end
```

图展开新任务路径；幂等命中直接返回既有结果。创建、领取、开始尝试、检查点、发布意图、完成发布各自是短事务，均不包住网络或文件处理。图把入场与传输归在一个参与者：实际额度属于 runtime，I/O 属于适配器，准备阶段与实际执行之间通过应用契约协调。

`attempt` 是逻辑尝试次数，`execution_id` 是领取世代，`version` 是持久快照版本。入场延后不消耗新尝试次数，重新领取更换执行世代；旧回调不得覆盖新执行。图只展开首个操作的 Deferred 分支，后续解析、探测、传输也遵守相同规则；进入 processing 前已释放网络额度。完整契约见[内部接口](backend-service/internal-contracts.md)。

关闭客户端只会断开响应或通知，不取消已提交任务。任务事件可能早于创建响应到达，客户端按 task_id 与 version 合并。

## 图 5：执行器、上游额度与背压

[打开 SVG](diagrams/detail-4.svg)

```mermaid
flowchart TB
    control["控制池：昂贵操作按 admission 配置"]
    coordinator["短请求协调：有界队列，共享请求截止"]
    scheduler["downloads/Scheduler：身份公平性、领取、配置执行槽"]
    taskStore[("持久任务：queued / waiting_retry 与恢复证据")]
    media["媒体池：按 media_limits 限制响应"]
    admission["runtime/admission：唯一上游额度账本"]
    shortPool["短请求池：按 upstream 限制来源并发"]
    downloadPool["下载 Runner：按 downloads 限制传输并发"]
    mediaPool["可选媒体上游池：建流不排长队"]
    transport["Transport：核对租约、目标与身份后执行"]
    processing["processing 池：有界队列"]
    writer["DB：单 writer，有界队列与读取连接"]
    control --> coordinator
    coordinator -->|"申请；Deferred 回到有界协调队列"| admission
    scheduler -->|"条件领取"| taskStore
    scheduler -->|"Runner 按实际操作申请"| admission
    media -->|"无额度在发头前拒绝"| admission
    admission -->|"获租约后派发"| shortPool
    admission -->|"获租约后推进"| downloadPool
    admission -->|"获租约后建流"| mediaPool
    shortPool --> transport
    downloadPool --> transport
    mediaPool --> transport
    transport -->|"真实结束后归还；报告限流反馈"| admission
    downloadPool -->|"Deferred：保存后释放执行槽"| taskStore
    downloadPool -->|"先释放网络额度，再处理"| processing
    downloadPool -->|"状态与检查点"| writer
```

配额数值统一见[配置基线](backend-service/configuration-and-deployment.md#配置来源与生效时机)，图只表示职责；媒体上游随在线代理能力启用。admission 的输出表示获得资格后的派发关系，不表示它解释或领取任务。并发租约、速率冷却、物理连接池容量和下载执行槽分别管理。Provider 内部准备下一条网络操作时同样非阻塞入场，不能在 worker 内等平台 semaphore。

允许的等待方向是控制 → 短请求、下载 → processing／DB。禁止反向等待、同池提交后阻塞等子任务，以及持锁或持数据库事务等 I/O。processing 可以占用下载执行槽，但不能继续占用下载网络租约。

Deferred 持久延后之前必须确认旧 I/O 结束；DB 提交失败不能先释放执行所有权。重定向每跳重新校验和入场；停止信号、HTTP 已返回和真正 I/O 结束不是同一时刻。外部 helper 按已验证的连接上界联合预留，实际退出后归还。完整规则见[运行时](backend-service/runtime-and-tasks.md)。

## 图 6：文件发布、取消与崩溃恢复

[打开 SVG](diagrams/detail-5.svg)

```mermaid
flowchart TB
    verified["处理完成：临时产物已通过验证"]
    gate{"Repository 条件转换：取消还是发布先提交？"}
    cancel["canceling：等待真实工作退出，清理后 canceled"]
    intent["事务 A：finalizing、路径预留、finalization_intent"]
    move["文件操作：禁止覆盖，同文件系统重命名并验证"]
    commit["事务 B：file_assets、task_files、completed、意图完成"]
    event["提交后通知 SSE；HTTP 可读持久结果"]
    restart["重启：先恢复未完成发布意图"]
    evidence{"核对意图、文件位置、大小与身份依据"}
    retryMove["只有合法临时产物：继续发布"]
    finishCommit["已有匹配目标文件：补提交完成事务"]
    conflict["两处均有文件或未知目标：按证据处理，禁止覆盖"]
    missing["产物不存在或无法验证：报告恢复错误，不标完成"]
    verified --> gate
    gate -->|"取消先提交"| cancel
    gate -->|"发布先提交；之后取消返回 409"| intent
    intent --> move
    move --> commit
    commit --> event
    restart -.-> evidence
    evidence --> retryMove
    evidence --> finishCommit
    evidence --> conflict
    evidence --> missing
    retryMove --> move
    finishCommit --> commit
```

SQLite 与文件系统之间没有跨资源原子事务。发布意图让崩溃窗口可识别、可验证和可继续；无法确认产物时报告错误，而不是猜测成功。DB 已提交但 SSE 通知失败不回滚任务；关闭受影响订阅，让客户端重新查询。

恢复不仅检查 `.part` 长度，还检查任务版本、资源身份和产物依据。工作区在 destination 内，不能默认放在可能跨文件系统的 data_dir。其他阶段的恢复和停机规则见[运行时](backend-service/runtime-and-tasks.md#崩溃恢复与优雅退出)，发布故障矩阵见[内部契约](backend-service/internal-contracts.md#7-文件发布与崩溃矩阵)。

## 图 7：文件、在线媒体与任务事件

[打开 SVG](diagrams/detail-6.svg)

```mermaid
flowchart TB
    client["客户端：播放器、本机保存、任务视图"]
    access["media-access：校验既有 asset_id / file_id，按需签票据"]
    subgraph mediaPath["media_server：独立 HTTP 池与响应额度"]
        auth["校验身份或票据、归属、期限与 Range"]
        file["FileResponder：只读文件租约"]
        bridge["可选 StreamBridge：每流有界缓冲"]
    end
    assets[("已发布文件资产：后端 destination")]
    upstream["可选上游 worker：先入场、校验响应头，再发送字节"]
    direct["可选安全直链：客户端直接访问 CDN"]
    subgraph eventPath["application/events：共享事件状态"]
        hub["TaskEventService / EventHub：按身份过滤已提交快照"]
        queue["每订阅队列：事件数与字节数双重上限"]
    end
    responder["event_server：独立池与订阅额度；发送、心跳、断开"]
    commit["任务状态或进度提交成功"]
    query["TaskQueryService：读取 SQLite 同源快照"]
    client <-->|"获取访问描述与后端 URL"| access
    client <-->|"GET / Range 与文件或音频响应"| auth
    auth -->|"file_id"| file
    file -->|"读取"| assets
    auth -.->|"asset_id；代理启用时"| bridge
    bridge -.->|"受限生产与消费，先校验头"| upstream
    access -.->|"满足外发条件"| direct
    client -.->|"直链读取，不占后端传输额度"| direct
    client <-->|"SSE 订阅与 task.updated"| responder
    responder -->|"注册与注销"| hub
    commit -->|"有序短回调"| hub
    hub --> queue
    responder -->|"读取已提交的有界通知"| queue
    client <-->|"首次同步、重连或 HTTP 兜底"| query
```

文件发送不占媒体上游线程。代理必须先核对上游状态与 Range 再向下游发头，缓冲满时限制生产者继续读取；客户端断开只取消这一次流。直链默认关闭，需秘密请求头的资源不能外发。播放位置、暂停、音量、解码与本机写盘进度属于客户端。

SSE 可以合并未发送进度，但不能静默丢弃终态；超限时关闭订阅，重连后先收到 ready，再读取列表及已经追踪的任务，按 version 合并事件。首版不建持久事件日志，也不依赖 SSE 保证任务执行。**后端 completed、媒体传输结束、客户端保存成功是三个不同事实。**

## 状态归属与生命周期

| 数据或状态 | 权威持有者 | 重启与对外边界 |
| --- | --- | --- |
| principal、token 哈希、scope | SQLite 身份记录 | 引导文件只做一次性交付 |
| 凭证描述、稳定绑定、版本 | CredentialService + Repository | 秘密放 SecretStore，对外仅去敏描述 |
| 任务、规范意图、attempt、deadline、version | SQLite 任务与尝试记录 | 新领取产生 execution_id，旧执行不得写入 |
| 批次与任务关系 | batch_items | 多批可引用同任务，取消真实任务影响全部引用 |
| 文件资产、任务文件关系、发布意图 | SQLite + 经核验的实际文件 | file_id 与 task_id 分开，路径不是客户端本机路径 |
| context、cursor、asset、match、媒体票据 | 有界内存句柄与缓存 | 重启失效，持久任务不依赖短期句柄续命 |
| ProviderRegistry | 组装层创建的不可变对象 | 启动校验、按需实例化，首版不热加载 |
| 网络租约、执行句柄、订阅、StreamBridge | runtime 与当前操作 | 不恢复旧网络租约，实际退出才释放 |
| 播放位置、本机保存进度 | 外部客户端 | 不写入 DownloadTask 或另建后端播放会话 |

字段和关系以[数据模型](backend-service/data-models.md)为准，事务和所有权以[内部契约](backend-service/internal-contracts.md)为准。图用于解释规范，不另建第二套 API 定义。
