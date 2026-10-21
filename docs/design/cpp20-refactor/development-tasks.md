# 开发任务拆分与验收计划

更新：2026-10-20。本文将当前设计拆成 41 项公共能力／阶段验收任务和 12 项后续平台任务。T01～T15 **已完成**，S0 基础服务关口通过；其余全部**未开始**；状态与证据见文末[实施记录](#实施记录)。

本文管理实施顺序、依赖与交付关口；规则和字段仍以[模块设计](backend-service/feature-modules.md)、[内部契约](backend-service/internal-contracts.md)、[公共模型](backend-service/data-models.md)、[API](backend-service/api-contract.md)和[运行时](backend-service/runtime-and-tasks.md)为准。A01～A50 的完整场景以[验收矩阵](backend-service/flows-and-acceptance.md#7-故障与验收矩阵)为准；本文只分配落实任务，不另建一套行为规范。

## 任务使用规则

- `T` 表示公共能力或阶段验收任务，`N` 表示已研究的后续原生平台，`X` 表示开放扩展候选。编号保持稳定，后续可拆子任务，不因调整优先级重新编号。
- 表内“直接前置”表示集成交付所必需的任务，已由所有可选路径保证的传递依赖不重复列出；仅在部分候选路径下被覆盖的前置仍保留。`—` 表示没有前置，`ANY(...)` 表示任选一项完成，并在验收记录中固定所选项。确定接口后可用替身并行开发，但前置未验收时不能把依赖它的任务标记为完成。
- 平台优先顺序与技术依赖分开记录。后续平台不天然依赖排在前面的所有平台；一个来源被账号、地区或接口阻塞时，可推进其他已具备条件的任务。
- 状态采用未开始、进行中、受阻、待验收、已完成。受阻记录具体缺口与恢复条件；完成必须附实现版本、验证命令、结果及适用系统／账号／能力范围，不以文档存在、编译成功或 mock 通过代替真实交付。
- 可选功能和平台单独验收。没有完成的功能保持关闭，不列入支持清单；S2 的基础关口不要求所有平台上线，S3 也不等待所有平台扩展结束。

## 总体顺序与交付关口

首条交付主线是 **S0 基础服务 → S1 酷我下载闭环**。完成后分别推进平台扩展与下载可靠性；可靠性具备后接入匹配、在线媒体和发布集成。

| 阶段 | 推荐执行顺序 | 可并行的工作 | 通过关口 |
| --- | --- | --- | --- |
| S0 基础服务 | T01～T02 → T03～T09 → T10 → T11 | 端口确定后，文件／数据库、runtime、注册表、传输可分支推进；T07、T09 完成后即可开始 T12 | T11：Windows／Linux 和最小容器可独立启动、健康、停止 |
| S1 酷我闭环 | T12 → T13 → T14～T19 → T20 | T12 与 S0 后半并行；音频最小验证／发布、任务命令、事件和文件出口按各自前置推进 | T20：酷我标准品质可持久下载、重启恢复和取回文件 |
| S2 平台扩展 | T21 → 第二平台 → T41 → T23 → T28 | T22 优先，受阻可改选 N01～N08；T41 通过后优先 T25、T26～T27，T24 授权独立推进 | T28：两平台与集合基础通过；海外来源、授权和 helper 按启用组合另行验收 |
| S3 下载可靠性 | T29、T30 → T31 → T32 | T29／T30 在 T20 后即可开始，与 S2 平台扩展并行 | T32：续传、处理、重试与故障恢复通过，不以全部平台完成为条件 |
| S4 匹配与在线媒体 | T33 → T35；T34 独立推进 → T36 | 手动匹配与媒体代理可并行；Spotify 跨源组合依赖相应平台任务 | T36：所启用的匹配、代理／直链、自动策略分别有验收证据 |
| S5 发布集成 | T37 → T38 → T39 → T40 | S0 起积累部署证据；T32 后可开展正式打包，不必等待可选功能 | T40：选定发布组合的目标运行、升级和恢复通过 |

S0／S1 对扩展契约采用“端口完整、实现最小”的原则：AccountBinding、AdmissionRequest、ProviderBundle 等类型一次定好字段，但只实现酷我闭环实际需要的维度与模式；应用 token、共享应用配额组、helper 预算和大规模来源分别在 T24、T26、T28 实现并验收，不作为 T11／T20 的通过条件。

T20 可作为首个可用的酷我预览版本；正式发布基线按 T40 管理。阶段不是一个全平台串行队列：S3 基线通过不表示 S2 的所有扩展已完成，发布清单必须逐项列出已验收能力。以下任务表给出精确依赖，不能仅按编号推断前置关系。

## 模块与任务归属

| 模块／目录 | 首次任务 | 后续任务与边界 |
| --- | --- | --- |
| `domain`、`application/ports` | T02 | 各任务按需要补端口；领域不依赖 HTTP、JSON、SQLite 或系统 API |
| `infrastructure/config`、`observability`、`filesystem`、`secrets`、`storage` | T03～T05 | T31 恢复与清理；文件系统提供原语，下载发布流程由 downloads 持有 |
| `runtime` | T06 | T17 接入调度、T29 策略增强、T26 helper 配额；统一上游账本不复制到各模块 |
| `providers/registry`、`providers/<source>` | T07、T12 | T22／T41、T25、T27、N／X 平台；协议、签名、解析与平台授权在平台包内 |
| `application/accounts`、`infrastructure/auth` | T08 | T24 OAuth／刷新；前者协调平台凭证，后者处理服务 token，职责不同 |
| `infrastructure/transport` | T09 | T29 传输可靠性、T34 流桥接；不拥有第二套下载队列 |
| `src/server`、`api/httplib` | T10 | 随每项用例补路由；仅组装、生命周期和协议转换，不最后集中补 API |
| `application/catalog`、`application/assets` | T13 | T21／T23 集合与发现，T33／T35 匹配；保留原曲与实际来源 |
| `application/tasks`、`downloads` | T14、T16～T17 | T29／T31 强化；意图归一属于 application，执行与发布属于 downloads |
| `audio_processing` | T15 | T30 标签、容器与解密；不控制账号、任务调度或平台匹配 |
| `application/events`、`event_server/httplib` | T18 | 提交后通知与恢复快照，不保存另一份任务事实 |
| `application/media`、`media_server/httplib` | T19 | T34 在线代理／直链；本地文件出口不依赖在线代理开关 |
| `providers/external`、`infrastructure/external_tools` | T26 | 平台映射与进程管理分开，T27／X 任务按需使用 |
| `tests`、`packaging` | T01、T11 | 测试随功能交付，T20／T28／T32／T36 为集成关口，T37～T40 为发布任务 |

目录与编译依赖以[模块与目录](backend-service/modules-and-layout.md)为准，不为每个任务新建库，也不提前创建所有候选平台的空目录。

API 按功能随任务交付，路由与字段只在[API 契约](backend-service/api-contract.md)维护，下面用于防止漏项：

| API 范围 | 实施归属 |
| --- | --- |
| 地址／版本、权限、错误、请求预算与通用约定 | T10 建立中间件，各用例任务验证自身 scope、错误和幂等语义 |
| health、info、providers、limits、destinations | T10；平台注册／调用方能力分别依赖 T07／T08 |
| search、tracks/detail、assets/resolve、assets/probe | T13；多源由 T21 扩展，跨源解析由 T33／T35 扩展 |
| 集合、歌手、分类、榜单、推荐、歌词、links/parse | T21 提供公共用例和路由，T23 及各平台实现实际能力 |
| 下载创建／查询／取消／重试、有限批次 | T14 提供命令和查询，T17 执行；手动 retry 创建新任务，不重开旧终态 |
| 下载事件 | T18；HTTP 查询不依赖 SSE |
| 文件元数据、media-access、媒体字节路由 | T19 提供本地文件与票据，T34 增加在线资产访问 |
| credentials 与 credential-defaults | T08 提供凭证及默认选择用例，T10 接路由，T12 验证实际 Cookie，T24 扩展授权；服务 token 使用本机 CLI |
| admin/status、admin/download-policy | T10 提供基础状态，后续模块补实际指标；T17 增加暂停／恢复领取策略 |

公开 API 未实现时须按既有能力／功能开关契约响应，不用返回空成功的占位路由充当交付。

## S0 基础服务任务

| ID | 模块／任务 | 直接前置 | 交付范围 | 验收标准 |
| --- | --- | --- | --- | --- |
| T01 | 构建、依赖与验证基础 | — | 在既有骨架上确定首批库、TLS 组合、测试工具和工具链；接入真实库目标、FetchContent 固定 tag／commit、Windows／Linux 构建检查 | 干净目录可重复配置与编译；依赖 HEAD 不符即失败；不链接研究克隆；记录编译器／运行库版本、依赖许可证与体积；尚未实现模块仍不伪造功能 |
| T02 | 领域规则与最小应用端口 | T01 | S1 所需类型、Result、错误、品质、身份、意图／指纹、任务迁移，以及平台、存储、传输、执行器端口 | 公共头不泄漏第三方类型；独立样本覆盖 strict／试听／未知完整性、稳定身份、指纹与非法迁移；用替身验证端口，不提前实现 S4 匹配 |
| T03 | 配置、时钟与可观测性基础 | T02 | 默认值、配置校验、路径配置、预算所需时钟、结构化日志与关联 ID；为计数和延迟预留实际采集点 | 无效端口／配额／路径配置在启动前明确失败；日志不含 token、Cookie 或带秘密 URL；时钟可用于确定性超时验证，配置行为与部署文档一致 |
| T04 | 文件系统、工作目录与 SecretStore | T03 | data_dir／destination 分工、实例锁、路径约束、权限、不可变秘密引用与临时文件原语 | 第二实例拒绝；越界路径拒绝；中文路径与权限失败可定位；提供根规范化与安全文件创建原语，T05／T10 落实不可变目录绑定；秘密不进入普通输出 |
| T05 | SQLite 与仓储基础 | T04 | WAL、单 writer、有限读连接、迁移及身份／默认凭证／目录绑定／任务／资产／发布意图仓储 | 初始化／重开、事务回滚、唯一约束及版本更新通过；默认选择与凭证撤销原子提交；目录 ID 绑定根不可改；writer 不执行网络和文件处理；支撑 S1 恢复 |
| T06 | 执行器、取消与统一上游入场 | T03 | 有界队列、取消／deadline、UpstreamAdmissionPort 联合租约和停机原语；S0 实现全局、principal、source、origin 四个维度，共享应用配额组与 helper 上界维度只保留端口字段，分别由 T24／T26 启用 | 满额返回 Deferred 而不占 worker 等 semaphore；无部分预留泄漏；不同来源可推进；取消／超时释放资源；额度仅在真实操作结束后归还；A43 的运行时部分通过 |
| T07 | ProviderBundle 注册与能力发现 | T03 | 静态工厂、开放 source、细粒度能力、auth_profiles 绑定验证、按需实例化、少量模拟目录型／音源型来源 | 酷我占位与 2～3 个模拟来源覆盖目录型、音源型组合及 providers 分页；无每平台线程／启动网络探测；重复 source 或错误工厂绑定启动失败；未注册能力不暴露，增加现有能力来源不改公共路由。100 个来源的规模验证归 T28 |
| T08 | 服务身份与 Cookie 凭证基线 | T05、T06、T07 | Principal／AccessToken 管理用例、Cookie 凭证 CRUD、SQLite 默认选择、anonymous／user 两种模式的 AccountBinding／AuthIdentityKey、secret 提交与固定绑定租约；application 模式和应用 token 只保留字段，由 T24 实现 | A06、A34、A38～A39 的用户凭证部分及 A47～A48 用例通过；默认选择可设置／清除、重启保留，撤销不隐式改选；同版本账号隔离；身份命令的实际 CLI 接线归 T10 |
| T09 | HTTP／TLS 上游传输 | T06 | httplib Client 适配、流式读取、请求预算、取消、重定向逐跳校验与租约、敏感头处理 | 本地上游验证 GET／POST、断流、错误证书链／主机名、跨 origin 重定向和取消；响应不整段缓冲入内存；每跳真实 source／origin 计量正确，无秘密头错误传播；未声明的明文 HTTP 端点被拒绝，HTTPS→HTTP 降级跳转默认拒绝，明文请求不携带秘密 |
| T10 | 组装入口与基础 HTTP／管理 CLI | T08、T09 | ProcessHost、三入口与中间件、系统／凭证／默认选择路由、基础 admin/status；init 与离线 principal／token 命令 | T08 用例通过 API／CLI 可操作；A47～A49 基础验收通过；实例运行时管理命令拒绝，明文仅安全文件交付；同 ID 改根启动失败；正常停止 DB 最后关闭，无 GUI 依赖 |
| T11 | S0 集成与最小部署关口 | T10 | Windows x64／MSVC、Linux x86_64／GCC 的构建与运行，以及最小 Docker 启动证据 | 无前端、桌面会话或音频设备也可启动／健康／停止；三个入口与配置一致；记录证书、运行库、架构与命令；空工程编译或交叉编译不能代替运行验收 |

## S1 酷我下载闭环任务

| ID | 模块／任务 | 直接前置 | 交付范围 | 验收标准 |
| --- | --- | --- | --- | --- |
| T12 | 酷我标准品质协议包 | T07、T09 | 脱敏样本、请求／解析、descriptor、搜索、详情／链接和标准品质音源解析；逐端点确认可用的 HTTPS 地址，仍需明文的端点按传输规则登记；仅按实证注册凭证验证 | 平台契约与受限在线协议探测通过，格式／品质／试听证据明确；本任务只验协议包，不依赖持久下载，可与 T08、T10、T11 并行，以尽早暴露平台接口风险。产品下载、票据和恢复统一在 T20 验收；不依赖 relay 或高品质 |
| T13 | 搜索、详情、解析与缓存用例 | T11、T12 | 单源目录与资产用例、API 路由、游标、完整身份分区的缓存／singleflight／上下文、必要探测 | HTTP 契约含权限、错误与分页；A06、A38～A39 的缓存／上下文部分通过；默认账号切换使旧游标失效；过期音源不能作为有效解析结果，无自动换源 |
| T14 | 下载意图、命令、查询与批次 | T13 | 事务外意图归一、指纹、幂等、活动唯一约束、已完成资产复用复核、提交／取消／手动重试／查询 API、有限批次 | A01～A03、A35～A37、A46 的领域／仓储契约通过；同 key 重放先取原结果，不重读过期句柄；策略不同不误复用；手动 retry 创建新任务；并发有版本保护；匹配先用持久目标替身验证 |
| T15 | 最小音频文件验证 | T12 | 酷我首批实际容器的识别、必要长度／结构与资源证据校验，统一 AudioProcessor 结果；尚不写真实标签 | HTML／JSON 错误体、明显截断或不支持内容不能作为成功音频发布；未知长度维持未知；不通过 URL 可达推断整曲；验证失败按 required 行为阻止发布 |
| T16 | 安全发布与最小重启恢复 | T11、T15 | 发布意图、目标预留、临时文件到最终文件流程、资产记录、取消／发布竞争、启动恢复 | A12、A17、A19 的基础验证、A20 通过；模拟文件移动后退出可依意图补提交；不覆盖未知目标；无安全续传证据的半文件允许重下，不能盲目追加 |
| T17 | 持久调度与最小下载执行 | T14、T16 | 公平领取、Runner、固定账号绑定、执行代次、有限重试、流式写盘、取消、admission 延后、停机检查点和 admin/download-policy | A04、A22、A25～A27、A43～A44 通过；Deferred 持久化后释放槽，重新领取换 execution_id 不增加 attempt；deadline 不延长；暂停只停止新领取，既有任务继续；关闭客户端仍下载 |
| T18 | 事件、SSE 与 HTTP 同步 | T14 | 提交后事件、按 principal 订阅、有界缓冲、连接配额、心跳与重连同步；HTTP 查询独立可用 | A07～A11、A21、A28 通过；SSE 满额／慢消费者不拖住控制请求或 Runner；用最小协议客户端验证先事件后响应、迟到版本和断线恢复，不要求开发正式前端 |
| T19 | 已完成文件访问与票据 | T16 | FileAccess、文件元数据、媒体授权／访问描述、票据、file_id 路由、磁盘文件 Range 与断开回收 | A23、A24 的本地文件部分、A32 的票据部分、A33 通过；只开放已发布文件，缺失文件保留历史并标记；其他 principal 或过期票据拒绝；关闭代理仍可取文件；路径和秘密不外泄 |
| T20 | S1 酷我端到端关口 | T17、T18、T19 | 搜索 → 详情／解析 → 持久任务 → SSE／HTTP → 文件获取；整合身份、归一、入场、恢复验证 | Windows／Linux 和容器执行无前端闭环；在线结果与离线故障证据分开；关闭客户端、重启、取消、文件发布、仅 HTTP 查询均通过；A29 用独立客户端模拟保存失败；不将 mock 匹配／标签记成已上线功能 |

## S2 多平台与目录扩展任务

| ID | 模块／任务 | 直接前置 | 交付范围 | 验收标准 |
| --- | --- | --- | --- | --- |
| T21 | 多源、集合、发现与歌词用例 | T20 | 多源搜索、逐源分页、歌单／专辑／歌手、分类／榜单／推荐、歌词／封面与链接解析端口及 API，按能力路由 | A05 通过；单源超时保留其他结果；来源数和并发有界；unsupported／disabled 明确区分；目录型替身无音源仍可用；短链只在允许范围展开；不后台枚举完整歌单或增加平台路由分支 |
| T22 | 5sing 原生平台接入 | T21 | 作品编号／类型复合身份、搜索／详情／解析、主备资源与样本；利用已有下载服务完成集成 | 平台包及在线下载／文件验收通过；缺失时长保留未知，不混合主备文件。T22 只代表 5sing；公共扩展验证归 T41，T22 受阻不阻塞其他原生来源 |
| T41 | 第二个真实平台扩展关口 | ANY(T22,N01,N02,N03,N04,N05,N06,N07,N08) | 使用酷我和任选一个已完成的原生扩展平台，验证注册、能力差异、账号隔离和公共下载复用；优先采用 T22 | 两个真实来源通过，不改通用路由／状态机／平台专表；记录所选平台任务和证据。任一候选受阻可换另一项；候选自身不依赖 T41，不能拿模拟平台替代真实证明 |
| T23 | 已接平台的集合功能落地 | T41 | 对实际通过 T41 的来源补支持的歌单／专辑／歌手／分类／榜单／推荐／歌词／链接，按页提交有限批次 | 每种操作有契约样本；上线能力另有真实验证；分页不串源；无歌词与不支持歌词分别响应；不强求各平台具备全部集合能力 |
| T24 | OAuth、API key 与刷新扩展 | T20 | 按首个需要的平台补授权 profile、Verifier／Refresher／ApplicationTokenProvider、刷新合并、CAS、应用 token 缓存和稳定配额组 | A40～A41 通过；并发刷新仅一次在途，等待者可分别超时；更新／撤销赢过旧结果；不同 scope 的 token 独立身份；无同池递归等待；新授权协议只扩平台包／注册，不修改凭证服务的平台分支 |
| T25 | Spotify 原生目录 Provider | T24、T41 | 先核对当前应用权限，再接搜索、曲目／专辑／歌单、分页及链接解析；保留 original_track | 离线分页／授权／限流契约通过，实际可用端点另有在线证据；目录能力单独可用，resolve_audio 明确不支持；同源整曲下载和静默跨源匹配不进入本任务；受应用权限限制的能力不宣称支持 |
| T26 | 可选外部 helper 基础设施 | T41 | 版本化有类型协议、工具版本校验、extractor／域名范围、受限参数、工作目录、进程与输出限额、取消和 admission 上界计量 | A45 的 helper／共享配额部分通过；启动失败、输出异常、超时／崩溃、子进程退出可回收；真实退出前不归还额度；没有第二套任务队列；关闭 helper 的核心发行包无需 Python／FFmpeg |
| T27 | YouTube 音源 adapter | T26 | 通过可选 yt-dlp adapter 提取已验证的独立 HTTP 音轨，交给公共传输；稳定视频身份、品质和过期地址处理 | 固定样本与有权限在线样本分别验证；音频真实格式／身份正确，取消与凭证隔离有效；只能分段或必须转码的结果明确不支持；ExternalTransfer 如需启用须单独补验证，不能因 helper 可下载就宣称通用支持 |
| T28 | S2 基础扩展关口 | T23 | 两个真实平台、集合／发现契约、多源部分失败、100 来源有界发现／调用、上下文升级和平台禁用后的恢复 | 声明能力与 API 一致；未使用来源无线程／网络开销；旧上下文按稳定引用重解析或明确失败；禁用来源不静默换源，已发布文件仍可读；新增现有能力来源不改通用业务。T24～T27 启用时另验收，不因受阻拖住 S3 |

T25 优先于 T27 是产品顺序；T27 在技术上不依赖 Spotify。T24 的协调逻辑可先用模拟授权平台验证，Spotify 的实际协议和权限在 T25 验收，避免形成相互等待。T26 不默认引入外部下载／转码全能力。

## S3 下载可靠性与文件处理任务

| ID | 模块／任务 | 直接前置 | 交付范围 | 验收标准 |
| --- | --- | --- | --- | --- |
| T29 | 续传、重试与速率策略增强 | T20 | Range 校验、资源一致性、过期 URL 重解析、429／5xx 冷却、重试与持久预算、限速和取消竞争 | A13～A16、A22、A25～A26、A43～A45 的传输／预算部分通过；200 不追加、错误 206 不混合、416 不直接成功；恢复不重置 deadline／attempt；一个来源冷却不阻塞其他来源 |
| T30 | 容器、标签与解密处理扩展 | T20 | 选择并固定标签库，按 MP3／FLAC／M4A 能力补验证、标签／封面／歌词写入、侧文件和解密端口；处理器按真实平台需要增加 | A18～A19、A35 的真实文件部分通过；重复写入、未知标签、载荷保真、临时空间与回退有证据；optional 失败有警告，required 失败不发布；首批平台不需要的解密只验证端口，不能标记为已支持 |
| T31 | 故障恢复、清理与迁移强化 | T29、T30 | 发布各崩溃窗口、侧文件一致性、磁盘耗尽／目标离线、过期工作文件清理、资产版本复核、数据库迁移与备份恢复 | A12、A17、A20～A22、A27、A46 在故障注入下通过；清理不删除在用文件；重启不重复下载／误覆盖；资产复用检查与提交之间变化不会返回错误 reused=true；迁移失败有可验证恢复路径 |
| T32 | S3 可靠性与资源关口 | T31 | 组合验证断流、重启、磁盘、发布、处理、限速和多任务运行；记录资源与延迟 | 所有启用格式／处理器有独立样本；线程、连接、队列、内存均符合配置上界；故障无伪完成或混合文件；给出运行参数和实测结果，不以“高并发”代替指标；不等待 N／X 平台全部完成 |

T30 的处理器可用本地元数据样本开发；接入在线歌词／封面时，附加依赖 T21 及对应平台能力的完成证据。平台特有解密在 N08／N09 等任务落实，避免核心处理模块反向等待平台。S1 的安全发布一直保留，S3 不用于补救一个允许错误完成的首版。

## S4 匹配与在线媒体任务

| ID | 模块／任务 | 直接前置 | 交付范围 | 验收标准 |
| --- | --- | --- | --- | --- |
| T33 | 手动匹配与稳定目标保存 | T20、T41 | 候选查询、版本／身份排除规则、证据和上下文、用户显式目标、original_track／actual_track、稳定意图持久化 | A36～A37 的真实用例通过；同名／翻唱／现场／Remix 不误认；两个临时句柄选择同目标产生相同意图，目标不同则不同；受理后过期／重启不丢目标，重放不重读句柄 |
| T34 | 在线代理、受控直链与流桥接 | T32 | 在线资产访问描述、proxy／direct／auto、背压、Range、刷新与稳定资源身份、媒体池与文件出口隔离 | A24 的上游部分、A30～A33 通过；秘密头不交给客户端；票据与资产过期分别处理；慢读／断开回收，URL 更新不拼接不同音频；两个客户端不共享播放状态 |
| T35 | 受控自动匹配与换源 | T33 | 显式启用、候选源顺序／白名单、匹配阈值与策略版本、有限候选和预算；默认关闭 | 标注样本独立评估误匹配，记录阈值选择与误判；不无限换源／循环回退，不隐藏实际来源；严格品质与试听规则不被绕过；新策略版本按规范进入意图，不误复用旧结果 |
| T36 | S4 集成关口 | T34、T35 | 手动匹配、自动策略与媒体通路组合；按选定平台验证目录到实际音源的真实链路 | 通用契约与资源故障通过，A42 的模拟链路计量正确；声明 Spotify → YouTube 支持还必须完成 T25、T27 并通过真实组合验证；原曲和实际音源可追溯，不占错来源额度，不把 mock 结果列为在线支持 |

手动匹配、在线代理和自动换源是独立开关，可分别在相应任务验收后交付；只有全部声明完成才记为完整 S4。若某个音源无法提供稳定候选，记录该组合受阻，不阻塞本地文件下载或其他已验收组合。

## S5 部署与发布任务

| ID | 模块／任务 | 直接前置 | 交付范围 | 验收标准 |
| --- | --- | --- | --- | --- |
| T37 | 原生包与系统服务 | T32 | 选定 Windows／Linux 发行 preset、安装布局、依赖／证书／许可证、Windows 服务与 Linux 服务集成、ARM64 构建 | 干净机器无开发环境可运行；服务账号权限、中文／长路径、日志和停止行为通过；记录目标最低系统／运行库；ARM64 需要目标运行证据，只有产物不标正式支持 |
| T38 | Docker 多架构与 NAS 部署 | T37 | amd64／arm64 镜像、Compose、非 root、配置／数据／下载卷、端口与信号、选定 NAS 环境 | 目标架构上启动／HTTPS／下载／SIGTERM／重建恢复通过；data_dir 符合本地 SQLite 与单实例约束；NAS 下载目录离线／权限／发布失败可恢复；不把一台 NAS 结果推广到所有文件系统 |
| T39 | 目标矩阵与运行验证 | T38 | 按部署矩阵执行功能、负载、升级、备份恢复、证书、权限和资源验证；固定每个包的功能组合 | 每个正式声明的系统／CPU／运行库／部署组合都有实测报告；启用功能已通过各自任务与关口；超配额响应和资源上界可观察；缺失目标运行证据的组合标实验或不发布 |
| T40 | 发布关口与运维文档 | T39 | 版本、依赖清单、平台能力与验证日期、安装／配置／升级／恢复说明、发行包清单与已知限制 | 从空目录按说明部署成功；示例无秘密；可回查代码／配置／依赖版本和目标报告；备份恢复有效；所选 S2／S4／helper／平台功能附各自验收，不因主程序打包成功默认全支持 |

T37～T40 默认以通过 T32 的下载服务为基线；发布 S2 集合功能另需 T28，发布 S4 或 helper 按实际启用任务加验，不能用关闭功能规避已声明能力的验收。系统与架构范围以[部署矩阵](backend-service/configuration-and-deployment.md#部署方式)为准。

## 后续平台任务

推荐顺序与难度只在[平台路线图](platform-roadmap.md)维护；本节给出任务与技术依赖。酷我协议包为 T12、产品闭环为 T20，5sing 为 T22，其他平台见下表。Apple Music 不进入本次清单。

T41 通过后优先安排 Spotify 目录与 YouTube adapter。T41 的候选均只依赖公共能力，5sing 受阻时可先完成其他候选再验收 T41；不要求先完成所有较早平台。候选平台的真实下载集成复用 T20，不依赖 T41，避免再次形成循环。

| ID | 模块／任务 | 直接前置 | 交付范围 | 验收标准 |
| --- | --- | --- | --- | --- |
| N01 | Jamendo | T21 | 原生搜索／详情／解析；该端点实际需要的 SHA-1 协议适配 | 固定签名向量通过；download／stream 字段和实际品质不混淆；按平台交付模板完成标准闭环，不把参考网站接口称为无签名开放 API |
| N02 | 千千 | T21 | 参数排序、时间戳、MD5 与详情／tracklink | 固定输入输出与时钟边界通过；权限错误明确；实际品质闭环验证；摘要实现可复用但不以 N01 完成为技术前置 |
| N03 | JOOX | T21 | 地区、Cookie、JSON／JSONP、音源与可用目录 | 地区／账号条件入报告；不复制固定会话凭证；缺少目标样本即保留受阻能力；字段、分页、错误和下载闭环通过 |
| N04 | QQ 音乐 | T21 | songmid／文件名、vkey／CDN、外部 Cookie、标准品质，逐端点确认签名 | 资源与品质映射、权限、过期地址和错误样本通过；不靠静默降级完成；高品质独立验收 |
| N05 | 酷狗 | T21 | 多品质 hash、MD5／Tracker 等必需路径、权限与候选 | 请求品质、hash 与实际文件一致；候选变化不混合录音；App 凭证或高品质不因标准路径通过而自动声明支持 |
| N06 | Bilibili | T21、T30 | bvid／cid／分 P、必需请求头及按端点需要的 WBI、独立音轨与真实容器验证 | 独立音轨可保存／验证；不引入未支持分段流程；UP 主不映射为演唱者；默认不作为音乐匹配候选；取消和过期地址行为符合公共契约 |
| N07 | 网易云 | T21 | 闭环必需端点的 Linux／EAPI／WeAPI 包装、MD5／AES／按需 RSA、Cookie 与品质 | 协议固定向量与解析样本通过；不为未用端点实现全部包装；账号／权限／品质闭环实测；协议加密不与 TLS 选型绑定 |
| N08 | 咪咕 | T21、T29、T30 | 多资源标识、重定向、品质候选及实际需要的 FileKey／解密处理 | 明文和加密分开列支持；加密样本解密后验证格式／载荷，失败不发布；明文标准品质可拆子任务先交付，不能据此标记整项复杂品质完成 |
| N09 | 汽水 | T26、T30 | 签名器／应用组件的可部署性、Cookie、整曲解析及必要解密 | 先验证签名依赖能在声明目标运行；不只依赖开发机组件；只拿到试听不算整曲完成；进程／远端签名失败、取消和解密有证据 |
| X01 | SoundCloud | T21 | 先确定原生 API 或 helper 路线，再按能力接搜索／链接、曲目／集合及可验证音源 | 账号和分页范围明确；独立 HTTP／分段／试听分开处理；走 helper 另需 T26；只有通过单平台契约与在线验证的能力可启用 |
| X02 | Bandcamp | T21 | 曲目／专辑链接、目录、实际可下载音频 | 不假设全站搜索存在；区分试听与整曲、格式与权限；走 helper 另需 T26；未验证音源时只交付目录能力 |
| X03 | Audius | T21 | 原生 API 或 adapter、曲目／歌单／作者及音源，节点与地址变化 | 鉴权、分页、节点切换、资源身份和目标传输经过验证；走 helper 另需 T26；必要文件处理另需 T30，不把节点可达作为下载通过 |

X 任务的路线在其协议准备子任务中确定，并把 T26／T30 等条件前置记入执行记录，之后才能集成。N09 若最终选择纯原生签名，可依据验证结果移除 helper 前置并记录理由。新的平台持续追加同类任务，不以表中数量作为上限。

### 单个平台的任务模板

平台包与产品集成分别验收。T12 只包含 `.1`～`.4`，酷我 `.5` 由 T20 承担；其他平台已有 T20 下载基线，父任务包含全部适用步骤。协议包完成不等于产品已支持该来源，也不从 helper 站点清单批量声明能力。

| 子步骤 | 前置 | 交付与完成条件 |
| --- | --- | --- |
| `.1` 协议准备 | 父任务技术前置可用 | 确认当前端点、稳定 ID、能力、分页、品质、凭证／地区、签名和传输形态；保存脱敏样本、验证日期与缺口；确定 native／helper 及新增依赖 |
| `.2` 平台包 | `.1` | descriptor、能力／授权工厂、协议／解析、固定向量；仅实现已选择能力，无平台自有队列／SQL／通用路由分支 |
| `.3` 离线契约 | `.2` | 成功／空／缺字段／权限／限流／失效／取消样本，完整身份隔离与上下文版本测试；目录型来源不被强制提供音源 |
| `.4` 协议包在线验证 | `.3` | 记录账号／地区／时间／系统；验证真实目录请求与音源解析证据，必要时由传输验证程序受限取样，不要求持久任务、媒体或 SSE 已实现 |
| `.5` 产品集成与启用 | 酷我：T17、T18、T19；其他来源：`.4` 与 T20 | 酷我由 T20 验收完整下载／恢复／文件通路；后续音源平台在父任务内复用基线验证同样通路，目录型来源只验其目录能力。完成后才在指定发行组合声明已支持 |

跨源组合还要通过 T33／T35／T36 中适用的匹配验收；单个平台成功不自动意味着该组合可以正确匹配。新增一种全新公共能力时先更新端口和相应设计，再建任务，避免用平台私有字段绕过接口。

## 验收场景责任映射

“主责”表示负责实现并提供证据的任务，不是人员分配。阶段关口负责组合复验；前期的领域样本、可控上游或模拟 Provider 不构成后期真实平台能力的交付。A10／A29／A30 的客户端部分通过协议测试客户端验收，不要求本仓库开发 GUI。

| 场景 | 主责任务 | 首次关口与后续复验 |
| --- | --- | --- |
| A01 | T14 | T20：同 key 同输入重放 |
| A02 | T14 | T20：同 key 不同输入冲突 |
| A03 | T14 | T20：并发指纹唯一约束 |
| A04 | T17 | T20：关闭客户端后继续 |
| A05 | T21 | T28：多源部分失败 |
| A06 | T08、T13、T18 | T20：完整身份、数据与事件隔离；新平台追加复验 |
| A07 | T18 | T20：订阅满额下控制入口可用 |
| A08 | T18 | T20：慢消费者有界 |
| A09 | T18 | T20：断线后查询终态与文件 |
| A10 | T18 | T20：事件先于响应的合并契约 |
| A11 | T18 | T20：旧版本不能覆盖新状态 |
| A12 | T16 | T20：取消／发布竞争；T31 扩大故障覆盖 |
| A13 | T29 | T32：下载 Range 收到 200；T34 复验代理路径 |
| A14 | T29 | T32：错误 206／资源变化；T34 复验代理路径 |
| A15 | T29 | T32：416 必须核验完整性 |
| A16 | T29 | T32：URL 过期重解析；T34 复验在线资产 |
| A17 | T16、T31 | T20：基础写盘错误；T32／T39：磁盘与目标离线 |
| A18 | T30 | T32：可选标签失败的真实文件行为 |
| A19 | T15、T30 | T20：必要验证；T32／对应平台：真实解密失败 |
| A20 | T16 | T20：移动后退出；T31：更多发布窗口 |
| A21 | T18 | T20：DB 提交后通知前退出；T31 复验恢复 |
| A22 | T17 | T20：旧 execution_id；T29／T31 复验重试与恢复 |
| A23 | T19 | T20：文件授权与票据 |
| A24 | T19、T34 | T20：本地 Range／断开；T36：上游代理 |
| A25 | T17 | T20：未知长度；T29 复验断流／重试 |
| A26 | T17、T29 | T20：最小预算；T32：长期重试／重启 |
| A27 | T10、T17 | T11：基础停机；T20：任务检查点；T39：目标环境 |
| A28 | T18 | T20：仅 HTTP 完整操作与进度 |
| A29 | T20、T38 | T20：客户端保存与后端任务分离；T39：实际 NAS |
| A30 | T34 | T36：两个客户端无共享播放控制状态 |
| A31 | T34 | T36：秘密头与 direct／auto 选择 |
| A32 | T19、T34 | T20：文件票据；T36：在线资产与票据分别过期 |
| A33 | T19 | T20：关闭 proxy 后仍能取文件；T34 复验 |
| A34 | T08 | T20：撤销事实不被引导文件覆盖 |
| A35 | T14、T30 | T20：指纹／证据样本；T32：真实标签与文件复用 |
| A36 | T14、T33 | T20：稳定目标领域样本；T36：真实匹配用例 |
| A37 | T14、T33 | T20：持久替身／幂等；T36：真实匹配重启与重放 |
| A38 | T08、T13、T24 | T20：同 source 两个 version=1 用户凭证的隔离；T24：应用 token 条目隔离；T25：真实应用 token |
| A39 | T08、T13、T17 | T20：迟到结果与固定绑定；T24：真实刷新后复验 |
| A40 | T24 | 授权扩展关口：刷新／人工更新／撤销竞争；T25 在线复验 |
| A41 | T07、T24 | T07：工厂注册校验；T24：新授权协议接入 |
| A42 | T09、T36 | T09：逐跳租约；T36：模拟跨源；T25＋T27 完成后验证真实 Spotify → YouTube |
| A43 | T06、T17 | T20：入场延后释放槽与来源隔离；T29 复验冷却 |
| A44 | T17 | T20：admission 不消耗尝试、代次改变；T29 复验实际重试 |
| A45 | T24、T26、T29 | 启用应用 token／helper 时各自验收；T32 复验稳定组与真实退出 |
| A46 | T14、T31 | T20：完成文件与证据复用；T32：复核期间版本变化 |
| A47 | T08、T10、T13 | T11：默认凭证设置／清除／原子撤销；T20：默认选择世代与固定任务绑定 |
| A48 | T08、T10 | T11：身份和 token 的离线管理、独占锁、交付失败与撤销不复活 |
| A49 | T05、T10、T14、T19 | T11：目录根绑定与启动拒绝；T20：新任务受理、新旧目录文件读取及已受理任务恢复 |
| A50 | T13、T17、T18 | T20：码率 bit/s 与传输 byte/s 的模型、API、SSE 一致 |

上述编号不覆盖所有平台差异；每个平台还必须完成[平台扩展验收](backend-service/provider-extensibility.md#9-新平台交付模板与验收)，以及 API 中适用的错误／授权／分页契约。阶段验收失败应回到主责任务修正，不能只在验收任务中屏蔽失败场景。

## 待验证事项与完成定义

已有设计方向直接作为开发输入；以下事项由对应任务在实施时给出证据并写回规范，不作为要求用户重复确认的清单。

| 待验证事项 | 负责落实 | 关闭条件 |
| --- | --- | --- |
| 首批依赖发布版本、测试框架、TLS、链接方式、编译器／运行库基线 | T01、T09、T37 | 版本与许可证固定，目标系统 TLS／分发验证通过，写回构建与部署文档 |
| 平台端点、签名向量、账号／地区、品质与工具路线 | T12、T22、T25、T27、各 N／X 的 `.1` | 留存脱敏样本和验证日期；缺口对应具体关闭的能力，不凭旧研究推定可用 |
| OAuth／API key 范围、授权工厂和外部 helper 协议／版本 | T24、T26 | 竞争、取消和隔离契约通过，声明能力与绑定一致；核心发行包依赖边界明确 |
| MP3／FLAC／M4A 标签覆盖、解密与可选工具 | T30、N08、N09 | 实际文件与载荷验证通过，失败策略可复现，工具仅在对应功能组合启用 |
| 匹配候选、排除规则、阈值与策略版本 | T33、T35 | 独立标注样本有误判统计与取舍记录；不凭主观阈值宣称匹配正确 |
| 默认资源配额、延迟目标与设备适用范围 | T06、T20、T32、T39 | 先声明硬件、并发、负载和通过阈值，再记录实测；无统一硬件数据时不编造通用性能承诺 |
| ARM64、NAS、Docker 多架构与最低系统发布范围 | T37～T40 | 每个正式组合有目标运行证据；其余明确为实验／候选，不外推支持 |

一项开发任务完成至少应具备：

1. 交付范围内的实现、配置和文档一致；未覆盖能力关闭，公共依赖方向未被破坏。
2. 相应领域／协议、集成或契约检查通过，失败路径有独立样本或故障注入；不只验证正常返回，也不为简单转发重复造测试。
3. 变更影响的平台组合有构建与运行证据；平台在线验证单独记录时间、账号条件与结果，秘密不得进入报告。
4. 记录任务 ID、实现提交、直接前置状态、验证命令、报告位置、已知限制和后续子任务；阶段关口只聚合这些证据，不替代日常功能验证。

下一项为 **T18**（事件、SSE 与 HTTP 同步）与 **T16**（安全发布与最小重启恢复）；T17、T19 随后按前置推进。当前没有设置工期或人员分配，避免把尚未完成的平台协议验证估算为确定日期。

## 实施记录

### T01 构建、依赖与验证基础：已完成（2026-10-20，提交 7a60bb0）

| 项目 | 记录 |
| --- | --- |
| 交付 | `cmake/dependencies`（版本、获取、提交校验）、`cmake/adapters`（cpp-httplib + Mbed TLS、SQLite）、项目编译与警告选项、测试预设、依赖测试 `tests/dependencies`、CI `.github/workflows/ci.yml`；适配模块（infrastructure、三个 HTTP 入口）已链接真实依赖目标 |
| 选型 | cpp-httplib v0.62.0、Mbed TLS v3.6.7、nlohmann/json v3.12.0、SQLite 3.54.1、Catch2 v3.16.1；许可证、体积与选择理由见[依赖清单](../../../cmake/dependencies/README.md) |
| Windows 证据（2026-10-20） | Windows 11 x64，MSVC 19.51.36257（VS 2026，工具集 14.51.36231），Windows SDK 10.0.28000.0，CMake 4.4.4，Ninja 1.13.2。从空目录 `cmake --preset windows-msvc-debug` 获取并校验全部依赖；构建 0 条项目警告；`ctest --preset windows-msvc-debug` 5/5 通过；windows-msvc-release 配置与构建通过 |
| 校验失败验证 | 将 nlohmann/json 源码覆盖指向其他提交的检出，配置以 “HEAD is …, expected …” 失败 |
| GCC 证据（部分） | MinGW GCC 16.2.0 在 `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion` 下编译项目测试文件无警告；该编译器构建 Mbed TLS 时发生内部编译器错误，MinGW 不是支持的工具链，不作为 Linux 证据 |
| Linux 证据（CI 运行 39358091883） | GitHub Actions ubuntu-24.04 x86_64，GCC 13.3.0，glibc 2.39，CMake 3.31.6：全部依赖校验通过，构建 0 条项目警告，`ctest --preset linux-gcc-debug` 5/5 通过；同一运行的 windows-2025（MSVC 19.51.36260）5/5 通过 |
| 后续 | Linux ARM64 与 Docker 最小容器属于 T11 |
| 已知限制 | 首次配置需要联网；MSYS2／MinGW 发行的 cmake 无法校验 HTTPS 证书，Windows 须使用官方或 VS 自带 CMake；Mbed TLS 上游 CMakeLists 有 cmake_minimum_required 弃用警告，不影响构建 |
| 审查修正（2026-10-20） | 外部审查发现 3 处构建可重复性问题并均已复现后修复：SQLite 解压保留归档时间导致升级后 Ninja 不重编（改为解压时间）；SQLite 源码覆盖绕过归档哈希却仍报告已校验（新增 sqlite3.c／sqlite3.h 固定文件哈希，每次配置校验）；Git 依赖只比对 HEAD、未发现工作区修改（新增工作区、暂存、未跟踪文件与子模块检查）。修复后被篡改的源码与覆盖目录均在配置阶段失败，干净构建 110/110 通过 |

### T02 领域规则与最小应用端口：已完成（2026-10-20，提交 32cde52）

| 项目 | 记录 |
| --- | --- |
| 交付 | `music_service::domain` 静态库：Result／错误码、SourceId／PlatformId／TrackRef 与 UTF-8 校验、曲目与分页、品质选择与资源判定、AccountBinding／AuthIdentityKey、任务状态迁移与取消决策、NormalizedDownloadIntent 归一与指纹。`application/ports` 头文件端口：平台描述与搜索／音源解析能力、上游入场与租约、时钟与操作上下文、上游 HTTP、任务仓储 |
| 覆盖的验收 | strict／allow_lower、已知试听拒绝、未知完整性与未知品质带警告；同版本不同凭证的身份键不同（A38 领域部分）；试听／标签策略、手动目标、账号绑定、auto 顺序与策略版本进入指纹（A35、A36 领域部分）；终态无出口、finalizing 不可取消、非法迁移报错；入场不阻塞并延后、其他来源可推进、租约只归还一次、传输拒绝不匹配的租约（替身） |
| 规则补充 | 已写回规范：queued／waiting_retry → failed 迁移；品质未知与降级的警告规则、从不自动升档；指纹编码与 FNV-1a 摘要；auto 去除原曲来源、手动目标等于原曲时归一为同源、绑定按需裁剪 |
| Windows 证据（2026-10-20） | MSVC 19.51 /W4：0 条警告；`ctest --preset windows-msvc-debug` 45/45 通过（含 5 项依赖测试） |
| Linux 证据（CI 运行 39360376271） | ubuntu-24.04，GCC 13.3.0，glibc 2.39：domain 与单元测试 0 条项目警告，与 T03 合计 68/68 通过 |
| 未覆盖 | 端口只有替身实现；真实入场账本（T06）、传输（T09）、仓储（T05）分别在对应任务实现和验收 |

### T03 配置、时钟与可观测性基础：已完成（2026-10-20，提交 52b7d3a）

| 项目 | 记录 |
| --- | --- |
| 交付 | `application/config/ServiceConfig`（配置基线全部配置组的缺省值）；`infrastructure/config`：缺省 → JSON 文件 → MUSIC_SERVICE_* 环境变量 → 命令行的加载、路径解析与组合校验；`SystemClock`／`ManualClock`；结构化日志（JSON 行／文本／内存 sink、绑定上下文、级别过滤）与字段脱敏；客户端 request_id 校验与服务 ID 生成；计数与延迟指标接口。infrastructure 改为静态库 |
| 覆盖的验收 | 未知键、类型错误、越界值、无效 JSON、文件不可读均在启动前失败并一次报告全部字段；端口冲突、端口 0 非 loopback、非 loopback 缺 public_urls、远程 http、构建缺失功能、未编入平台、default_sources 不合法、事件／媒体额度超过 workers−2、目标目录 ID 与 principals 均被拒绝；文件内相对路径按配置文件目录、环境变量与命令行按工作目录解析，含 BOM 与中文路径；日志中 Authorization／Cookie／token／ticket 字段、URL 查询串与票据路径段、Bearer 文本均被脱敏，`*_id`／`*_version` 保留；ManualClock 可推进且墙钟跳变不影响单调时钟 |
| 规则补充 | 已写回配置文档与[配置键名](../../../config/README.md)：JSON 键名与单位后缀、相对路径基准、端口 0 与 public_urls 规则、构建前 media／events／file_access 缺省关闭、app_profiles 在 T24 前只接受空对象 |
| Windows 证据（2026-10-20） | MSVC 19.51 /W4：0 条警告；`ctest --preset windows-msvc-debug` 68/68 通过 |
| Linux 证据（CI 运行 39360376271） | ubuntu-24.04，GCC 13.3.0，glibc 2.39：`ctest --preset linux-gcc-debug` 68/68 通过；GCC 13 报告 1 条 `-Wformat-truncation`（日志时间格式化缓冲区），已在目录调整提交中修复 |
| 未覆盖 | `check-config` 等命令行入口归 T10；日志文件轮转与指标导出格式在有部署需求时补充 |

### 目录结构调整（2026-10-20）

本仓库是跨平台纯后端，前端另建仓库，因此 CMake 工程从 `cpp/` 移到仓库根目录；`cpp/include/music_service/` 并入 `src/`，头文件与实现同目录、以 `src/` 为包含根；服务入口 `apps/server` 改为 `src/server`；配置说明从 `resources/config` 移到 `config/`。模块划分、目标名与命名空间不变，见[模块与目录](backend-service/modules-and-layout.md#目录)。调整后 Windows 68/68 测试通过。

### 项目改名（2026-10-20）

项目与仓库名统一为 music-service：CMake 项目与目标前缀 `music_service`，C++ 命名空间 `music_service`，可执行程序 `music-service`，环境变量前缀 `MUSIC_SERVICE_`，`/api/v1/info` 的 service 字段 `music-service`，任务工作目录 `.music-service-work`。外部参考项目的 GitHub 地址保留其原仓库名，其余文字与目录统一称为 Go 参考实现（`docs/research/go-reference`）。改名后 Windows 68/68 测试通过。

### T04 文件系统、工作目录与 SecretStore：已完成（2026-10-20，提交 10533ab）

| 项目 | 记录 |
| --- | --- |
| 交付 | `infrastructure/filesystem`：UTF-8 路径转换与 ASCII 化的文件错误；相对路径检查（空段、`.`／`..`、绝对与盘符、反斜杠、冒号、控制与 Windows 禁用字符、首尾空格与结尾点、保留设备名）；`resolve_within` 解析最深已存在祖先以拒绝符号链接／重解析点逃逸；文件名清理（UTF-8 边界截断）；独占创建并落盘的写入、按大小上限读取、不覆盖目标的重命名（Windows MoveFileEx、Linux renameat2 或 link+unlink）、仅所有者权限目录与文件；进程级实例锁（Windows 独占打开、POSIX flock，写入 PID）；data_dir 布局与可写检查、目标根校验（必须已存在、可写、不与 data_dir 互相包含）、任务工作区。`application/ports/storage/SecretStore` 与不可打印、不可复制、析构清零的 SecretBytes；`infrastructure/secrets/FileSecretStore`（不可变 sec_ 引用、仅所有者文件、64 KiB 上限）。新增错误码 DESTINATION_UNAVAILABLE、INSTANCE_LOCKED、FILE_SYSTEM_ERROR |
| 覆盖的验收 | 第二实例拒绝且释放后可再取得；越界、`..`、保留名与符号链接逃逸被拒绝（Windows 实际创建目录符号链接验证）；中文路径往返、中文目录下的配置与秘密文件；文件已存在时不覆盖、重命名不替换既有目标；目标根缺失时报 DESTINATION_UNAVAILABLE 且不自动创建；秘密引用不能指向其他文件，更新得到新引用，日志描述不含秘密值 |
| 规则补充 | 已写回配置文档：秘密文件权限与命名、目标根从不自动创建、data_dir 与目标根不得互相包含 |
| Windows 证据（2026-10-20） | MSVC 19.51 /W4：0 条警告；`ctest --preset windows-msvc-debug` 85/85 通过 |
| Linux 证据（CI 运行 39363696302） | ubuntu-24.04，GCC 13.3.0，glibc 2.39：0 条项目警告，97/97 通过，覆盖 POSIX 分支（flock、O_EXCL、renameat2、0600／0700 权限断言） |
| 未覆盖 | 目标目录的不可变 ID 绑定落在 T05（SQLite）与 T10（启动校验）；NAS 断开与权限变化的实际环境验证在 T38 |

### T05 SQLite 与仓储基础：已完成（2026-10-20，提交 c86a6f2）

| 项目 | 记录 |
| --- | --- |
| 交付 | `infrastructure/storage/sqlite`：不暴露 sqlite3.h 的连接／语句封装与错误映射；Database（WAL、synchronous=FULL、foreign_keys、唯一 writer 有界等待与等待者上限、query_only 读取连接池、事务内执行并在错误时回滚）；带校验和的版本化迁移与 v1 表结构（数据模型中的全部 S1 表）；身份与 token、凭证与默认选择、目录绑定、任务（幂等、活动意图唯一、配额、领取、受执行世代与版本保护的迁移、取消决策、意图读取）、发布（路径预留、完成、放弃、待恢复意图、资产查询与 missing 标记）仓储；`storage/intent_codec`（下载意图 JSON，解码时重新归一校验）。新增端口：身份、凭证、目录绑定、发布仓储；任务仓储端口补充 force、resume_stage、get_intent 与时间字段。新增错误码 DESTINATION_ROOT_CHANGED、DESTINATION_IN_USE、IDEMPOTENCY_CONFLICT、ACTIVE_DUPLICATE、QUEUE_LIMIT_EXCEEDED |
| 覆盖的验收 | 初始化与重开、迁移幂等、拒绝更新的 schema 与被改动的迁移；失败写入回滚；writer 被占用时在等待预算内返回可重试错误且读取不受阻；唯一约束（token 哈希、活动指纹、发布路径大小写不敏感）；A01～A03（同键重放、同键异请求冲突、活动意图复用、force 冲突）与配额；A22（旧执行世代与旧版本拒写，waiting_retry 到期后换新执行世代并从记录的阶段恢复）；A12 基础（finalizing 先到时取消返回 TASK_FINALIZING，完成后 TASK_TERMINAL）；A47 的存储部分（默认选择 A→B→A 递增、清除语义、撤销默认凭证在同一事务内清空并推进版本、跨重启保留）；A49 的存储部分（同 ID 改根拒绝、in_use 判断）；任务、默认选择与待恢复发布意图跨重开保留 |
| 规则补充 | 已写回数据模型：STRICT 表、毫秒时间、synchronous=FULL、writer 有界等待、path_key 大小写不敏感、撤销清空 secret_ref |
| Windows 证据（2026-10-20） | MSVC 19.51 /W4：0 条警告；`ctest --preset windows-msvc-debug` 97/97 通过（存储集成测试 12 项、239 个断言） |
| Linux 证据（CI 运行 39363696302） | ubuntu-24.04，GCC 13.3.0，glibc 2.39：0 条项目警告，97/97 通过（含存储集成测试） |
| 未覆盖 | begin_attempt、checkpoint、defer_admission 与批次在 T14／T17 实现；token 哈希算法与签发在 T08；目录绑定的启动校验在 T10；幂等记录的定期清理随运行时任务加入 |

### T06 执行器、取消与统一上游入场：已完成（2026-10-20，提交 57c8a68）

| 项目 | 记录 |
| --- | --- |
| 交付 | `runtime` 静态库：BoundedExecutor（固定线程、有界队列，满或停止时立即 SERVICE_BUSY，任务获得 stop_token，支持停止接收／等待空闲／请求停止／join 与统计）；DeadlineTimer（单线程按单调时间触发回调，可取消，用于截止取消与冷却到期唤醒）；AdmissionLedger 实现 UpstreamAdmissionPort（全局连接、principal、source×流量类别、origin 四维一次性预留；不足时返回带受限维度与重试时刻的 Deferred；Retry-After 按来源或配额组冷却，上限 max_cooldown，只延长不缩短；租约持有账本状态，账本销毁后归还仍安全；helper 进程维度在 T26 前返回不支持）；ShutdownSequence（停止接收 → 共享软预算等待空闲 → 触发服务停止信号并丢弃未开始的排队工作 → join，报告未在预算内空闲的执行器）。入场端口新增 readiness 提示；新增配置 upstream.per_principal、upstream.per_origin 与错误码 SERVICE_BUSY |
| 覆盖的验收 | A43 的运行时部分：来源满额时 Deferred 不阻塞，其他来源可推进；各维度独立限额；被拒请求不留部分预留；租约只归还一次（含析构与移动），多连接计量；冷却到期前延后、到期后恢复，共享配额组一起冷却（A45 的入场基础）；截止已过与未启用的 helper 明确报错；执行器满队列拒绝、停止信号传到运行中的工作、未开始的排队工作被丢弃；停机在软预算内排空并报告超时的执行器；定时器按序触发与取消 |
| Windows 证据（2026-10-20） | MSVC 19.51 /W4：0 条警告；`ctest --preset windows-msvc-debug` 110/110 通过；涉及线程与计时的 runtime 测试连续运行 20 次无失败 |
| Linux 证据（CI 运行 39364672250） | ubuntu-24.04，GCC 13.3.0，glibc 2.39：0 条项目警告，110/110 通过；审查修正（提交 6aa2177，CI 运行 39384805494）后 120/120 通过 |
| 未覆盖 | 入场额度与真实传输的结合（重定向逐跳入场、租约与 Transport 校验）在 T09；下载的持久延后与执行槽在 T17；共享应用配额组的配置与 helper 上界在 T24／T26 |

### T07 ProviderBundle 注册与能力发现：已完成（2026-10-20，提交 3ef2722）

| 项目 | 记录 |
| --- | --- |
| 交付 | `application/ports/providers`：能力端口 ITrackSearch、ITrackDetail、ILinkParser、IAssetResolver、ILyrics 及键映射 capability_traits；授权端口 ICredentialVerifier 与只读 CredentialSnapshot；`bundle.hpp`：ProviderBundle（契约版本、descriptor 字段、CapabilitySpec、带端口类型的 CapabilityBinding、AuthProfileSpec、品质提示、options 校验器）。`providers` 改为静态库：ProviderRegistry（启动校验并一次报告全部问题、不可变、按需创建并复用能力对象、按 source + auth_profile + kind 取校验器、providers 分页与绑定游标）；`MUSIC_SERVICE_PROVIDERS` 生成直接引用 `make_bundle()` 的内置清单；`providers/kuwo` 占位 bundle（不声明能力）。新增错误码 CURSOR_EXPIRED、CURSOR_MISMATCH |
| 覆盖的验收 | 酷我占位与 4 个模拟来源（仅目录、仅音源带 Cookie 校验、仅链接解析、目录加音源）同时注册；未声明能力返回 CAPABILITY_UNSUPPORTED，未注册与禁用分别为 UNKNOWN_PROVIDER、PROVIDER_DISABLED；同一段搜索代码服务所有来源，新增来源不改调用方；构建、describe、list 不创建任何对象，首次 require 才创建且并发首用只创建一次，工厂失败可重试；重复 source、配置未编入的 source、契约版本不符、implementation 不符、options 被拒、声明无工厂、工厂未声明、端口类型错误、未知能力键、重复 profile、引用未注册 profile、校验器与 verify_credentials 不一致均在注册时失败（A41 的 T07 部分）；supports_verify 由绑定生成，刷新／应用 token 恒为 false；分页按 source 排序，游标跨调用方或筛选为 CURSOR_MISMATCH，新注册表（重启）为 CURSOR_EXPIRED；内置 source 清单可直接用于配置校验 |
| 规则补充 | 已写回[平台模块说明](../../../src/providers/README.md)与[配置键名](../../../config/README.md)：注册失败条件、没有 options 校验器的平台只接受空对象、未在配置中出现的已编入平台按 ProviderConfig 缺省（启用）处理但不会进入 default_sources |
| Windows 证据（2026-10-20） | MSVC 19.51 /W4：0 条警告；`ctest --preset windows-msvc-debug` 132/132 通过；providers 用例（12 项、203 个断言）连续运行 20 次无失败 |
| Linux 证据（CI 运行 39390389343） | ubuntu-24.04，GCC 13.3.0，glibc 2.39：0 条项目警告，132/132 通过 |
| 未覆盖 | availability 与当前调用方凭证摘要的投影在 T08／T10；`GET /api/v1/providers` 路由与 CLI 组装在 T10；酷我实际能力在 T12；刷新／应用 token 端口在 T24；100 个来源的规模验证在 T28 |

### T08 服务身份与 Cookie 凭证基线：已完成（2026-10-20，提交 4f7c6fa，测试修正 8a0ffcb）

| 项目 | 记录 |
| --- | --- |
| 交付 | `application` 改为静态库并新增 `accounts`：IdentityAdminService（init、principal 创建／列出／启停、token 签发／列出／撤销）、AccessTokenAuthenticator、CredentialService（Cookie 凭证创建／更新／撤销／校验、默认选择、resolve_binding、lease、reclaim_orphan_secrets）与 API scope 清单。新增端口 AccessTokenCodec、TokenDelivery、ProviderDirectory（ProviderRegistry 实现）。`infrastructure/auth`：MbedTlsTokenCodec（`mst_` + 32 字节随机数，SHA-256 存储）与 FileTokenDelivery。仓储：principal 可用固定 ID（init 的 prn_local）；凭证更新可只改标签；新增校验结果记录与被引用秘密清单；CredentialRecord 增加最近校验时间与结果 |
| 覆盖的验收 | A34：撤销后引导文件仍在也不能认证，重启与再次 init 均不导入；A48 用例部分：交付文件已存在则不签发不覆盖，数据库失败后删除交付文件且无有效 token，撤销一个 token 不影响其他 token，禁用 principal 阻止认证、重新启用不复活已撤销 token，scope 必须显式、已定义且不重复；中断的 init 仅在 prn_local 无 token 时续做；A47：默认选择的设置／重复设置不增版本／A→B→A／清除／他人凭证 404／来源不符与已撤销 422／未知来源 UNKNOWN_PROVIDER／重启保留，撤销默认凭证同事务清空且不改选；A38：同 principal 同来源两个 version=1 凭证的身份键不同；A39：默认切换后已固定绑定仍租用原账号，更新后租用同一账号的新版本，校验期间凭证被更新时旧结果不写入新版本（VERSION_CONFLICT）；A06 用户凭证部分：他人凭证不可读取、伪造绑定不可租用；撤销后绑定返回 CREDENTIALS_REQUIRED 而不换账号；秘密先写新引用再条件提交，失败回收新秘密、成功后删除旧秘密，孤立秘密可回收 |
| 规则补充 | 已写回[通用身份适配说明](../../../src/infrastructure/auth/README.md)：token 格式与哈希、交付文件规则。实现约定：Cookie 不得含控制字符（防止注入其他请求头），标签为至多 128 字节的 UTF-8；oauth2／api_key 在 T24 前返回 CAPABILITY_UNSUPPORTED；可选凭证能力在没有可用默认时退回 anonymous，并返回所读取的 selection_version；孤立秘密回收只在启动时、开始受理请求前运行 |
| Windows 证据（2026-10-20） | MSVC 19.51 /W4：0 条警告；`ctest --preset windows-msvc-debug` 143/143 通过（新增账号集成测试 11 项） |
| Linux 证据（CI 运行 39436724565） | ubuntu-24.04，GCC 13.3.0，glibc 2.39：0 条项目警告，全部用例通过。T08 推送后的 CI（运行 39391684081）在 Linux 上发现 A39 测试的缺陷：钩子在自身执行中重置 std::function，销毁正在运行的闭包后继续使用其捕获（MSVC 未暴露，GCC 段错误），已在提交 8a0ffcb 修正为一次性标志；同时消除一处 Catch2 宏嵌套引起的 -Wshadow |
| 未覆盖 | 实例锁下的 CLI 命令、HTTP 凭证与默认选择路由、Bearer 中间件归 T10；缓存与游标的失效（含 selection_version 绑定）随 T13 的缓存实现；A06 中请求头、任务与 SSE 的隔离分别在 T09、T14、T18 复验；application 模式、应用 token 与刷新在 T24 |

### T09 HTTP／TLS 上游传输：已完成（2026-10-20，提交 25b0f08）

| 项目 | 记录 |
| --- | --- |
| 交付 | 传输端口：HttpRequest 增加 secret_headers、operation、平台登记的明文端点与已跟随跳数，HttpResponseHead 增加跨 origin 重定向与消费者停止标记；ProviderBundle 增加 plaintext_endpoints（注册表校验并按来源提供）。`application/transport`：严格 URL 解析／origin 规范化／Location 解析，每跳请求规则与重定向规则，UpstreamClient（每个 origin 非阻塞入场、旧跳结束释放后再入场、无法入场交回 PendingHop、429／503 的 Retry-After 反馈冷却）。`infrastructure/transport`：HttplibTransport（Mbed TLS，校验证书链与主机名，可配 CA bundle，有界与流式两种读取，截止与取消）。测试 TLS 材料见 `tests/fixtures/tls` |
| 覆盖的验收 | 本地 HTTPS 上游的 GET／POST 与错误状态；自签名证书链、证书主机名不符、未信任测试根时均失败且不发出请求；未登记明文端点拒绝且不联网，登记后可用，明文请求携带秘密被拒；同 origin 重定向保留 Cookie，跨 origin 重定向不带 Cookie、先归还旧租约再按新 origin 入场（A42 的传输部分），新 origin 无额度时立即交回且不等待；303 改 GET 去 body，跳数上限；HTTPS→HTTP 降级仅限登记的下载端点并剥离秘密；Retry-After 写入冷却，冷却内的请求延后；4 MiB 分块流式接收不整体缓冲，有界读取超限失败（分块与 Content-Length 两种）；消费者拒绝响应头时不读正文；响应被截断为可重试的 connection_lost；正文传输中取消、截止时间到达均结束并归还租约；租约缺失或不覆盖目标 origin 时不发请求 |
| 规则补充 | 已写回[上游传输说明](../../../src/infrastructure/transport/README.md)与[测试 TLS 材料说明](../../../tests/fixtures/tls/README.md)。实现约定：Cookie／Authorization／Proxy-Authorization 只能放在 secret_headers；明文端点按主机＋端口＋路径前缀＋操作登记；Retry-After 只采信秒数形式 |
| Windows 证据（2026-10-20） | MSVC 19.51 /W4：0 条项目警告（httplib.h 的 C4702 在包含处局部屏蔽）；`ctest --preset windows-msvc-debug` 158/158 通过；传输集成测试（10 项、185 个断言）连续运行 20 次无失败 |
| Linux 证据（CI 运行 39436724565） | ubuntu-24.04，GCC 13.3.0，glibc 2.39：0 条项目警告，传输集成测试全部通过，含等待响应头期间的取消在 1.5 秒内返回（本地 shutdown 能唤醒阻塞等待）；`[online]` 用例经系统 CA 文件访问公开 HTTPS 站点通过（Windows 同一运行中经系统证书存储通过） |
| 已知限制 | Windows 上等待响应头期间的取消不能立即中断（本地 shutdown 不唤醒阻塞等待，httplib 客户端无可中断读），要到上游应答、空闲超时（30 秒）或截止时间才返回，期间租约保持；Linux 上即时中断已由 CI 验证。暂不复用连接，每跳新建连接；CA bundle 尚无配置键，部署所需的信任根配置在 T11 确定；配置只能关闭明文端点的开关留待有平台登记时加入 |
| 未覆盖 | 平台实际端点登记与在线协议探测在 T12；下载写盘、断点续传与传输重试策略在 T17／T29；媒体代理在 T34 |

### T10 组装入口与基础 HTTP／管理 CLI：已完成（2026-10-20，提交 54afdb1）

| 项目 | 记录 |
| --- | --- |
| 交付 | `api` 静态库 ControlApi（cpp-httplib）：Bearer 认证与 scope、统一 JSON 信封与错误码→HTTP 状态映射（500 只返回去敏信息）、X-Request-Id 校验或生成、CORS 预检、请求体上限 413、非 JSON 415、未知字段／参数 400、身份相关响应 no-store；路由 health/live、health/ready、info、providers（分页与调用方凭证摘要，可用状态 unknown、不探测）、limits、destinations（按配置 ACL，不返回目录）、credentials（列出／创建含 cookie 简写／更新／校验／撤销）、credential-defaults、admin/status；尚未交付的路由不挂载、返回 404。`server`：命令行（serve、version、check-config、init、healthcheck、principal、token）、离线管理持实例锁、ProcessHost 按文档顺序启动并反序停止、目标目录不可变绑定校验、启动行输出到 stdout。ProviderDirectory 端口增加 describe／list 及列表类型 |
| 覆盖的验收 | T08 用例经 API／CLI 可操作：init 只把明文写入引导文件、再次 init 冲突，principal 创建／列出／启停，token 签发不覆盖既有文件、明文不进控制台、列出／撤销（重复撤销成功）；A47 经 API：设置、重复不增版本、跨来源 422、他人凭证 404、撤销默认凭证同事务清空且不改选；A48：服务运行时管理命令返回 INSTANCE_LOCKED，停止后可执行；A49 基础：同 ID 改根启动与 check-config 均失败，新 ID＋旧项 accept_new_tasks=false 时两者都可用，仍被任务引用的 ID 从配置移除时启动失败、未被引用的可移除，新 ID 根不存在时失败且不创建目录；停止顺序为监听 → 服务 → 数据库 → 实例锁，日志不含 token 与 Cookie；认证、scope、CORS、413／415／400／404 与 no-store 行为 |
| 规则补充 | 已写回[服务入口说明](../../../src/server/README.md)：命令与退出码、启动行格式、check-config 只读比对绑定、healthcheck 在端口 0 模式下的限制、已绑定但离线的目标根保留并标记不可用 |
| Windows 证据（2026-10-20） | MSVC 19.51 /W4：0 条项目警告；`ctest --preset windows-msvc-debug` 161/161 通过（服务集成测试 3 项、471 个断言，连续 5 次无失败）；`music-service.exe` 实机：version、init、serve（启动行、healthcheck 成功、运行中 token list 被拒、带引导 token 的 info 200）、进程结束后锁释放 |
| Linux 证据（CI 运行 39436724565） | ubuntu-24.04，GCC 13.3.0，glibc 2.39：0 条项目警告，服务集成测试全部通过；可执行程序的 SIGTERM 收尾见 T11 |
| 已知限制 | 媒体与 SSE 监听在相应功能实现前不启动，info 如实声明全部功能为 false；admin/download-policy 随 T17；Windows 服务／systemd 集成未做（T37）；CA bundle 仍无配置键 |
| 未覆盖 | 搜索、下载、文件等业务路由分别在 T13～T19；Linux／容器运行与部署证据在 T11 |

### T11 S0 集成与最小部署关口：已完成（2026-10-20，提交 ce106f0、17c6c85）

| 项目 | 记录 |
| --- | --- |
| 交付 | `packaging/docker`：多阶段 Dockerfile（ubuntu:24.04 按 digest `sha256:68ee2200…33ac` 固定；构建阶段 pip 安装 CMake 3.31.6／Ninja 1.13.0；运行镜像只含剥离符号的程序与 ca-certificates，uid 10001 非 root，`/data`、`/downloads` 卷，exec 形式 ENTRYPOINT，HEALTHCHECK 调用 `healthcheck` 子命令）与容器配置；`packaging/{windows,linux,docker}` 运行冒烟脚本；`[online]` 用例经系统信任根访问公开 HTTPS；CI 增加冒烟、在线 TLS 与容器任务；ProcessHost 支持 logging.format=text |
| Windows x64／MSVC | 本机：Windows 11 Pro 10.0.26200 x64，MSVC 19.51.36257，`cmake --preset windows-msvc-release`（官方 CMake 4.4.4）构建 0 条警告，程序 3.2 MB，依赖 kernel32、ws2_32、crypt32、bcrypt、VC++ 2015-2022 运行库（MSVCP140／VCRUNTIME140）与 UCRT，无 user32 等界面依赖；release 与 debug 程序各运行 `smoke.ps1` 通过。CI（运行 39436724565，windows-2025，Windows NT 10.0.26100 x64）：测试全部通过，`[online]` 经系统证书存储通过，`smoke.ps1` 通过 |
| Linux x86_64／GCC | CI（运行 39436724565，ubuntu-24.04，Linux 6.17 x86_64，GCC 13.3.0，glibc 2.39）：0 条项目警告，测试 162/162 通过；程序动态依赖 libstdc++、libgcc_s、libc、libm；`[online]` 经系统 CA 文件通过；`smoke.sh`：init、serve 就绪、healthcheck、带 token 200／无 token 401、运行中管理命令被拒，SIGTERM 后退出码 0，停机顺序监听 → 服务 → 数据库 → 实例锁，之后管理命令可执行 |
| 容器 | CI（运行 39435328360 与 39436724565，linux/amd64）：镜像 85 MB，用户 music-service；数据卷上 init，健康检查 healthy，发布端口带 token 访问，容器内管理命令被拒；`docker stop` 退出码 0 且停机顺序完整；删除容器后用同一卷重建再次 healthy，原 token 仍有效 |
| 覆盖的验收 | 无前端、桌面会话或音频设备时三种目标均可启动、健康、停止；入口与配置一致（控制入口；媒体与 SSE 在功能实现前不启动，info 如实声明）；证书来源、运行库、架构与命令已记录；运行验证均在目标环境实际执行，未以编译或交叉编译代替 |
| 过程问题 | 首版 Linux 冒烟脚本把 shell 函数放到后台，SIGTERM 发给了子 shell；随后清理钩子以空 PID 执行 `kill 0`，信号打到整个进程组导致 CI 运行器被停（运行 39435328360），提交 17c6c85 修正 |
| 已知限制 | Windows 上未验证可执行程序收到 Ctrl+C 后的收尾（PowerShell 无法向后台进程发送控制台事件；停机顺序由进程内测试覆盖，冒烟脚本验证强制结束后锁释放与重启）；Windows 服务、systemd、Compose 示例、arm64 镜像与 NAS 未做，归 T37～T39；CA bundle 配置键仍未加入，目前三种目标都使用系统信任根 |

### T12 酷我标准品质协议包：已完成（2026-10-20，提交 c4c0e62）

| 项目 | 记录 |
| --- | --- |
| 交付 | `providers/kuwo`：protocol（请求构造）、parsing（搜索 JSON、歌曲页面、音源响应、分享链接，纯函数）、capabilities（经上游客户端）与 bundle（kuwo-native 0.1.0，匿名 search_tracks／track_detail／parse_links／resolve_audio，品质提示仅 standard，无凭证 profile）。新增 `UpstreamHttp` 端口（UpstreamClient 实现），经 ProviderSetup 注入工厂并带配置的 rate_limit_group；无法入场映射为可重试 SERVICE_BUSY。脱敏样本见[样本说明](../../../tests/fixtures/providers/kuwo/README.md)，平台说明见[酷我适配器](../../../src/providers/kuwo/README.md) |
| 端点核实（2026-10-20，本机 Windows，中国大陆网络） | 搜索 `https://www.kuwo.cn/search/searchMusicBykeyWord` 可用（http 访问 301 到 HTTPS）；音源 `https://mobi.kuwo.cn/mobi.s` 可用；音源 CDN（`kw-*.kuwo.cn`）上游下发 http 地址，同一路径 HTTPS 证书有效且返回 206；歌曲页 `https://www.kuwo.cn/play_detail/{rid}` 可用，不存在的 rid 返回 HTTP 430。参考实现的 `m.kuwo.cn/newh5/singles/songinfoandlrc` 对所测歌曲均返回“音乐查询失败”，`www.kuwo.cn/api/www/music/musicInfo` 拒绝请求。结论：T12 所用端点全部为 HTTPS，未登记任何明文端点；标准品质无需凭证，未注册凭证校验 |
| 格式／品质／试听证据 | 免费歌曲 78932517：mobi.s 返回同 rid、type 0、mp3、bitrate 128、duration 176，CDN 文件 2,829,338 字节（约 128.6 kbit/s），与搜索时长一致 → 标准品质、上游声明整曲。付费歌曲 228908 在固定 user 值下返回另一 rid（260839262）、type 1、bitrate 1、11 秒短片 → 报告为 preview、无品质声明，默认被 evaluate_resource 拒绝；同一歌曲在随机 user 值的请求下返回完整标准品质，替代与否取决于请求条件，因此逐次按响应证据判定。字节层面的格式与时长核验归 T15 |
| 覆盖的验收 | 平台契约：搜索映射（rid、标题、歌手及 id、专辑及 id、时长，不按标题猜版本，缺专辑留空）、分页游标与总数、实体解码、HTML 或缺字段的响应为协议错误而非空结果；歌曲页只接受同 rid 页面，缺失为 NOT_FOUND；音源地址限 `*.kuwo.cn` 默认端口并改为 HTTPS，相似域名被拒；链接解析只接受已知形式；请求均为 HTTPS、匿名、经上游客户端并带正确入场字段，非法输入不发请求，503 为可重试上游错误，入场延后为 SERVICE_BUSY；strict 高品质在发请求前返回 QUALITY_UNAVAILABLE，allow_lower 回落标准品质。受限在线探测（`MUSIC_SERVICE_KUWO_ONLINE=1`，本机）：搜索“晴天”10 条（共 3552），7 条解析为整曲标准品质，详情标题与搜索一致，入场额度全部归还 |
| Windows 证据（2026-10-20） | MSVC 19.51 /W4：0 条项目警告；`ctest --preset windows-msvc-debug` 171/171 通过（2 项在线用例按设计跳过） |
| Linux 证据（CI 运行 39439859236） | ubuntu-24.04，GCC 13.3.0，glibc 2.39：0 条项目警告，171/171 通过（离线契约测试；酷我在线探测不在 CI 运行，因海外运行器可能受地区限制）；Windows 与容器任务同时通过 |
| 已知限制 | 高品质、歌词（XOR+Base64 参数）、歌单／专辑／榜单与凭证均未实现、未声明；详情依赖歌曲页面结构，页面改版会以协议错误暴露；替代短片的 rid 未作为 actual_track 记录（无该 rid 的元信息） |

### T13 搜索、详情、解析与缓存用例：已完成（2026-10-20，提交 9e4efaa）

| 项目 | 记录 |
| --- | --- |
| 交付 | `application/catalog`：CatalogService（单曲搜索、详情、分享文本链接解析）与 AssetService（同源音源解析、asset 句柄、有限探测），通用的身份分区结果缓存、在途合并与有界句柄表。控制入口新增 `GET /api/v1/search/tracks`、`GET /api/v1/tracks/detail`、`POST /api/v1/links/parse`、`POST /api/v1/assets/resolve`、`POST /api/v1/assets/probe`，info 声明 search。新增 WorkExecutor 端口（BoundedExecutor 实现，ProcessHost 以 upstream.short_workers／queue 建 catalog 池并在停机时先于服务收尾）、ProviderContext 携带租用的秘密、ProviderDirectory 的类型化能力访问 |
| 规则落实 | 来源为显式列表、all（启用且支持搜索的来源）或 catalog.default_sources；未知来源整体 400，超过 max_sources_per_request 为 400；按 catalog.fanout 有界并发，单源截止取 per_source_budget 与总预算较小者，总截止到时未答来源记为 UPSTREAM_TIMEOUT 并请求停止，迟到结果丢弃；至少一源成功 200（有失败则 partial），已执行来源全超时 504、其他全失败 502、无可用来源 422，均附逐源结果。缓存键与在途合并键含完整 AuthIdentityKey、实现版本、查询与页游标，失败不缓存。游标为服务句柄，绑定 owner、来源、查询、页大小、实现版本、授权身份及隐式默认选择的 selection_version，未知／他人／过期／重启后为 410 CURSOR_EXPIRED，来源或查询不符为 400 CURSOR_MISMATCH。asset_id 绑定 owner 与授权身份，有效期取 lifetimes.asset 与上游有效期减 30 秒的较小者，到手即过期的地址视为失败；探测只读 16 字节，按首字节识别格式，不以扩展名作证据。不提供自动换源与匹配选择（CAPABILITY_UNSUPPORTED）；没有任何来源需要私有上下文，context_ref 暂为 null |
| 覆盖的验收 | A38：同 principal 同来源两个 version=1 凭证的搜索结果与缓存分区互不复用；A47 的游标部分：默认账号 A→B 后旧游标 410，切回 A 仍 410，A 的缓存分区仍只服务 A；A39：凭证更新后旧游标 410、新版本另起分区；A06：两 principal 结果与游标互不可用，asset 他人不可见；单源超时不拖住请求（1 秒内返回 partial）；并发相同查询只发一次上游调用；过期 asset、账号变化后的 asset 探测为 410；strict／allow_lower、preview 规则与 422 无执行路径；HTTP 层的信封、逐源 meta、状态码映射、scope 与参数校验 |
| 端到端（2026-10-20，本机，真实酷我） | 经 music-service.exe：搜索“晴天”5 条（共 3552，201 ms），重复搜索命中缓存，游标翻到第 2 页；详情、分享文本解析成功；解析得到 standard／full／mp3／128 kbit/s 的 asset，响应不含上游地址；探测 206、2,829,338 字节、首字节识别为 mp3、估算 176.8 秒（目录时长 176 秒） |
| Windows 证据（2026-10-20） | MSVC 19.51 /W4：0 条项目警告；`ctest --preset windows-msvc-debug` 179/179 通过（2 项在线用例按设计跳过）；目录集成测试连续 10 次无失败 |
| Linux 证据（CI 运行 39451786357） | ubuntu-24.04，GCC 13.3.0，glibc 2.39：0 条项目警告，179/179 通过；Windows 与容器任务同时通过 |
| 过程问题 | 容器任务的发布构建（GCC 13 -O2）对 T12 酷我搜索解析中已检查的 optional 读取报告 -Wmaybe-uninitialized（调试构建不出现，误报），改为带哨兵的整数读取（提交 fix(kuwo)）；此后对全部源码执行 GCC -O2 严格警告检查无警告 |
| 已知限制 | 歌单／专辑／歌手搜索与集合、分类、歌词归 T21／T23；匹配、自动换源归 T33；context_ref 在出现需要私有上下文的来源时实现；在途合并的跟随者只等到自己的截止时间，领头调用失败时跟随者得到同一错误 |

### T14 下载意图、命令、查询与批次：已完成（2026-10-21，提交 7c610bb）

| 项目 | 记录 |
| --- | --- |
| 交付 | `application/tasks` DownloadService：事务外准备意图（resolve_audio 的账号绑定按显式凭证／默认选择／能力允许的匿名模式固定，目标目录按 ACL、accept_new_tasks 与可用性校验，补全默认值与产物配置版本）、幂等创建与重放、已完成结果复用候选的事务外核验、取消、手动重试、列表与有限批次。任务仓储新增共用的事务内创建核心（按路由的幂等记录、活动意图复用、复核后的完成结果复用、配额、retry_of）、全有或全无的批次创建与查询、带过滤绑定游标的列表。控制入口新增 `POST/GET /api/v1/downloads`、`GET /api/v1/downloads/{task_id}`（弱 ETag、If-None-Match 返回 304）、`POST /api/v1/downloads/{task_id}/cancel`、`POST /api/v1/downloads/{task_id}/retry`、`POST /api/v1/download-batches`、`GET /api/v1/download-batches/{batch_id}`、`POST /api/v1/download-batches/{batch_id}/cancel` |
| 规则落实 | Idempotency-Key 为 16～128 个可打印 ASCII（无空格），按 principal＋方法＋路由＋key 记录，保留 idempotency.retention；请求哈希基于规范化 JSON 请求体（键排序），与意图指纹分开。已提交的 key 先重放首个结果（含状态码），不再读取已变化的账号或上下文；同 key 不同请求 409。活动同意图返回 200 reused，force 与重试遇活动同意图 409 ACTIVE_DUPLICATE。完成结果仅在事务外确认文件仍在目标根内存在，且事务内任务版本、文件资产版本与 available 状态均未变化时复用，否则创建新任务。重试沿用持久意图与账号绑定，重新校验能力、目标目录与绑定可租用，不改选默认账号。批次 1～download_limits.max_batch_items 项，全部预检并列出所有失败项（取首个失败项的错误码），事务内任一项失败（如配额）整批不落库；批次内重复项复用前项；全部复用 200，否则 202。列表按 created_at、task_id 倒序。匹配与自动换源暂返回 CAPABILITY_UNSUPPORTED（T33）；任务在 T17 Runner 加入前只排队，info 仍声明 downloads=false |
| 覆盖的验收 | A01：同 key 同输入只建一个任务并重放首个结果；A02：同 key 不同输入 409 且无副作用；A03：并发 8 次提交同一新意图只建一个任务，其余复用，force 冲突；A35：metadata_policy（skip／required）与 allow_preview 不同的意图互不复用；A36：手动目标以持久 TrackRef 进入意图，不同目标指纹不同、相同目标指纹相同（以领域与仓储替身验证，匹配接口在 T33）；A37：凭证撤销后同 key 重放仍返回首个结果，重试沿用持久绑定；A46：文件缺失时不返回 reused=true 而新建任务，完成结果在核验后被标记缺失（版本变化）时事务拒绝旧证据并新建任务；另有取消（排队即取消 200、运行中 canceling 202、终态 409）、重试的前置与重放、列表分页与游标过滤绑定、批次全有或全无与配额、HTTP 层的 Location、Idempotency-Replayed、ETag／304 与错误映射 |
| Windows 证据（2026-10-21） | MSVC 19.51 /W4：0 条项目警告；`ctest --preset windows-msvc-debug` 191/191 通过（2 项在线用例按设计跳过）；下载与服务集成测试连续 8 次无失败；GCC 16.2 在 -O2 严格警告下检查改动文件 0 条警告 |
| Linux 证据（CI 运行 39455698718） | ubuntu-24.04，GCC 13.3.0，glibc 2.39：0 条项目警告，191/191 通过；Windows 与容器任务同时通过 |
| 已知限制 | 执行、进度与结果写入归 T17；完成结果的复用目前只校验文件存在与版本，品质／试听／元数据证据随 T15～T17 的结果记录补充；任务快照中 actual_track、actual_quality、stage 与 warnings 在执行加入前为空 |

### T15 最小音频文件验证：已完成（2026-10-21，提交 3b257a5、23c2271）

| 项目 | 记录 |
| --- | --- |
| 交付 | `domain/assets/container`：只按前导字节识别容器（MP3／ADTS AAC／FLAC／Ogg／MP4／WAV），并把 HTML／XML、JSON、文本与空正文识别为非音频；资产探测改用它（ADTS 不再被当作 MP3）。`application/ports/processing`：AudioProcessor 端口、ProcessingPlan（processor_id＋版本，ResolvedAudio 默认 `audio.verify` v1）、AudioExpectation、AudioFacts、阶段与警告报告；ResolvedAudio 增加平台声明时长（酷我取 duration）。`audio_processing`（由 INTERFACE 改为静态库）：64 KiB 窗口随机读取、MP3 帧逐帧遍历（跳过 ID3v2／ID3v1／APEv2／Lyrics3v2，首帧须与后续帧成链，Xing／Info／VBRI 头帧不计入音频）、基线处理器 VerifyingProcessor 与按 id／版本查找的 ProcessorCatalog；新增任务错误码 AUDIO_INVALID |
| 规则落实 | 处理器只读工作文件，output 即原文件，保留原始编码。HTML／JSON／文本／空正文为 AUDIO_INVALID（not_audio:*、empty，可重试）；无法识别为 unrecognized，已识别但尚未验证的容器为 unsupported_container:*（均不可重试），不能作为成功音频发布。截断：ID3v2 标签或末帧被切断、文件小于传输声明总字节、帧数少于 Xing／VBRI 声明（truncated，可重试）；大于声明为 size_mismatch。帧间或尾部非帧字节超过 4 KiB 且超过 1% 为 damaged，较少时跳过并警告 DAMAGED_DATA_SKIPPED。时长由帧数 × 每帧采样 / 采样率实测，不从大小或 URL 推算；与平台声明时长相差在 max(3 秒, 3%) 内才作为整曲证据，明显更短改判 preview（再由 evaluate_resource 按 allow_preview 拒绝），更长仅警告；未提供声明时长时完整性维持原判定，已知 preview 不会被升级。另报告容器／码率与声明不符的警告。读取期间检查取消与截止时间。标签写入在 T30 前不实现：metadata_policy=required 在处理阶段返回 CAPABILITY_UNSUPPORTED，best_effort 附 METADATA_NOT_WRITTEN |
| 覆盖的验收 | A19 的必要验证部分（验证失败不得发布）；HTML／JSON／文本／空正文、截断（末帧、标签、声明大小、Xing 帧数）、损坏、未支持容器、未知长度维持 unknown、短片段判为试听并被默认策略拒绝、CBR／VBR／MPEG-2 实测、取消与截止、处理器按 id／版本查找与重复注册拒绝；容器识别的领域样本覆盖 ADTS 与非法 MPEG 头 |
| 真实样本（2026-10-21，Windows，匿名） | 酷我 rid 78932517 标准品质（mobi 接口声明 mp3、128 kbit/s、176 秒），经 HTTPS 下载 2,829,338 字节（Content-Type audio/mpeg）：mpeg1-layer3、44100 Hz、实测 128000 bit/s、6768 帧、176,796 ms，completeness=full，仅有 METADATA_NOT_WRITTEN 警告；截取前 2,000,000 字节的副本返回 AUDIO_INVALID truncated。样本只保存在本机临时目录，未入库；复现方式为设置 MUSIC_SERVICE_AUDIO_SAMPLE（及 MUSIC_SERVICE_AUDIO_SAMPLE_MS）运行 `[online-audio]` 用例 |
| Windows 证据（2026-10-21） | MSVC 19.51 /W4：0 条项目警告；`ctest --preset windows-msvc-debug` 205/205 通过（3 项在线用例按设计跳过）；GCC 16.2 在 -O2 严格警告下检查改动的源码与测试 0 条警告 |
| Linux 证据（CI 运行 39525823045、39526810353） | ubuntu-24.04，GCC 13.3.0：205/205 通过，Windows 与容器任务同时通过。首次运行在测试替身 mock_providers.hpp 中有 4 条 -Wmissing-field-initializers（ResolvedAudio 新字段未初始化），由 23c2271 修复后复跑 0 条项目警告 |
| 已知限制 | 只验证 MP3（酷我标准品质的实际容器）；FLAC／M4A／Ogg／WAV／ADTS 的结构验证、标签写入与解密归 T30 及具体平台；不做完整解码证明；整曲判定依赖平台声明时长；处理器由 T17 的 Runner 在 processing 阶段调用，结果写入任务记录后才参与已完成结果复用的证据复核 |

