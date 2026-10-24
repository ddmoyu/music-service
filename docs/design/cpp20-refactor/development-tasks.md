# 开发任务拆分与验收计划

更新：2026-10-23。已提交基线为 0.1.6（`e13053c`）；N04 QQ 音乐和 N05 酷狗首阶段已通过 Windows 本机验收，代码尚未提交，分别见[QQ 验收记录](platform-roadmap.md#n04-qq-音乐首阶段验收2026-10-23)和[酷狗验收记录](../../research/kugou-acceptance.md)。当前应用版本以根目录 CMakeLists 为准。T01～T19、T21～T23、T29、T30 及 T41（选用 5sing）已完成本机验收；T30.s 侧文件及原生 NCM 导入也已完成本机验收，见补齐计划第 14 节。T20 的容器在线闭环仍保留受阻记录，T28／T31／T32／T34 的跨系统验收及 T37 的系统服务／其他系统发行包仍有缺口。N07 网易云已完成本轮批准范围，真实 Hi-Res 成功样本独立待验收。T26 已取消，后续平台坚持原生实现。**QQ、酷狗与咪咕首阶段已完成，未覆盖的品质与账号能力按子项推进；优先 Windows，非 Windows 的专门适配和验收排到最后。** 新平台的顺序、范围与验收统一见[平台路线图](platform-roadmap.md#当前执行顺序与交付范围)；文末保留历史实施证据，不用其旧状态覆盖最新子项结论。

本文管理实施顺序、依赖与交付关口；规则和字段仍以[模块设计](backend-service/feature-modules.md)、[内部契约](backend-service/internal-contracts.md)、[公共模型](backend-service/data-models.md)、[API](backend-service/api-contract.md)和[运行时](backend-service/runtime-and-tasks.md)为准。A01～A50 的完整场景以[验收矩阵](backend-service/flows-and-acceptance.md#7-故障与验收矩阵)为准；本文只分配落实任务，不另建一套行为规范。

## 任务使用规则

- 用户本轮明确 Windows 优先、新音乐平台优先。按路线图推进 N04／N05／N08 的推荐、排行榜、单曲／歌单／专辑搜索及关联详情和下载；其他系统部署不抢占本轮平台功能；跨平台匹配、手动与自动换源已取消。T20 的容器缺口不作为新平台 Windows 子项的阻塞条件；父任务需要的跨系统证据仍保留待验收，不据此将完整发布关口标为通过。
- 本轮参考项目差距见[功能补齐计划](reference-gap-plan.md)。用户已批准 A～C；A（G01～G03）、B（G04～G06）、C 的 G07.a／G07.b 与 G08 已完成本机验收；G07.c Hi-Res 独立待验收。子项 T21.a／T30.a、T14.a／T30.b／T31.a 对应 A；N07.b／T23.b（网易云集合）、T12.b／T22.b（三平台集合链接）、T14.b／T28.b（有限批次）对应 B。不重写父任务的历史验收结论，C 对应 N07.c／T23.c，记录见计划第 9～11 节。本轮追加 G10～G13（N07.d／T21.d／T23.d／T30.d），明确排除 G09 扫码；交付与验收见计划第 12 节。0.1.5 按最新要求补齐现有三平台接口（酷我歌词／高品质、网易云歌手目录／Hi-Res 请求、5sing 纯文本歌词与文件品质复核），见第 13 节；真实 Hi-Res 成功样本仍待权限，不做扫码。 0.1.6 按用户追加确认完成 G14／G16（原生 NCM 离线导入与产物集合侧文件），T30.s 及对应 T31 子项验收见补齐计划第 14 节；下文父任务保留当时历史结论。
- `T` 表示公共能力或阶段验收任务，`N` 表示已研究的后续原生平台，`X` 表示开放扩展候选。编号保持稳定，后续可拆子任务，不因调整优先级重新编号。
- 表内“直接前置”表示集成交付所必需的任务，已由所有可选路径保证的传递依赖不重复列出；仅在部分候选路径下被覆盖的前置仍保留。`—` 表示没有前置，`ANY(...)` 表示任选一项完成，并在验收记录中固定所选项。确定接口后可用替身并行开发，但前置未验收时不能把依赖它的任务标记为完成。
- 平台优先顺序与技术依赖分开记录。后续平台不天然依赖排在前面的所有平台；一个来源被账号、地区或接口阻塞时，可推进其他已具备条件的任务。
- 状态采用未开始、进行中、受阻、待验收、已完成。受阻记录具体缺口与恢复条件；完成必须附实现版本、验证命令、结果及适用系统／账号／能力范围，不以文档存在、编译成功或 mock 通过代替真实交付。
- 可选功能和平台单独验收。没有完成的功能保持关闭，不列入支持清单；S2 的基础关口不要求所有平台上线，S3 也不等待所有平台扩展结束。

### 平台实现原则

本项目的意义就是自己解析、自己下载。所有平台都由服务内的 C++ 原生代码完成协议、签名、解密与传输，**不引入任何外部第三方下载服务或工具**：不调用 yt-dlp、spotDL、外部签名器、其他语言 SDK 或 FFmpeg 等程序，也不经过第三方解析接口。某项能力无法原生实现时，记为受阻或不支持并保持关闭，不以外部工具补齐。开源项目只作为协议与字段映射的参考资料。

## 总体顺序与交付关口

首条交付主线是 **S0 基础服务 → S1 酷我下载闭环**。完成后分别推进平台扩展与下载可靠性；可靠性具备后接入在线媒体和发布集成。

| 阶段 | 推荐执行顺序 | 可并行的工作 | 通过关口 |
| --- | --- | --- | --- |
| S0 基础服务 | T01～T02 → T03～T09 → T10 → T11 | 端口确定后，文件／数据库、runtime、注册表、传输可分支推进；T07、T09 完成后即可开始 T12 | T11：Windows／Linux 和最小容器可独立启动、健康、停止 |
| S1 酷我闭环 | T12 → T13 → T14～T19 → T20 | T12 与 S0 后半并行；音频最小验证／发布、任务命令、事件和文件出口按各自前置推进 | T20：酷我标准品质可持久下载、重启恢复和取回文件 |
| S2 平台扩展 | T21 → 第二平台 → T41 → T23 → T28 | 当前第二平台关口已通过，按平台路线图推进新增 N 任务；T24／T27 保留后续计划 | T28：两平台与集合基础通过；海外来源和授权按启用组合另行验收 |
| S3 下载可靠性 | T29、T30 → T31 → T32 | T29／T30 在 T20 后即可开始，与 S2 平台扩展并行 | T32：续传、处理、重试与故障恢复通过，不以全部平台完成为条件 |
| S4 在线媒体 | T34 → T36 | 代理／受控直链 | T36：实际启用的媒体通路分别有验收证据；不含匹配与换源 |
| S5 发布集成 | T37 → T38 → T39 → T40 | S0 起积累部署证据；T32 后可开展正式打包，不必等待可选功能 | T40：选定发布组合的目标运行、升级和恢复通过 |

S0／S1 对扩展契约采用“端口完整、实现最小”的原则：AccountBinding、AdmissionRequest、ProviderBundle 等类型一次定好字段，但只实现酷我闭环实际需要的维度与模式；应用 token、共享应用配额组和大规模来源分别在 T24、T28 实现并验收，不作为 T11／T20 的通过条件。

T20 可作为首个可用的酷我预览版本；正式发布基线按 T40 管理。阶段不是一个全平台串行队列：S3 基线通过不表示 S2 的所有扩展已完成，发布清单必须逐项列出已验收能力。以下任务表给出精确依赖，不能仅按编号推断前置关系。

## 模块与任务归属

| 模块／目录 | 首次任务 | 后续任务与边界 |
| --- | --- | --- |
| `domain`、`application/ports` | T02 | 各任务按需要补端口；领域不依赖 HTTP、JSON、SQLite 或系统 API |
| `infrastructure/config`、`observability`、`filesystem`、`secrets`、`storage` | T03～T05 | T31 恢复与清理；文件系统提供原语，下载发布流程由 downloads 持有 |
| `runtime` | T06 | T17 接入调度、T29 策略增强；统一上游账本不复制到各模块 |
| `providers/registry`、`providers/<source>` | T07、T12 | T22／T41、T27、N／X 平台；协议、签名、解析与平台授权在平台包内 |
| `application/accounts`、`infrastructure/auth` | T08 | T24 OAuth／刷新；前者协调平台凭证，后者处理服务 token，职责不同 |
| `infrastructure/transport` | T09 | T29 传输可靠性、T34 流桥接；不拥有第二套下载队列 |
| `src/server`、`api/httplib` | T10 | 随每项用例补路由；仅组装、生命周期和协议转换，不最后集中补 API |
| `application/catalog`、`application/assets` | T13 | T21／T23 集合与发现；解析与下载仅使用原来源 |
| `application/tasks`、`downloads` | T14、T16～T17 | T29／T31 强化；意图归一属于 application，执行与发布属于 downloads |
| `audio_processing` | T15 | T30 标签、容器与解密；不控制账号、任务调度或平台匹配 |
| `application/events`、`event_server/httplib` | T18 | 提交后通知与恢复快照，不保存另一份任务事实 |
| `application/media`、`media_server/httplib` | T19 | T34 在线代理／直链；本地文件出口不依赖在线代理开关 |
| `tests`、`packaging` | T01、T11 | 测试随功能交付，T20／T28／T32／T36 为集成关口，T37～T40 为发布任务 |

目录与编译依赖以[模块与目录](backend-service/modules-and-layout.md)为准，不为每个任务新建库，也不提前创建所有候选平台的空目录。

API 按功能随任务交付，路由与字段只在[API 契约](backend-service/api-contract.md)维护，下面用于防止漏项：

| API 范围 | 实施归属 |
| --- | --- |
| 地址／版本、权限、错误、请求预算与通用约定 | T10 建立中间件，各用例任务验证自身 scope、错误和幂等语义 |
| health、info、providers、limits、destinations | T10；平台注册／调用方能力分别依赖 T07／T08 |
| search、tracks/detail、assets/resolve、assets/probe | T13；多源目录由 T21 扩展，同源解析 |
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
| T02 | 领域规则与最小应用端口 | T01 | S1 所需类型、Result、错误、品质、身份、意图／指纹、任务迁移，以及平台、存储、传输、执行器端口 | 公共头不泄漏第三方类型；独立样本覆盖 strict／试听／未知完整性、稳定身份、指纹与非法迁移；用替身验证端口 |
| T03 | 配置、时钟与可观测性基础 | T02 | 默认值、配置校验、路径配置、预算所需时钟、结构化日志与关联 ID；为计数和延迟预留实际采集点 | 无效端口／配额／路径配置在启动前明确失败；日志不含 token、Cookie 或带秘密 URL；时钟可用于确定性超时验证，配置行为与部署文档一致 |
| T04 | 文件系统、工作目录与 SecretStore | T03 | data_dir／destination 分工、实例锁、路径约束、权限、不可变秘密引用与临时文件原语 | 第二实例拒绝；越界路径拒绝；中文路径与权限失败可定位；提供根规范化与安全文件创建原语，T05／T10 落实不可变目录绑定；秘密不进入普通输出 |
| T05 | SQLite 与仓储基础 | T04 | WAL、单 writer、有限读连接、迁移及身份／默认凭证／目录绑定／任务／资产／发布意图仓储 | 初始化／重开、事务回滚、唯一约束及版本更新通过；默认选择与凭证撤销原子提交；目录 ID 绑定根不可改；writer 不执行网络和文件处理；支撑 S1 恢复 |
| T06 | 执行器、取消与统一上游入场 | T03 | 有界队列、取消／deadline、UpstreamAdmissionPort 联合租约和停机原语；S0 实现全局、principal、source、origin 四个维度，共享应用配额组只保留端口字段，由 T24 启用 | 满额返回 Deferred 而不占 worker 等 semaphore；无部分预留泄漏；不同来源可推进；取消／超时释放资源；额度仅在真实操作结束后归还；A43 的运行时部分通过 |
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
| T14 | 下载意图、命令、查询与批次 | T13 | 事务外意图归一、指纹、幂等、活动唯一约束、已完成资产复用复核、提交／取消／手动重试／查询 API、有限批次 | A01～A03、A35、A46 的领域／仓储契约通过；同 key 重放先取原结果，不重读过期句柄；策略不同不误复用；手动 retry 创建新任务；并发有版本保护 |
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
| T41 | 第二个真实平台扩展关口 | ANY(T22,N01,N02,N04,N05,N06,N07,N08) | 使用酷我和任选一个已完成的原生扩展平台，验证注册、能力差异、账号隔离和公共下载复用；优先采用 T22 | 两个真实来源通过，不改通用路由／状态机／平台专表；记录所选平台任务和证据。任一候选受阻可换另一项；候选自身不依赖 T41，不能拿模拟平台替代真实证明 |
| T23 | 已接平台的集合功能落地 | T41 | 对实际通过 T41 的来源补支持的歌单／专辑／歌手／分类／榜单／推荐／歌词／链接，按页提交有限批次 | 每种操作有契约样本；上线能力另有真实验证；分页不串源；无歌词与不支持歌词分别响应；不强求各平台具备全部集合能力 |
| T24 | OAuth、API key 与刷新扩展 | T20 | 按首个需要的平台补授权 profile、Verifier／Refresher／ApplicationTokenProvider、刷新合并、CAS、应用 token 缓存和稳定配额组 | A40～A41 通过；并发刷新仅一次在途，等待者可分别超时；更新／撤销赢过旧结果；不同 scope 的 token 独立身份；无同池递归等待；新授权协议只扩平台包／注册，不修改凭证服务的平台分支 |
| T26 | ~~可选外部 helper 基础设施~~ | — | **已取消**（2026-10-22）：项目不引入外部下载工具或 helper 程序，见[平台实现原则](#平台实现原则)；曾交付的实现（c58233f）已由 e06a076 撤回 | — |
| T27 | YouTube 原生音源 | T41 | 原生解析视频页与播放器接口，自行实现播放器脚本中的签名与 n 参数变换，提取独立 HTTP 音轨交给公共传输；稳定视频身份、品质和过期地址处理 | 签名／n 参数有固定向量，并有跟进播放器变更的验证方式；固定样本与有权限在线样本分别验证；音频真实格式／身份正确，取消与凭证隔离有效；只能分段或必须转码的结果明确不支持；不调用 yt-dlp 等外部工具，无法原生实现的部分记为受阻 |
| T28 | S2 基础扩展关口 | T23 | 两个真实平台、集合／发现契约、多源部分失败、100 来源有界发现／调用、上下文升级和平台禁用后的恢复 | 声明能力与 API 一致；未使用来源无线程／网络开销；旧上下文按稳定引用重解析或明确失败；禁用来源不静默换源，已发布文件仍可读；新增现有能力来源不改通用业务。T24、T27 启用时另验收，不因受阻拖住 S3 |

T27 原生音源按独立平台任务推进。T24 的通用协调逻辑保留，按后续实际需要的授权平台验证。任何平台都不借助外部下载或转码工具。

## S3 下载可靠性与文件处理任务

| ID | 模块／任务 | 直接前置 | 交付范围 | 验收标准 |
| --- | --- | --- | --- | --- |
| T29 | 续传、重试与速率策略增强 | T20 | Range 校验、资源一致性、过期 URL 重解析、429／5xx 冷却、重试与持久预算、限速和取消竞争 | A13～A16、A22、A25～A26、A43～A45 的传输／预算部分通过；200 不追加、错误 206 不混合、416 不直接成功；恢复不重置 deadline／attempt；一个来源冷却不阻塞其他来源 |
| T30 | 容器、标签与解密处理扩展 | T20 | 选择并固定标签库，按 MP3／FLAC／M4A 能力补验证、标签／封面／歌词写入、侧文件和解密端口；处理器按真实平台需要增加 | A18～A19、A35 的真实文件部分通过；重复写入、未知标签、载荷保真、临时空间与回退有证据；optional 失败有警告，required 失败不发布；首批平台不需要的解密只验证端口，不能标记为已支持 |
| T31 | 故障恢复、清理与迁移强化 | T29、T30 | 发布各崩溃窗口、侧文件一致性、磁盘耗尽／目标离线、过期工作文件清理、资产版本复核、数据库迁移与备份恢复 | A12、A17、A20～A22、A27、A46 在故障注入下通过；清理不删除在用文件；重启不重复下载／误覆盖；资产复用检查与提交之间变化不会返回错误 reused=true；迁移失败有可验证恢复路径 |
| T32 | S3 可靠性与资源关口 | T31 | 组合验证断流、重启、磁盘、发布、处理、限速和多任务运行；记录资源与延迟 | 所有启用格式／处理器有独立样本；线程、连接、队列、内存均符合配置上界；故障无伪完成或混合文件；给出运行参数和实测结果，不以“高并发”代替指标；不等待 N／X 平台全部完成 |

T30 的处理器可用本地元数据样本开发；接入在线歌词／封面时，附加依赖 T21 及对应平台能力的完成证据。平台特有解密在 N08 等任务落实，避免核心处理模块反向等待平台。S1 的安全发布一直保留，S3 不用于补救一个允许错误完成的首版。

## S4 在线媒体任务

| ID | 模块／任务 | 直接前置 | 交付范围 | 验收标准 |
| --- | --- | --- | --- | --- |
| T33 | 已取消：手动匹配与换源 | — | 2026-10-24 用户明确不做 | 从待开发与验收范围移除 |
| T34 | 在线代理、受控直链与流桥接 | T32 | 在线资产访问描述、proxy／direct／auto、背压、Range、刷新与稳定资源身份、媒体池与文件出口隔离 | A24 的上游部分、A30～A33 通过；秘密头不交给客户端；票据与资产过期分别处理；慢读／断开回收，URL 更新不拼接不同音频；两个客户端不共享播放状态 |
| T35 | 已取消：自动匹配与换源 | — | 2026-10-24 用户明确不做 | 从待开发与验收范围移除 |
| T36 | S4 在线媒体集成关口 | T34 | 代理与受控直链的实际媒体通路 | Range、过期、撤销、断连与资源有界；各启用平台分别有实测证据 |

S4 仅验收实际启用的在线媒体能力；T33、T35 已取消，不再作为任何阶段的前置任务。

## S5 部署与发布任务

| ID | 模块／任务 | 直接前置 | 交付范围 | 验收标准 |
| --- | --- | --- | --- | --- |
| T37 | 原生包与系统服务 | T32 | 选定 Windows／Linux 发行 preset、安装布局、依赖／证书／许可证、Windows 服务与 Linux 服务集成、ARM64 构建 | 干净机器无开发环境可运行；服务账号权限、中文／长路径、日志和停止行为通过；记录目标最低系统／运行库；ARM64 需要目标运行证据，只有产物不标正式支持 |
| T38 | Docker 多架构与 NAS 部署 | T37 | amd64／arm64 镜像、Compose、非 root、配置／数据／下载卷、端口与信号、选定 NAS 环境 | 目标架构上启动／HTTPS／下载／SIGTERM／重建恢复通过；data_dir 符合本地 SQLite 与单实例约束；NAS 下载目录离线／权限／发布失败可恢复；不把一台 NAS 结果推广到所有文件系统 |
| T39 | 目标矩阵与运行验证 | T38 | 按部署矩阵执行功能、负载、升级、备份恢复、证书、权限和资源验证；固定每个包的功能组合 | 每个正式声明的系统／CPU／运行库／部署组合都有实测报告；启用功能已通过各自任务与关口；超配额响应和资源上界可观察；缺失目标运行证据的组合标实验或不发布 |
| T40 | 发布关口与运维文档 | T39 | 版本、依赖清单、平台能力与验证日期、安装／配置／升级／恢复说明、发行包清单与已知限制 | 从空目录按说明部署成功；示例无秘密；可回查代码／配置／依赖版本和目标报告；备份恢复有效；所选 S2／S4／helper／平台功能附各自验收，不因主程序打包成功默认全支持 |

T37～T40 默认以通过 T32 的下载服务为基线；发布 S2 集合功能另需 T28，发布 S4 按实际启用任务加验，不能用关闭功能规避已声明能力的验收。系统与架构范围以[部署矩阵](backend-service/configuration-and-deployment.md#部署方式)为准。

## 后续平台任务

推荐顺序与难度只在[平台路线图](platform-roadmap.md)维护；本节给出任务与技术依赖。酷我协议包为 T12、产品闭环为 T20，5sing 为 T22，其他平台见下表。Apple Music 不进入本次清单。

T41 已通过本机验收，本轮按平台路线图扩展国内平台的发现与搜索；YouTube 原生 adapter 保留后续计划。候选平台直接复用已有公共能力和 T20 的 Windows 下载基线，不要求先补完容器或其他操作系统验收，也不要求先完成所有历史排序较早的平台。

| ID | 模块／任务 | 直接前置 | 交付范围 | 验收标准 |
| --- | --- | --- | --- | --- |
| N01 | Jamendo | T21 | 已实现 20 项匿名能力；三类搜索、官方精选歌单搜索、原生歌曲分类、官方歌曲榜、详情与集合分页、歌词及标准下载 | 用户确认精选歌单搜索与歌曲分类范围；高品质／无损的匿名接口核实及 Windows 闭环见[验收记录](../../research/jamendo-acceptance.md#第二阶段发现能力与音质核查) |
| N02 | 千千 | T21 | 参数排序、时间戳、MD5；单曲／专辑搜索、详情、集合分页、分类与热门歌单、链接、歌词／封面和三档音源 | 0.1.11 协议包实现，签名向量、权限／试听与 Windows 验收见[接入记录](../../research/qianqian-acceptance.md)；未接歌单搜索、榜单、歌手目录、登录和 Hi-Res |
| N04 | QQ 音乐 | T21 | 推荐、榜单、单曲／歌单／专辑搜索、详情及分页曲目、链接、歌词／封面；songmid／文件名、vkey／CDN、Cookie 与标准品质；逐端点确认签名 | 按路线图逐项通过 Windows 目录与下载验收；资源／品质映射、权限、过期地址和错误样本通过；高品质与歌手扩展分别报告，不靠静默降级完成 |
| N05 | 酷狗 | T21 | 已实现推荐、榜单、四类搜索、歌手资料与作品分页、集合详情／曲目、链接、歌词／封面；移动端详情核对多品质 hash 与权限 | 协议包 0.1.17 已通过 Windows 歌手目录与标准整曲闭环，37 项离线用例通过并修复三处边界问题，见[验收记录](../../research/kugou-acceptance.md)；高品质／无损成功样本待验收，App 凭证、Hi-Res 与加密音源未接入 |
| N06 | Bilibili | T21、T30 | bvid／cid／分 P、必需请求头及按端点需要的 WBI、独立音轨与真实容器验证 | 独立音轨可保存／验证；不引入未支持分段流程；UP 主不映射为演唱者；默认不作为音乐匹配候选；取消和过期地址行为符合公共契约 |
| N07 | 网易云 | T21 | 闭环必需端点的 Linux／EAPI／WeAPI 包装、MD5／AES／按需 RSA、Cookie 与品质 | 协议固定向量与解析样本通过；不为未用端点实现全部包装；账号／权限／品质闭环实测；协议加密不与 TLS 选型绑定 |
| N08 | 咪咕 | T21、T29、T30 | 首阶段已实现 18 项能力：官网推荐／分类／榜单、四类搜索、普通与数字专辑／歌单分页、详情、链接、歌词／WebP 封面及原生音源 | Windows 构建、19 项专项测试、434 项全量回归及匿名标准下载／复用／重启回读通过，6 项原有测试跳过，见[验收记录](../../research/migu-acceptance.md)；高品质／无损成功下载待权限样本，Hi-Res／加密与账号相关子项仍未完成 |
| X01 | SoundCloud | T21 | 原生 API／协议，按能力接搜索／链接、曲目／集合及可验证音源 | 账号和分页范围明确；独立 HTTP／分段／试听分开处理；只有通过单平台契约与在线验证的能力可启用 |
| X02 | Bandcamp | T21 | 曲目／专辑链接、目录、实际可下载音频 | 不假设全站搜索存在；区分试听与整曲、格式与权限；未验证音源时只交付目录能力 |
| X03 | Audius | T21 | 原生 API、曲目／歌单／作者及音源，节点与地址变化 | 鉴权、分页、节点切换、资源身份和目标传输经过验证；必要文件处理另需 T30，不把节点可达作为下载通过 |

X 任务的原生协议路线在其协议准备子任务中确定，并把 T30 等条件前置记入执行记录，之后才能集成；无法原生实现的来源记为受阻。新的平台持续追加同类任务，不以表中数量作为上限。

## 单个平台的任务模板

平台包与产品集成分别验收。T12 只包含 `.1`～`.4`，酷我 `.5` 由 T20 承担；其他平台已有 T20 下载基线，父任务包含全部适用步骤。协议包完成不等于产品已支持该来源，也不从第三方工具的站点清单批量声明能力。

| 子步骤 | 前置 | 交付与完成条件 |
| --- | --- | --- |
| `.1` 协议准备 | 父任务技术前置可用 | 确认当前端点、稳定 ID、能力、分页、品质、凭证／地区、签名和传输形态；保存脱敏样本、验证日期与缺口；确定原生实现所需的新增源码依赖 |
| `.2` 平台包 | `.1` | descriptor、能力／授权工厂、协议／解析、固定向量；仅实现已选择能力，无平台自有队列／SQL／通用路由分支 |
| `.3` 离线契约 | `.2` | 成功／空／缺字段／权限／限流／失效／取消样本，完整身份隔离与上下文版本测试；目录型来源不被强制提供音源 |
| `.4` 协议包在线验证 | `.3` | 记录账号／地区／时间／系统；验证真实目录请求与音源解析证据，必要时由传输验证程序受限取样，不要求持久任务、媒体或 SSE 已实现 |
| `.5` 产品集成与启用 | 酷我：T17、T18、T19；其他来源：`.4` 与 T20 | 酷我由 T20 验收完整下载／恢复／文件通路；后续音源平台在父任务内复用基线验证同样通路，目录型来源只验其目录能力。完成后才在指定发行组合声明已支持 |

跨平台匹配与换源已取消。新增一种全新公共能力时先更新端口和相应设计，再建任务。

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
| A36 | 已取消 | 手动跨源目标不在验收范围 |
| A37 | 已取消 | 匹配句柄不在验收范围；同源恢复与幂等仍需验证 |
| A38 | T08、T13、T24 | T20：同 source 两个 version=1 用户凭证的隔离；T24：应用 token 条目隔离；对应授权平台：真实应用 token |
| A39 | T08、T13、T17 | T20：迟到结果与固定绑定；T24：真实刷新后复验 |
| A40 | T24 | 授权扩展关口：刷新／人工更新／撤销竞争；对应授权平台在线复验 |
| A41 | T07、T24 | T07：工厂注册校验；T24：新授权协议接入 |
| A42 | T09、T36 | T09：逐跳租约；T36：模拟跨源；对应平台完成后验证真实跨源组合 |
| A43 | T06、T17 | T20：入场延后释放槽与来源隔离；T29 复验冷却 |
| A44 | T17 | T20：admission 不消耗尝试、代次改变；T29 复验实际重试 |
| A45 | T24、T29 | 启用应用 token 时验收；T32 复验稳定组与真实 I/O 结束 |
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
| 平台端点、签名向量、账号／地区、品质与工具路线 | T12、T22、T27、各 N／X 的 `.1` | 留存脱敏样本和验证日期；缺口对应具体关闭的能力，不凭旧研究推定可用 |
| OAuth／API key 范围与授权工厂 | T24 | 竞争、取消和隔离契约通过，声明能力与绑定一致 |
| MP3／FLAC／M4A 标签覆盖、解密与可选工具 | T30、N08 | 实际文件与载荷验证通过，失败策略可复现，工具仅在对应功能组合启用 |
| 默认资源配额、延迟目标与设备适用范围 | T06、T20、T32、T39 | 先声明硬件、并发、负载和通过阈值，再记录实测；无统一硬件数据时不编造通用性能承诺 |
| ARM64、NAS、Docker 多架构与最低系统发布范围 | T37～T40 | 每个正式组合有目标运行证据；其余明确为实验／候选，不外推支持 |

一项开发任务完成至少应具备：

1. 交付范围内的实现、配置和文档一致；未覆盖能力关闭，公共依赖方向未被破坏。
2. 相应领域／协议、集成或契约检查通过，失败路径有独立样本或故障注入；不只验证正常返回，也不为简单转发重复造测试。
3. 变更影响的平台组合有构建与运行证据；平台在线验证单独记录时间、账号条件与结果，秘密不得进入报告。
4. 记录任务 ID、实现提交、直接前置状态、验证命令、报告位置、已知限制和后续子任务；阶段关口只聚合这些证据，不替代日常功能验证。

T20（S1 酷我端到端关口）除容器内的在线下载闭环外均已通过：须在能访问酷我音源接口的网络上运行 `packaging/docker/e2e.sh` 后转为已完成，S1 随之通过。T29 的实现只依赖 T17 的执行代码，已先行交付；T30（标签库 TagLib v2.3.2，按 MPL-1.1 静态链接）已交付标签与 FLAC／M4A 验证，侧文件留作子任务 T30.s。T21 交付公共集合用例与路由；T22（5sing）作为第二个原生平台接入，T41 以酷我与 5sing 验证扩展关口；T23 为两平台接入歌单、专辑、榜单、分类与推荐；T28 以两平台与 100 个模拟来源验证 S2 基础扩展关口；T31 补齐清理、复用证据与数据库备份恢复；T32 以故障组合与资源计量验证 S3 关口；T34 交付在线代理、受控直链与流桥接；T37 交付发行包、Windows 服务与 systemd 集成。T33、T35 已取消。T24 仍依赖 T20。T26 已取消：项目只做原生解析与下载，不引入外部工具，T26 的实现已撤回，T27 改为 YouTube 原生解析，X01～X03 去掉 helper 路线。当前没有设置工期或人员分配，避免把尚未完成的平台协议验证估算为确定日期。

## 实施记录

以下记录保留各次交付时的事实。记录中关于 T33／T35、A36／A37 及未来换源的描述已由 2026-10-24 的取消决定取代，不代表当前待办。运维观测的新接口、计量口径与日志轮转见[运维观测](../../operations-observability.md)。

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

### N07 网易云（第一阶段）：进行中（2026-10-22，标准品质匿名闭环与 Cookie 凭据通过；真实账号未验证）

| 项目 | 记录 |
| --- | --- |
| 前置 | T21 已完成；下载基线 T20 受阻（仅容器在线闭环），本平台的下载闭环另行实测 |
| 协议准备（.1，2026-10-22，中国大陆网络，匿名） | 网页端 weapi 可匿名使用：`cloudsearch/pc`（`cloudsearch/get/web` 对 weapi 返回 50000005，不用）、`v3/song/detail`、`song/enhance/player/url/v1`、`song/lyric`、`w/nuser/account/get`。明文 `/api/*` 同样可用，但统一采用网页端 weapi，只实现这一种包装（不实现 EAPI／Linux API）。fee=0／8 的歌曲匿名可得 128 kbit/s MP3；fee=1（会员）匿名为单曲 code -110；无版权与不存在的 id 为 404。CDN 返回 http 地址，同一地址 HTTPS 可用、带强 ETag（即 md5）与 Range，无需 Referer 或 Cookie。无效 Cookie 与不带 Cookie 的账号接口应答相同（account 为 null）。样本见 `tests/fixtures/providers/netease`（签名与查询串已脱敏；登录态账号、301、405 为按结构构造的样本） |
| 交付 | 平台包 `providers/netease`（netease-native 0.1.0）：weapi 加密以 Mbed TLS 原生实现（AES-128-CBC 两层、base64、无填充 RSA），新增依赖只有已固定的 mbedcrypto；能力 search_tracks（游标为 offset）、track_detail、parse_links、lyrics（匿名），resolve_audio（standard，凭据可选），verify_credentials；授权方式 `web_cookie`（cookie 类，单个秘密字段 cookie，scopes 含 vip）。Cookie 只作为秘密请求头发往 music.163.com，`__csrf` 作为 csrf_token；没有 MUSIC_U 不发请求即 invalid。音源规则与错误映射见 `src/providers/netease/README.md`。默认构建清单加入 netease（`kuwo;fivesing;netease`）；`kuwo_e2e.py` 增加 `--source netease` |
| 离线契约（.3） | `netease_test.cpp`（`[netease]`，14 项）：weapi 与独立 Python 实现的固定向量逐字节一致；各请求的方法、URL、csrf 与加密前 JSON；Cookie 解析与只走秘密头；搜索映射、空结果、损坏应答；顶层 code 301／405／-460／50000005；详情（歌手与专辑 id、不存在、id 不符）；音源证据（HTTPS 化、CDN 域名限制、-110、404、试听标记）；歌词、翻译、纯音乐占位；账号登录与否；链接六种形式与十种拒绝；能力对象的游标、参数校验不发请求、503、入场受限；匿名不带 Cookie、账号调用带 Cookie 与 csrf 且请求体不含 Cookie；会员曲目 CREDENTIALS_REQUIRED、无版权 NOT_FOUND、试听为 preview 且无品质；严格 high 不发请求即 QUALITY_UNAVAILABLE；校验器的 valid／invalid／错误；bundle 注册与 web_cookie 可校验 |
| 在线验证（.4，2026-10-22，Windows，匿名） | `[online-netease]`（MUSIC_SERVICE_NETEASE_ONLINE=1）：搜索“周杰伦”10／277 条且第二页不重复；详情《屋顶》；歌词与纯音乐 unavailable；5257138 解析为 standard、full、HTTPS、128001 bit/s；210049 为 CREDENTIALS_REQUIRED；186016 为 NOT_FOUND；失效 Cookie 校验为 invalid |
| 产品闭环（.5，2026-10-22，Windows，匿名） | `python -I packaging/e2e/kuwo_e2e.py --binary …music-service.exe --source netease` 3.8 秒全部通过：搜索“屋顶”、详情、resolve 为 standard／full／mp3／128001、202 创建与 SSE 到 completed、文件「周杰伦, 温岚, 吴宗宪 - 屋顶.mp3」5,106,944 字节（CDN 5,105,833 字节加 ID3v2.4 标签）、票据 Range、客户端中途断开、取消、强制结束后重启恢复、幂等重放与复用。经服务 API 创建 web_cookie 凭据并校验：失效会话与缺 MUSIC_U 均为 invalid；会员曲目匿名 resolve 为 422 CREDENTIALS_REQUIRED |
| Windows 证据 | MSVC 19.51 /W4：0 条项目警告；`ctest --preset windows-msvc-debug` 331 项全部通过（6 项在线用例按设计跳过） |
| 未验证与限制 | 真实登录账号（有效 MUSIC_U）的校验、会员曲目解析与试听判定只有离线样本，待用户提供 Cookie 后在线验证；high／lossless 未实现（需会员账号验证，按任务要求单独验收）；歌单、专辑、歌手、榜单与集合链接未实现；短链 163cn.tv 不支持；Linux／macOS 待 CI |

### T26 可选外部 helper 基础设施：已取消（2026-10-22）

| 项目 | 记录 |
| --- | --- |
| 决定 | 项目只做自己的解析与下载，不引入任何外部第三方下载服务或工具（见[平台实现原则](#平台实现原则)） |
| 处理 | 曾交付的 helper 协议、进程运行器与入场计量（c58233f）由 e06a076 撤回；同时删除早已存在但从未启用的外部入口：配置 `external_adapters`、`providers.<source>.implementation=external`、ExecutionMode::external 与 AdmissionRequest.helper_processes；`execution_mode` 在 API 中固定为 native。设计文档去掉外部适配路径，T27 改为 YouTube 原生解析，X01～X03 去掉 helper 路线 |
| 影响 | 含 `external_adapters` 键或 `implementation: external` 的配置文件现在启动校验失败 |

### T37 原生包与系统服务：待验收（2026-10-22，Windows 发行包本机通过；服务与 Linux 待验证）

| 项目 | 记录 |
| --- | --- |
| 前置 | T32 待验收（Windows 通过） |
| 交付 | ① 发行 preset 与包：`windows-msvc-release`／`linux-gcc-release` 加 `--target package`，CPack 生成 `music-service-<版本>-<系统>-<架构>.zip`（Windows）／`.tar.gz`（Linux），布局为 `bin/music-service[.exe]`、`share/music-service/{README.md, config.example.json, licenses/, service/}`（`cmake/Packaging.cmake`）；数据不在安装目录。② 运行库：所有 MSVC 目标（含第三方）改为静态运行库（`CMAKE_MSVC_RUNTIME_LIBRARY`），无需 VC++ 运行库；Linux GCC 程序静态链接 libstdc++／libgcc（`MUSIC_SERVICE_STATIC_CXX_RUNTIME`，缺省开），只依赖 glibc。③ 许可：随包附 Mbed TLS、cpp-httplib、nlohmann/json、TagLib（MPL-1.1）、utfcpp 的许可文本与 THIRD-PARTY.md（SQLite 为公有领域，Catch2 不随包）。④ Windows 服务：新命令 `service` 在 SCM 下运行 serve（`src/server/windows_service.cpp`）：上报 START_PENDING → 就绪后 RUNNING，STOP／SHUTDOWN 走与 Ctrl+C 相同的收尾，STOP_PENDING 等待提示 60 s，退出码作为服务专用退出码；服务没有控制台，日志、就绪行与启动后错误写 `data_dir/logs/music-service.log`（serve 新增 log_to_file），配置错误等更早的错误写应用程序事件日志；在 SCM 之外运行给出提示并以 2 退出。`install-service.ps1`／`uninstall-service.ps1`：配置在 `%ProgramData%\music-service`，以管理员 init 后把数据目录属主交给虚拟账户 `NT SERVICE\music-service`（secrets 的 OWNER RIGHTS 规则因此只授予服务账户），授予配置只读与下载目录修改权限，自动启动、失败重启，引导 token 移到仅管理员可读的位置；安装脚本把程序复制到安装目录（缺省 `%ProgramFiles%\\music-service`）并授予服务账户读取执行权限（用户目录中的包服务账户读不到）；卸载默认保留数据。Windows 的仅所有者 ACL 加入 Administrators 组（设计文档同步修改），否则服务账户拥有数据后管理员无法执行离线管理命令。⑤ Linux：加固的 systemd unit（User=music-service、StateDirectory、ConfigurationDirectory、ProtectSystem=strict、NoNewPrivileges、SIGTERM＋TimeoutStopSec=60、Restart=on-failure、退出码 2 不重启）与 `install.sh`（建系统用户、复制到 /opt/music-service、写配置、以服务用户 init、下载目录 ReadWritePaths drop-in、启动并等待就绪；不改下载目录属主，无写权限时给出 setfacl 提示）。⑥ 验证脚本：`package_smoke.ps1`、`service_smoke.ps1`、`package_smoke.sh [--service]`，并加入 CI（Windows 与 Linux 作业各增加发行包与服务步骤） |
| 本机证据（Windows 11 x64，MSVC，2026-10-22） | `music-service-0.1.0-windows-x86_64.zip`：程序 5,572,096 字节，包内无 DLL；dumpbin 导入只有 advapi32、bcrypt、ws2_32、crypt32、kernel32 与 api-ms-win-core-synch，无 C/C++ 运行库；解压到 237 字符的中文深路径后 `package_smoke.ps1` 通过（version、check-config、init、serve 就绪、healthcheck、token、强制结束与重启；`service` 在 SCM 之外以 2 退出并提示）。Debug 改为静态运行库后 `ctest --preset windows-msvc-debug` 316 项全部通过 |
| 未验证 | Windows 服务实际安装运行（本机会话无管理员权限；`service_smoke.ps1` 需管理员 PowerShell 或 CI）；Linux 发行包、静态 C++ 运行库与 systemd 服务（本机无 Linux／WSL，待 CI 的 `package_smoke.sh --service`）；在全新、未装开发工具的机器上运行（本机装有 VS，导入检查替代但不等同）；ARM64 未构建，不标支持 |
| 最低系统 | Windows：x64，Windows 10 1809／Server 2019 起（与构建所用 SDK 目标一致，尚无更低版本实测）；Linux：glibc 不低于构建机（ubuntu-24.04 为 2.39，package_smoke.sh 输出实际所需版本），需发行版 CA 证书 |
| 附带修正 | T41 关口用例比较两源完整状态序列，SSE 合并快速更新时首个 queued 可能被合并，偶发失败（完整测试中出现 1 次）；改为检查每个任务的状态按状态机顺序出现且以 completed 结束 |
| 已知限制 | 只有 zip／tar.gz，没有 MSI、deb、rpm 与代码签名；服务停止只上报一次等待提示，不逐步推进检查点；Linux 包的 glibc 下限取决于构建机，未用旧发行版构建；macOS 无发行包与 launchd 集成 |

### T34 在线代理、受控直链与流桥接：待验收（2026-10-22，Windows 通过，CI 待运行）

| 项目 | 记录 |
| --- | --- |
| 前置 | T32 待验收（Windows 通过）；T19 已完成 |
| 交付 | ① media-access 支持 asset：proxy＋ticket（票据期限取 ttl 与 asset 到期的较早者）、proxy＋bearer（`/media/v1/assets/{asset_id}`，expires_at 为 asset 到期）、direct（返回上游地址，期限同样受 asset 限制）与 auto；direct 需 direct_media_urls（否则 501 FEATURE_UNAVAILABLE），且地址须由平台声明可外发（ResolvedAudio 新增 shareable，默认 false，酷我与 5sing 均未声明）、不需要请求头或账号秘密、不需解密（否则 422 DELIVERY_UNAVAILABLE）；auto 仅在这些条件全部满足时选 direct，否则 proxy；proxy 需 media_proxy（否则 501）；需解密的音频两种方式都不可播放（422）；访问描述对 asset 给出 actual_track、actual_quality，响应 Cache-Control: no-store。② 代理：AssetService.stream 经公共上游入场（TrafficClass::media）读取上游，秘密头只放在 secret_headers；单段 Range 与 If-Range 原样转发，200／206／416 按上游实际应答转发（上游忽略 Range 就是 200），无长度时以 chunked 转发而不编造长度，206 的区间须与客户端请求一致，否则 502；只转发强 ETag。首个 200／206 固定资源的总长、ETag 与 Last-Modified；403／404／410 视为地址失效，按原实际曲目与要求重新解析一次，曲目、品质、容器与证据都一致才换用新地址，否则使 asset 过期（410 RESOURCE_EXPIRED），从不拼接不同资源。③ 流桥接：媒体入口把上游读取放到 media_upstream.workers 大小的有界线程池，经 media_stream.bridge_buffer_bytes 的有界队列交给连接线程：客户端不读时上游读取也停下，客户端断开或响应结束即通过 stop token 取消上游；每个代理响应占用 media_limits.upstream 额度（与本地文件额度分开，共用总额与每 principal 额度），发头前无额度为 429／503；停机时先停媒体监听再等待线程池退出。④ 构建能力：media_proxy、direct_media_urls 可在配置中启用，/api/v1/info 按实际启用情况报告 |
| 验证 | `media_proxy_test.cpp`（`[media][proxy]`，真实进程＋模拟来源与 CDN）：完整 200 与 Content-Length／ETag／Accept-Ranges，bytes=100-199、-50、102000- 的 206 与 Content-Range，越界 416 带 `bytes */总长`，多段 400，过期 If-Range 得完整 200、匹配则 206，HEAD 只有头；Bearer 路由可读、无令牌 401；上游忽略 Range 时返回 200；无长度时以 chunked 返回且无 Content-Length。A31：需要 Cookie 的地址 direct 为 422、auto 选 proxy，秘密只出现在发往 CDN 的 secret_headers，不出现在访问描述、响应或普通请求头；可外发地址 auto 选 direct，响应 no-store。A16／A32：首个地址失效后同一票据透明续用新地址、内容不变、只多一次解析；新地址内容不同时返回 410 RESOURCE_EXPIRED，之后 media-access 也为 RESOURCE_EXPIRED；ttl=300 的访问描述不晚于 asset 到期。A24／A30：8 MiB 慢速资源，客户端读 256 KiB 后停读 1 秒，期间上游发送量不再增长且远少于一半；另一客户端同时读同一 asset 的另一区间不受影响；停读的客户端断开后上游被取消、媒体响应额度归零。A33：关闭代理时 asset 为 501、不发任何上游请求，文件访问照常 |
| Windows 证据 | MSVC /W4：0 条项目警告；`ctest --preset windows-msvc-debug` 316 项全部通过（5 项在线或按设计跳过）；`[proxy]` 连续 5 次通过 |
| Linux／macOS／容器证据 | 待 GitHub Actions 额度恢复后运行 CI |
| 已知限制 | 真实平台未声明可外发，direct 只在模拟来源上验证；代理在线验证（酷我、5sing 实际 CDN）尚未执行，S4 关口 T36 负责；身份撤销只拒绝新的代理请求，已在传输中的代理按传输预算或客户端断开结束，没有主动中断；代理不做缓存与转码；206 区间与请求不一致时直接 502，而不是改写区间；资产证据只比较总长与校验器，不校验内容哈希 |

### T32 S3 可靠性与资源关口：待验收（2026-10-22，Windows 通过，CI 待运行）

| 项目 | 记录 |
| --- | --- |
| 前置 | T31 待验收（Windows 通过）；T29、T30 已完成。不等待 N／X 平台 |
| 关口用例 | `s3_gate_test.cpp` 的 `[gate][t32]`，真实进程＋两个模拟来源（s3a、s3b，各自一个 CDN 源站），CDN 按曲目 id 注入故障；metadata_policy=required，三种格式都写标签。① 故障组合：18 个任务（MP3 12、FLAC 4、M4A 2，两个目标目录）在暂停时受理，再把第二目标目录删掉后恢复领取。注入：正文中途断开（MP3 40 KiB、FLAC 8 KiB）、首次 503、首次 429＋Retry-After: 1、断开后资源改变（新 ETag 与内容）、断开后续传得到错误起点的 206、FLAC 大部分音频缺失但长度自洽、永久 404；第二目标离线时其任务进入 waiting_retry，目录重建后完成。结果：16 个完成、2 个按预期失败（损坏 FLAC 为 AUDIO_INVALID，404 用尽 5 次尝试），失败任务无结果；完成文件逐一与当前资源比对：MP3 去掉 ID3v2 后逐字节一致，FLAC 自首帧起逐字节一致且时长一致，M4A 的 mdat 字节数与时长一致；资源改变的任务得到新内容而非旧前缀；两个目标目录中除这 16 个文件与工作目录外没有任何其他文件。断开的任务以非 0 起点 Range 续传并得到 206；错误 206 之后从头重下；429 的下一次请求至少晚 950 ms，其间另一来源的 CDN 仍有新请求。② 停机重启：6 个任务中 3 个（等于 workers）在传输中被停机打断，其余排队；重启后全部完成，attempt 仍为 1，被打断的 3 个以 Range 续传，内容与源一致 |
| 运行参数 | downloads.workers=3、per_source=2、max_attempts=5、transport_retries=2；retry.initial_backoff_ms=1000、multiplier=1；upstream.per_origin=2、per_principal=6、max_total_connections=6；CDN 每 4 KiB 一块、块间 1 ms；Debug 构建 |
| 实测（Windows 11，MSVC Debug，5 次） | 18 个任务耗时 9.0～12.0 s，CDN 请求 33 次、发送 1,470,355 字节；同时传输峰值 3（=workers），每个源站峰值 2（=per_origin，也等于 per_source）；进程线程数空闲 66～67、峰值 67（无按任务或按传输的线程）；进程峰值常驻内存 28～31 MiB（含测试进程本身）；负载期间控制入口 `GET /api/v1/downloads?limit=100` 每 20 ms 采样 57～86 次：p50 29～38 ms、p95 41～46 ms、最大 45～96 ms。断言：传输峰值 ≤ workers，每源站 ≤ per_origin，线程峰值 ≤ 空闲＋4，控制入口 p95 < 500 ms；指标以 `T32-METRICS` 一行 JSON 输出 |
| Windows 证据 | MSVC /W4：0 条项目警告；`ctest --preset windows-msvc-debug` 312 项全部通过（5 项在线或按设计跳过）；`[t32]` 连续 5 次通过 |
| Linux／macOS／容器证据 | 待 GitHub Actions 额度恢复后运行 CI |
| 已知限制 | 数值来自 Debug 构建与模拟 CDN，只说明上界被遵守和量级，不是吞吐或延迟承诺；Release 与目标设备的实测归 T39。per_source 与 per_origin 在本用例中取值相同且一源一站，未单独区分。真实磁盘写满与 NAS 目标离线未组合注入（写盘失败路径由 T16 用例覆盖，目标离线以删除目录模拟）；设计中没有带宽限速配置，“限速”按 429／Retry-After 与来源冷却验证；内存为进程整体峰值，未单独计量服务部分 |

### T31 故障恢复、清理与迁移强化：待验收（2026-10-22，Windows 通过，CI 待运行）

| 项目 | 记录 |
| --- | --- |
| 前置 | T29、T30 已完成。侧文件一致性依赖尚未开始的 T30.s，本任务不覆盖 |
| 交付 | ① 清理：`downloads::Maintenance` 在启动恢复之后执行一次，之后由服务循环每小时执行。工作目录按其任务的当前状态判定，而不是按时间或扩展名：任务可继续运行或仍有发布意图的保留；失败任务按 `filesystem.failed_work_retention_hours`（默认 24 小时，自 finished_at 起）保留后删除；已完成、已取消与库中不存在的任务（例如恢复了较旧的数据库）删除；名字不是 tsk_ 的条目不列出也不删除；目标离线则跳过、下次再查；删除失败只报告、下次重试。先列目录再读库，因此并发新建的目录不会被误判为孤立。过期幂等记录按批（每次至多 1000 条）删除。新端口 `MaintenanceStore`（SQLite）与 `WorkspaceDirectories`（本地文件系统）。② A46 复用证据：已完成结果的复用除文件存在外还要求磁盘大小等于资产记录的大小，被改写或截断的文件不再返回 reused=true，而是新建任务（发布时不覆盖原文件）；复用候选与提交之间的资产版本变化仍由事务内条件复核拒绝。③ 备份与恢复：`backup --output FILE` 用 SQLite 在线备份生成一致快照（服务运行时可执行，不取实例锁），写成单个独立文件，经 integrity_check 与迁移校验后改名，不覆盖已有文件；`restore --input FILE` 取实例锁，先校验备份（完整性、不比本程序新、迁移校验和一致），把当前数据库一致复制到 `backups/replaced-<UTC>.sqlite3` 后替换并删除旧 -wal／-shm。打开已有数据库且有待执行迁移时先自动备份到 `backups/pre-migration-v<版本>-<UTC>.sqlite3`。data_dir 新增仅所有者可访问的 `backups/` |
| 迁移失败恢复路径 | 每个迁移在独立事务中执行；注入失败的第 3 号迁移（先加列再写不存在的表）后，数据库仍为第 2 版、新列不存在、原数据保留，本程序照常打开；迁移前备份可用 restore 换回 |
| 验证 | `downloads_test.cpp` 的 `[maintenance]`：六种任务状态各有工作目录，1 小时后已完成、已取消与孤立目录删除，排队、待发布与 24 小时内失败的保留，非 tsk_ 目录与已发布文件不动，离线目标被跳过；25 小时后失败任务目录删除，5 条过期幂等记录被清除且同一 key 可再次受理；第三次运行不再删除。`publication_test.cpp`：服务启动即删除孤立工作目录并记录日志，排队任务的目录保留。`completed results are reused only while they still hold` 增加文件被改写为其他大小时不复用。`backup_test.cpp`：服务运行中 backup 成功、restore 因 INSTANCE_LOCKED 拒绝；同名输出为 CONFLICT；restore 后回到备份时的状态，被替换的数据库可再次 restore 撤销，恢复后的数据库可正常启动服务；损坏、缺失、schema 更新与校验和改动的备份均被拒绝且数据库不变；第 1 版数据库启动时生成 pre-migration-v1 备份且只生成一次；失败迁移回滚 |
| 已覆盖的既有故障用例 | A12（取消／发布竞争）、A17（写盘失败）、A20（移动后退出）、A21（提交后通知前退出）、A22（旧执行代次）、A27（停机检查点）沿用 T16～T18 的故障注入用例，本任务未改变这些路径，在同一次完整测试中通过 |
| Windows 证据 | MSVC /W4：0 条项目警告；`ctest --preset windows-msvc-debug` 310 项全部通过（5 项在线或按设计跳过）；新增用例连续 5 次通过 |
| Linux／macOS／容器证据 | 待 GitHub Actions 额度恢复后运行 CI |
| 已知限制 | 侧文件一致性归 T30.s；磁盘耗尽只验证到写盘错误路径（不可重试失败、不标完成），真实磁盘写满与 NAS 目标离线的组合留给 T32／T39；清理间隔固定为 1 小时，不另设配置键；备份不含 `secrets/`，restore 不校验凭证秘密文件是否齐全（命令输出提示一并恢复）；不提供数据库自动降级 |

### T28 S2 基础扩展关口：待验收（2026-10-22，Windows 通过，CI 待运行）

| 项目 | 记录 |
| --- | --- |
| 前置 | T23 已完成（CI 39710009819）；T41 已完成（酷我＋5sing）。T24～T27 未启用，不在本关口范围 |
| 实现改动 | 下载执行记录解析音源的实现（`implementation_id@implementation_version`）：写入执行快照（execution.implementation）与续传检查点；检查点的实现与本次解析不同（含早期构建未记录实现的检查点）时不续接旧字节，按稳定 TrackRef 重新解析后从头下载。私有上下文（context_ref）仍未使用，任务只持久化稳定引用，因此不存在需要迁移的旧上下文 |
| 关口用例 | `s2_gate_test.cpp` 的 `[gate][t28]`，真实进程＋真实酷我／5sing 包，上游为录制样本：① 规模：酷我、5sing 与 100 个模拟来源（每 4 个含音源，搜索经公共上游访问各自主机）共 102 个来源；启动与分页列出全部来源（50／50／2，按 source 排序且不重复；resolve_audio 筛选 27 个；limit=101 为 400；换筛选的游标为 CURSOR_MISMATCH）不创建任何能力对象、不发请求；默认搜索只创建并访问 default_sources 的 3 个来源；sources=all（101 个可搜索来源超出每请求 8 个）在触达来源前 400；按 8 个一批搜索只访问该批；进程线程数在 102 个来源与仅两个真实来源时相同（允许系统线程池 4 个波动）。② 声明一致：两源对 18 个按来源路由的集合／发现／歌词／详情／搜索操作逐一调用，已声明的均进入平台，未声明的均为 CAPABILITY_UNSUPPORTED（搜索为逐源 unsupported）且不发上游请求；声明的每个能力都有对应路由；resolve_audio 与声明一致。③ 多源部分失败（A05 复验）：酷我搜索 503 时整体 200、partial=true，酷我 failed 且 retryable，结果只有 5sing；失败不缓存，恢复后两源均 ok。④ 平台禁用：先发布 5sing 文件，再在暂停下载时受理 5sing 与酷我任务，配置 `providers.fivesing.enabled=false` 后重启：5sing 任务终态 failed／PROVIDER_DISABLED（不可重试，original_track 仍为 5sing，无结果），酷我任务照常完成，期间不向 5sing 或其 CDN 发任何请求；已发布文件的元数据与票据读取逐字节一致；providers 列表显示停用，resolve、歌词、歌单为 PROVIDER_DISABLED，多源搜索中 5sing 为 disabled，新下载（force）为 422 PROVIDER_DISABLED。⑤ 实现升级：酷我传输在首块后因停机中断，下次启动换为新版本酷我实现：任务按 TrackRef 重新取得音源地址，从首字节下载（无 Range），文件与上游一致；旧实现发出的歌单游标为 CURSOR_EXPIRED。去掉执行器改动时 ⑤ 失败（续传了旧字节），改动恢复后通过 |
| 不改通用业务 | `git diff 5e68248..8a7b260 -- src`（T23 为两个已有平台补集合能力）只有 `src/providers/` 下的改动；T28 本身只改下载执行器的检查点规则，未增加平台分支 |
| Windows 证据 | MSVC /W4：0 条项目警告；`ctest --preset windows-msvc-debug` 304 项全部通过（5 项在线或按设计跳过）；`[t28]` 连续 15 次通过 |
| Linux／macOS／容器证据 | 待 CI 运行；通过后补记运行号并改为已完成 |
| 已知限制 | 线程数比较依赖本机线程枚举，容许 4 个系统线程波动；context_ref 与 context_schema_version 在出现需要私有上下文的来源时实现；授权与 YouTube（T24、T27）启用时另行验收 |

### T23 已接平台的集合功能落地：已完成（2026-10-22，提交 8a7b260）

| 项目 | 记录 |
| --- | --- |
| 交付 | 酷我（kuwo-native 新增 12 项能力，全部 HTTPS）：search_playlists、search_albums（`search.kuwo.cn/r.s` ft=playlist／album，单引号伪 JSON 由 `legacy_json` 转换）、playlist_detail／playlist_tracks（`nplserver.kuwo.cn/pl.svc`）、album_detail／album_tracks（`r.s` stype=albuminfo）、chart_detail／chart_tracks（`kbangserver.kuwo.cn/ksong.s`，encode=utf8）、charts（`wapi.kuwo.cn/api/pc/bang/list` 树的叶子，id 即 ksong.s 的榜单号）、categories（getTagList 中 digest 10000 的歌单标签，专区类条目不列）、category_playlists（getTagPlayList）、recommend_playlists（getRcmPlayList order=hot）。5sing（新增 3 项）：search_playlists（`home/json` type=1，每页固定 15 条，游标 `页码[:偏移]`）、playlist_detail（`song/getsonglist`）、playlist_tracks（`song/getsonglistsong`，上游忽略分页、一次返回整张歌单，游标为偏移量），3 个明文端点按 operation 登记。酷我 HTTP 客户端移入 `client.hpp`，搜索与集合共用 |
| 规则落实 | 每次调用只请求一个上游页（5sing 歌单曲目除外：上游只给整表），不在后台枚举整张歌单；track_count 是平台声明，与当前页大小无关；酷我对未知歌单 pid 返回另一个空歌单，按 id 不符判 NOT_FOUND；未知专辑（无 albumid）与未知榜单（`{}`）为 NOT_FOUND；未知分类返回空页（与空分类无法区分）；非数字的分类或集合 id、其他来源的 id、非法游标与超限 limit 不发请求；wapi 的 code≠200 为带 kuwo_code 的失败；5sing code 13（操作太频繁）为可重试的 UPSTREAM_RATE_LIMITED，code 24 或 success=false 的歌单为 NOT_FOUND；5sing 网页歌单会截断长标题，因此只用 API。所有集合走 T21 的公共路由与游标（换 principal 为 CURSOR_EXPIRED），通用层无改动 |
| 协议准备 | 2026-10-22 自中国大陆网络匿名实测，上表端点均返回 UTF-8（ksong.s 需 encode=utf8，否则为 GBK）；样本见 `tests/fixtures/providers/kuwo` 与 `tests/fixtures/providers/fivesing`（歌单曲目只保留前 5 首，请求标识置零） |
| 离线契约 | `kuwo_collections_test.cpp`：伪 JSON 转换（引号、`\'`、未闭合）、歌单／专辑搜索映射与页码校验、同名不同专辑、歌单／专辑／榜单页（含专辑曲目的数字 albumId、榜单用 song_duration）、三种不存在、分类（67 个歌单标签）、分类与推荐歌单、榜单树（36 个）、能力对象的 URL／页码／游标／1 起页的 wapi、非法输入不发请求、只走 HTTPS、503 可重试、声明的能力；`fivesing_test.cpp`：歌单搜索（去高亮、空名、无简介）、歌单详情（占位简介、不存在、id 不符）、歌单曲目（完整标题、类型、不存在、code 13）、歌单 id 校验、游标页内续读与换页、偏移游标、非法输入不发请求；`fivesing_gate_test.cpp`：酷我歌单曲目经公共路由分页、他人游标 CURSOR_EXPIRED、榜单列表 catalog_kind=live、榜单详情、5sing 榜单 422 |
| 在线验证 | Windows（2026-10-22，中国大陆网络）：`[online-kuwo]` 歌单搜索“周杰伦”取首个歌单前 10／177 首、专辑“叶惠美”11 首、榜单 36 个且热歌榜第 2 页 10 首、分类 67 个且首个分类有歌单、推荐歌单 5 个；`[online-fivesing]` 歌单搜索“古风”、详情“古风 by 云知月”、曲目 153 首分两页读取且不重复 |
| Windows 证据 | MSVC 19.51 /W4：0 条项目警告；`ctest --preset windows-msvc-debug` 300 项全部通过；GCC 16.2 -O2 严格警告检查改动的平台包源码与测试 0 条项目警告 |
| Linux／macOS／容器证据 | CI 运行 39710009819：linux-gcc、macos-clang、windows-msvc 与 docker 四个作业全部通过 |
| 已知限制 | 酷我歌词未实现（newlyric 接口需 zlib 解压与 GB18030 解码，需新增依赖，另立任务）；两平台均未实现歌手（search_artists、artist_tracks、artist_albums）；5sing 没有专辑、榜单、分类与推荐（参考实现也未提供）；歌单与专辑链接解析未加入（parse_links 仍只识别单曲链接）；5sing 歌单曲目每次调用都取整表，超大歌单受上游响应大小上限约束；封面只透传公开地址 |

### T41 第二个真实平台扩展关口：已完成（2026-10-22，提交 8bf3bd4；选用 T22 5sing）

| 项目 | 记录 |
| --- | --- |
| 选用平台 | 酷我（T12／T20）与 5sing（T22），均为原生、匿名来源 |
| 不改公共层 | `git diff 48b3f6d..371646e -- src` 除 `src/providers/`（新增 `fivesing/`，默认平台清单加入 fivesing）外没有改动：路由、CatalogService、下载状态机与执行器、存储表、配置与传输均未为 5sing 增加分支；5sing 只通过 bundle 声明能力、品质提示与明文端点 |
| 关口用例 | `fivesing_gate_test.cpp` 的 `[gate][t41]`：真实进程同时载入两个真实协议包，两个 principal 各有自己的下载目录。注册：providers 列表含两源，`capability=resolve_audio` 两源、`capability=lyrics` 只有 5sing、`capability=search_playlists` 为空，执行方式均为 native、无授权方法，品质提示分别为 [standard] 与 [standard, high]。能力差异：酷我歌词 422 CAPABILITY_UNSUPPORTED、5sing 200；酷我 high 为 422 QUALITY_UNAVAILABLE，5sing high＋allow_lower 降为 standard；一个来源的 id 在另一来源为 400。身份隔离：A 的两源合并搜索第二次两源均命中缓存，B 的同一搜索两源都不命中并再次请求上游；A 同时下载酷我与 5sing、B 下载 5sing，三项并发完成，A 的 SSE 只有 A 的两项、B 的只有 B 的一项；两源任务经过完全相同的状态序列；文件分别落在各自目标目录，载荷与上游一致且标签完整；对方的任务与文件为 404，任务列表互不可见。下载复用：A 用新 key 再次提交两源均复用已完成文件、不再请求 CDN；B 的同一曲目单独下载（不跨 principal 复用） |
| 真实来源证据 | 2026-10-22 Windows（中国大陆网络）：`kuwo_e2e.py`（酷我，3.6 s）与 `kuwo_e2e.py --source fivesing`（5sing，3.0 s）的完整在线闭环均通过，两者走同一控制入口、同一下载执行器与同一文件访问；T22 CI 见 T22 记录 |
| Windows 证据 | MSVC 19.51 /W4：0 条项目警告；`ctest --preset windows-msvc-debug` 291 项全部通过；`[gate]` 连续 5 次通过 |
| Linux／macOS／容器证据 | CI 运行 39705296535：Linux GCC 13、macOS Clang、Windows MSVC 各 291／291 通过（含 `[t41]`），Linux 构建 0 条项目警告（T22 记录中 flac.cpp 的警告已消除）；容器构建与冒烟通过；5sing 在线闭环在三个运行器与容器内再次全部通过，酷我在线闭环仍为 code 407（T20 的既有缺口） |
| 已知限制 | 两个来源都是匿名来源，账号隔离验证的是 principal 与匿名授权身份；带凭证账号的多账号隔离在第一个需要凭证的平台（如 T24 及对应平台任务）复验。集合能力的真实落地归 T23 |

### T22 5sing 原生平台接入：已完成（2026-10-22，提交 371646e）

| 项目 | 记录 |
| --- | --- |
| 交付 | 平台包 `providers/fivesing`（fivesing-native 0.1.0，匿名）：search_tracks、track_detail、parse_links、lyrics、resolve_audio，品质提示 standard、high。作品身份为“类型＋编号”复合 id `<yc|fc|bz>-<编号>`，类型写入 version（fc＝cover、bz＝instrumental、yc 只标“原创”）；上传演唱者是 artist，原唱者不写入。两个 API 主机只有 HTTP（HTTPS 证书不匹配），bundle 精确登记 4 个明文端点（主机、80 端口、路径前缀、operation），请求无秘密；音频只接受 HTTPS `*.kugou.com`。取流：standard＝lq 128 kbit/s（缺 lq 用 hq 192），high＝sq 320 kbit/s，无 lossless；primary 先做 HEAD，backup 只在文件名等于该档 `<md5>.<ext>` 时作为同一文件使用；HEAD 长度等于声明大小时为 full，否则 unknown；时长始终未知。搜索每次调用一个上游页（10 条），游标 `页码[:偏移]`。构建默认 `MUSIC_SERVICE_PROVIDERS` 改为 `kuwo;fivesing`，5sing 的测试只在选入时编译；`packaging/e2e/kuwo_e2e.py` 增加 `--source fivesing`，`packaging/docker/e2e.sh` 透传参数，CI 各平台与容器各加一个允许失败的 5sing 在线步骤 |
| 协议准备（.1） | 2026-10-22 自中国大陆网络匿名实测：`search.5sing.kugou.com/home/json`（固定 10 条／页，`list` 为 null 表示空）、`song/newget`（不存在为 code 200062）、`song/getSongUrl`（不存在、类型不符或未知类型均为 code 28）；以 Range 读取帧头确认 sq／hq／lq 为 320／192／128 kbit/s MP3；CDN 主备返回相同长度与 Last-Modified、无 ETag，地址带时间与签名段、无明确过期时间。`song/newget` 与 `getSongUrl` 对 hq／lq 的大小字段互相颠倒，只采用 getSongUrl 并以 HEAD 长度核对。脱敏样本见 `tests/fixtures/providers/fivesing` |
| 离线契约（.3） | `fivesing_test.cpp`：复合 id 与非法形式；搜索映射（去高亮、类型到 version、无时长、未知类型条目略过、空结果）；详情与歌词只接受同一编号与类型、未计时文本不当 LRC；三档主备地址；链接；明文端点登记与传输规则（换 operation、换路径、带 Cookie、明文音频均被拒）；注册表接受 bundle；搜索游标页内续读、换页、页码不符、非法游标不发请求、429／502／入场受限；取流 standard／high、primary 无应答改 backup、primary 长度不符改 backup、backup 指向其他文件时不尝试、全部失败报错、入场受限不试 backup、非 HTTPS 或非 kugou.com 地址拒绝、按作品已有档位选择品质、长度未知为 unknown、不存在的作品。`fivesing_gate_test.cpp`（`[gate]`）：真实进程同时载入酷我与 5sing，providers 列表、两源合并搜索各自 ok、5sing 歌词 200 而酷我 422 CAPABILITY_UNSUPPORTED、无类型的 id 为 400、无 sq 的作品请求 high 为 422、公共下载执行器生成带标签的“琴酒蜀黍 - 千年食谱颂.mp3”，primary 503 时同一文件从 backup 下载 |
| 在线验证（.4／.5） | Windows（2026-10-22，中国大陆网络）：`[online-fivesing]` 搜索“千年”10／16777 条，10 首均解析为 full，7 首可取 high（320 kbit/s）；`kuwo_e2e.py --source fivesing` 全程通过（3.0 s）：搜索 → 详情 → 解析（standard、full、128000 bit/s）→ 持久任务 → SSE 版本前进到 completed → 文件“琴酒蜀黍 - 千年食谱颂.mp3”5,325,906 字节 → 票据 Range 与完整读取与发布文件一致 → 客户端保存中途失败任务仍 completed → 取消 202／canceled → 重启后任务、文件、幂等重放与复用均正确；同一脚本的酷我闭环仍通过（3.6 s） |
| Windows 证据（2026-10-22） | MSVC 19.51 /W4：0 条项目警告；`ctest --preset windows-msvc-debug` 290 项全部通过（4 项在线或按设计跳过）；GCC 16.2 -O2 严格警告检查平台包源码与单元测试 0 条项目警告 |
| Linux／macOS／容器证据 | CI 运行 39703712889：Linux GCC 13、macOS Clang、Windows MSVC 各 290／290 通过（在线用例按设计跳过），容器构建与冒烟通过；5sing 在线闭环在 Linux、macOS、Windows 运行器与容器内全部通过（搜索、解析、持久下载到完成、票据读取、取消与重启），可见 5sing 从海外网络可用；同一运行中酷我在线闭环仍因音源接口 code 407 失败，与 T20 记录一致。Linux 构建有 1 条项目警告（T30 的 flac.cpp 中 uint64_t 与 unsigned long long 混用），已在 T41 提交中修正 |
| 已知限制 | 歌单、歌手、登录与付费内容未实现；搜索每次只取一个上游页，limit 大于 10 时也最多返回 10 条；音频地址无过期时间，过期后由公共执行器重新解析；hq 只在缺 lq 时作为 standard 使用，不单独提供 192 kbit/s 档；链接只识别桌面页面形式；T41 关口另见其记录 |

### T21 多源、集合、发现与歌词用例：已完成（2026-10-22，提交 b27d061）

| 项目 | 记录 |
| --- | --- |
| 交付 | 领域 `domain/catalog/collections`：Playlist、Album、Artist、Category、Chart（ListedKind 表示列出的是歌单／曲目／专辑／歌手）、TrackLyrics（lrc、translated_lrc、毫秒单位的逐行逐词 karaoke、unavailable、warnings）。端口：按 data-models 能力键新增 15 个能力（search_playlists／albums／artists，playlist／album／chart_detail，playlist／album／artist／chart_tracks，artist_albums，category_playlists，categories，charts，recommend_playlists），以 ICatalogSearch／IEntityDetail／IEntityList 三种形状加标签类型逐键绑定，另有 ICategories、ICharts、IRecommendedPlaylists；ILyrics 改为返回 TrackLyrics；ProviderDirectory 增加按接口类型取能力对象的 `capability<Port>()`（注册表按键与端口类型校验）。CatalogService：曲目搜索改为按条目类型的模板，歌单／专辑／歌手搜索复用同一套扇出、逐源结果、游标与失败判定，游标另绑定搜索类型；新文件 catalog_entities 实现详情、实体列表、分类／榜单目录、推荐与歌词，统一经账号绑定、租约、按 AuthIdentityKey 与实现版本分区的缓存（与搜索共用一个有字节上限的缓存）和在途合并；列表游标绑定 owner、能力键、来源、实体、limit、账号身份、选择世代与实现版本。控制入口新增 `GET /api/v1/search/{playlists,albums,artists}`、`/playlists/{detail,tracks}`、`/albums/{detail,tracks}`、`/artists/{tracks,albums}`、`/categories`、`/categories/playlists`、`/charts`、`/charts/{detail,tracks}`、`/recommendations/playlists`、`/lyrics`，均需 catalog:read |
| 规则落实 | 按能力路由：没有该能力为 422 CAPABILITY_UNSUPPORTED，来源停用为 PROVIDER_DISABLED，未知来源为 UNKNOWN_PROVIDER；多源搜索中分别标为 unsupported、disabled 与 failed。列表只取请求的那一页，不在后台枚举整张歌单；集合的 track_count 是平台声明，与当前页大小无关。列表 limit：曲目类 1～100（缺省 50），artist_albums、category_playlists、recommendations 1～50（缺省 20）。游标换列表、实体或 limit 为 CURSOR_MISMATCH，换 owner、账号、实现或未知为 CURSOR_EXPIRED。失败不缓存。分类与榜单目录返回能力声明的 catalog_kind（live／built_in／search_derived）。歌词：一首歌没有歌词为 200、lrc 为 null、unavailable=true；平台不支持歌词为 422。目录型来源（无 resolve_audio）的集合与歌词照常可用 |
| 覆盖的验收 | A05（四个来源：一个正常、一个 3 秒慢源在 300 ms 单源预算内被判 UPSTREAM_TIMEOUT、一个不支持、一个停用，整体约在预算内返回 partial，只有正常来源的条目）；单源超时保留其他结果；来源数与并发沿用 T13 的上限与扇出；unsupported／disabled 明确区分；目录型替身无音源仍可用；分页不串源（播放列表游标不能继续曲目或专辑搜索，列表游标不能用于另一实体、另一列表或另一 limit）；缓存命中不再调用平台；HTTP 层的条目形状、page 对象、422／404／400、未知参数与 401 |
| Windows 证据（2026-10-22） | MSVC 19.51 /W4：0 条项目警告；`ctest --preset windows-msvc-debug` 278 项中 275 通过、3 项在线用例按设计跳过；`[collections]`、`[catalog]` 连续 5 次通过；GCC 16.2 在 -O2 严格警告下检查改动的源码与测试 0 条项目警告 |
| Linux／macOS 与容器证据（CI 运行 39642143812） | ubuntu-24.04，GCC 13.3.0：278/278 通过；Windows（windows-2025）与 macOS（macos-15）各 278/278；容器构建与冒烟通过；项目警告均为 0 |
| 已知限制 | 尚无真实平台实现这些能力（酷我只有曲目搜索、详情、链接与音源），实际能力由 T23 与各平台任务交付，本任务只用模拟来源验证公共边界；短链展开未实现（ILinkParser 仍只做离线识别，需要网络的短链展开另加能力并限制在平台声明的域名内）；context_ref 参数被接受但未使用；歌单／专辑／歌手的单源 page 跳页与搜索一样不支持；封面只透传公开 URL，需要秘密头的封面没有图片代理；歌词与封面尚未接入 T30 的标签写入 |

### T30 容器、标签与解密处理扩展：已完成（2026-10-22，提交 cc3dbf4；侧文件为子任务 T30.s，未开始）

| 项目 | 记录 |
| --- | --- |
| 交付 | 固定依赖 TagLib v2.3.2（`deadc299…`，子模块 utfcpp v4.2.0；静态库，只启用 MPEG、MP4、Vorbis／FLAC；无 zlib、绑定、示例和测试；utfcpp 只取固定子模块；MSVC 以 /utf-8 编译；依赖测试核对版本）。`audio_processing`：`format/flac`（STREAMINFO、元数据块链、首帧同步、按最小帧长判截断）、`format/mp4`（顶层盒铺满文件、ftyp／moov／mdat、soun 轨、mvhd 时长、stsd 编码）、`tags/tag_writer`（TagLib 写入并逐项读回）。处理器升级为 `audio.verify` v2（ProcessingPlan 缺省版本 2，并可声明解密步骤）：验证 MP3／FLAC／MP4，标签写入输入旁的副本 `tagged.<ext>` 并作为产物。端口：ProcessingRequest 增加 TrackMetadata（标题、歌手列表、专辑、歌词、封面），新增 AudioDecryptor 端口（无内置实现），警告 METADATA_PARTIAL，错误码 METADATA_FAILED（422）。Runner 在需要标签时总是读取平台详情（续传时不再因已有文件名而跳过），把元数据交给处理器，并发布处理器报告的产物而非固定的工作文件；Finalizer 的 metadata_status 增加 partial |
| 规则落实 | 收到的工作文件只读，重处理与崩溃恢复从同一字节开始；副本写入后重新测量，帧数／样本数、音频字节和时长与原文件不同即丢弃（载荷保真）。MP3 只写 ID3v2.4，已有 ID3v1、APE 与自定义帧保持原样；只改给出的项，其他字段保留；对已加标签的文件再次处理得到逐字节相同的结果。skip 不写；best_effort 无可信元数据或写入失败时发布未加标签的文件并附 METADATA_NOT_WRITTEN（metadata_status=failed），部分项写不进附 METADATA_PARTIAL（partial）；required 无元数据、写入失败或部分项写不进均为 METADATA_FAILED，不发布，详情读取失败按其错误重试或失败。处理计划声明的解密没有对应处理器时目录查找返回 CAPABILITY_UNSUPPORTED，从不跳过 |
| 覆盖的验收 | A18（可选标签失败：副本无法创建时 best_effort 发布已验证的原文件、阶段 failed、警告一致；required 返回 METADATA_FAILED）；A19 的真实文件部分（FLAC 元数据被截断、FLAC 音频大部分缺失、M4A 末尾缺 100 字节均为可重试的 AUDIO_INVALID truncated，不发布；声明解密而无处理器不发布）；A35 的标签部分（required 不接受没有满足标签要求的结果：无元数据即失败）。MP3／FLAC／M4A 三种样本写入标题、两位歌手、专辑、歌词与 PNG 封面后用 TagLib 读回一致；FLAC 实测 8000 Hz、单声道、24000 样本、3000 ms，M4A 为 aac、约 3000 ms；关口用例中酷我发布文件的 metadata_status 为 complete，去掉 ID3v2 后与 CDN 字节一致 |
| 真实样本（2026-10-22，Windows，匿名） | `kuwo_e2e.py` 在线闭环通过（3.3 秒）：rid 78932517 发布为 2,830,362 字节（原 2,829,338 字节加 ID3v2.4 标签），ffprobe 读到 title=晴天、artist=蓝心羽，时长不变（176.77 秒）；该曲详情没有专辑，因此未写专辑 |
| Windows 证据（2026-10-22） | MSVC 19.51 /W4：0 条项目警告；`ctest --preset windows-msvc-debug` 274 项中 271 通过、3 项在线用例按设计跳过；GCC 16.2 在 -O2 严格警告下检查新增与改动的源码和测试（TagLib 作系统头）0 条项目警告 |
| Linux／macOS 与容器证据（CI 运行 39636986679） | ubuntu-24.04，GCC 13.3.0：274/274 通过，TagLib 与 utfcpp 按固定提交拉取并通过校验；Windows（windows-2025）、macOS（macos-15，LLVM 20）各 274/274；容器镜像构建与冒烟通过；项目警告均为 0 |
| 已知限制 | 侧文件（.lrc 歌词、封面图片）未实现：需要 Finalizer 发布多个产物并纳入恢复集合，留作子任务 T30.s，与 T31 的侧文件一致性一起做；歌词与封面目前没有来源（平台能力归 T21／T23），只在样本中验证写入；FLAC 不逐帧遍历，截断只能由传输总长、STREAMINFO 最小帧长与时长发现；MP4 不检查 stco 偏移是否落在 mdat 内；Ogg、WAV、ADTS 仍为 unsupported_container；没有解密实现（N08）；发布包体积与 TagLib 许可文件随包分发在 T37／T40 落实 |

### T29 续传、重试与速率策略增强：已完成（2026-10-21，提交 d057b74）

| 项目 | 记录 |
| --- | --- |
| 交付 | `application/transport` 公开 Retry-After 解析（parse_retry_after／retry_after，仅 delta-seconds，HTTP-date 不信任，超大值按一年）。DownloadRunner：429／503 的 Retry-After 写入错误 details（retry_after_seconds），retry_later 读取错误携带的等待（含平台包在解析阶段给出的同一字段），下次尝试时间取指数退避与有上限的 Retry-After 中较晚者；上限 RunnerOptions.max_retry_after，ProcessHost 取与入场账本冷却相同的上限（300 秒）。416 不再直接失败：只有 Content-Range `bytes */N` 的 N 等于已有字节、等于检查点记录的总长、ETag（若返回）一致且有强校验器时才视为传输完成并交给验证，否则按资源变化丢弃前缀从零重下（尝试内有限次，耗尽后 upstream_retry） |
| 规则落实 | 200 不追加（T17 已有）；206 的起点、总长或校验器不符即从零重下，不混合两个资源；416 不仅凭状态码判完成；403／404／410 结束本次尝试并在下次重新解析，检查点按曲目、品质与校验器保留，同一资源以 Range＋If-Range 续传、不同则 200 覆盖；Retry-After 只推迟、不缩短，仍受尝试数与绝对截止约束；来源冷却仍由入场账本按配额组计时，调度器跳过受限来源（T17 的 A43） |
| 覆盖的验收 | A13（Range 收到 200 覆盖部分文件，T17 用例保留）；A14（206 起点不符、ETag 变化两种情况都从零重下，发布文件只含一个资源的字节）；A15（416 的总长与已有字节不一致时不完成，第三个请求不带 Range，文件完整，attempt 仍为 1）；A16（签名地址 403 后 waiting_retry、resume_stage=resolving，第二次解析得到新地址并以 Range／If-Range 续传，attempt=2）；Retry-After 30 秒使下次尝试至少推迟 30 秒，86400 秒按上限 120 秒（测试配置），attempt 递增。A22、A25、A26、A43、A44 由 T17 用例继续覆盖 |
| 顺带修正 | CI 中早已存在、与 T20／T29 无关的三条项目警告：macOS clang 的未使用常量 create_route（task_repositories.cpp）与 server_test 重复的 json 别名（-Wshadow），容器 Release 构建中 GCC 13 对 parsers.cpp 可选值比较的 maybe-uninitialized（改为 value_or 读取；本机 GCC 16 不复现，以 CI 为准） |
| Windows 证据（2026-10-21） | MSVC 19.51 /W4：0 条项目警告；`ctest --preset windows-msvc-debug` 268 项中 265 通过、3 项在线用例按设计跳过；`[runner]`、`[recovery]` 连续 5 次通过；GCC 16.2 在 -O2 严格警告下检查改动的源码与测试 0 条项目警告 |
| Linux 证据（CI 运行 39627678666） | ubuntu-24.04，GCC 13.3.0：268/268 通过，macOS 268/268，容器冒烟通过，三者的项目警告均为 0（上一节的三条已消除）。同一运行中 Windows 有 1 项失败，与 T29 无关：T09 的截止用例里，被截止时间截短的读超时在 Windows 上比截止早一个计时器刻度触发，httplib 把它报告为 Read 错误，传输据此归为 connection_lost。之后的提交在截短的超时于截止前 50 ms 内触发时报告 deadline_exceeded，截止前很早的连接重置仍是 connection_lost；本机 `[transport]` 连续 8 次通过 |
| 已知限制 | 设计中没有带宽限速配置，T29 的“速率策略”只落实为来源冷却与 Retry-After，不新增配置键；Retry-After 的 HTTP-date 形式被忽略；416 的“恰好完成”分支在当前续传规则下很少出现（已知总长等于偏移时不会发出 Range），用例覆盖的是不一致时重下；退避等待中的取消沿用 T17 的停止路径，没有新增专门用例；长期重试跨重启的预算由 T17 的 A26 用例覆盖，跨天级重试与故障组合归 T31／T32 |

### T20 S1 酷我端到端关口：受阻（2026-10-21，提交 57fe5c0、033026e、72d8054；仅容器在线下载闭环未通过）

| 项目 | 记录 |
| --- | --- |
| 交付 | 下载闭环的出口默认启用：media 与 events 的缺省值跟随构建（构建含媒体与 SSE 模块即启用，监听 17633／17634），features.file_access 未设置时跟随 media.enabled；容器配置让三个监听都在 0.0.0.0 并给出同号 public_urls，镜像 EXPOSE 17632～17634。HostOptions 增加 transport，关口测试据此把真实酷我协议包接到录制应答上。修正 serve 在 stdout 输出的 ready 行把 media_base_url 固定写成 null（T19 遗留，日志中的 startup.ready 正确）。`tests/integration/kuwo_gate_test.cpp`（`[gate]`）：真实 music-service 进程＋真实酷我包，上游为 T12 样本（搜索、歌曲页、mobi.s、缺失歌曲）与按强 ETag／单段 Range 应答的 CDN（合成 176 秒 MP3，可在首块后挂起）；SSE 客户端移入共享的 `sse_client.hpp`。`packaging/e2e/kuwo_e2e.py`：仅用 Python 标准库的在线闭环，自带进程或附着到已运行服务；`packaging/docker/e2e.sh` 用它验证容器。CI 的 Windows、Linux、macOS 与容器任务各增加一步在线闭环（允许失败） |
| 闭环（离线关口，三项用例） | ① 订阅 SSE 收到 stream.ready → 搜索“晴天”3 条含 rid 78932517 → 详情（不存在的 rid 为 404）→ resolve 为 standard／mp3／128000 bit/s／full → 创建连接关闭后任务在后台执行 → SSE 只按版本前进直到 completed → HTTP 快照含 actual_track、actual_quality、file_id 与 links.file，attempt=1，工作目录已清理，发布文件「歌手 - 晴天.mp3」与 CDN 字节一致 → 文件元数据与任务响应不含目标根路径或工作目录名 → 票据 Range 206 与 Content-Range → 客户端读取 64 KiB 后断开（A29），任务仍 completed、媒体响应额度归零、随后完整读取一致 → 上游请求全部为 HTTPS、只到 www／mobi／kw-lv.kuwo.cn、无秘密头，CDN 只请求一次 → 停止后离线新建另一 principal → 重启为新 instance_id，任务与文件仍在，旧票据 404、新票据可读，他人的任务、文件与列表均不可见 → 首个 key 重放 202 且 Idempotency-Replayed、新 key 200 reused、未再次下载。② 传输挂起时取消：202 canceling → canceled，SSE 依次出现 canceling 与 canceled，无工作目录、目标根内无任何文件；下一任务传输中停止服务（soft 1 秒）退出码 0，stdout 的 ready 行列出三个地址；重启后以 Range（非 0 起点）＋If-Range 强 ETag 续传，attempt 仍为 1，经票据读回完整文件。③ 关闭 SSE：info 声明 progress_sse=false、progress_polling=true，仅用 HTTP 创建、轮询、列表与取文件 |
| 在线闭环（2026-10-21，Windows，中国大陆网络，匿名） | 调试构建 `python -I packaging/e2e/kuwo_e2e.py --binary …music-service.exe`，2.9 秒全部通过：搜索“晴天”10 条；详情与 resolve 为 standard、full、mp3、128000 bit/s；202 创建，SSE 依次 queued → resolving → downloading → processing → finalizing → completed，版本单调；文件 2,829,338 字节、audio/mpeg、ETag "sha256-23b445c0…"，票据 Range 206、完整读取与发布文件 SHA-256 一致（23b445c0…6a9e89）；客户端保存中途断开后任务仍 completed；force 第二次下载在 downloading 时取消，202 → canceled、无结果；进程被强制结束后重启为新实例，任务、文件、重放与复用均正确，旧票据 404 |
| 覆盖的验收（T20 复验） | A01（重启后同 key 重放首个结果）；A04（创建连接关闭后完成）；A06／A23（另一 principal 的任务、文件与列表 404／为空，响应无路径）；A09～A11（SSE 版本只前进，终态以 HTTP 取得）；A12（传输中取消得到 canceled 且不发布；在线竞争得到一致的单一结果）；A21（重启后新实例，持久状态经 HTTP 同步，无重复任务）；A24 本地文件（票据 Range、断开后额度归还）；A27（停机保留检查点，重启以校验器续传且不计尝试）；A28（无 SSE 的完整操作）；A29（客户端保存失败与后端任务分离）；A32 票据部分（票据不跨重启，重新 media-access 即可）；A33（media_proxy 关闭时文件照常读取）；A46（重启后复用完成结果，不再下载） |
| Windows 证据（2026-10-21） | Windows 11 Pro 10.0.26200 x64，MSVC 19.51 /W4：0 条项目警告；`ctest --preset windows-msvc-debug` 264 项中 261 通过、3 项在线用例按设计跳过；`[gate]` 在最终代码上连续 5 次通过；`smoke.ps1` 通过（ready 行含 media／events 地址）；GCC 16.2 在 -O2 严格警告下检查改动的源码与测试（依赖作系统头）0 条项目警告 |
| Linux／macOS 与容器证据（CI 运行 39617822508、39619246485、39621420836） | 39617822508（57fe5c0）：四个任务的必需步骤全部通过，Linux（ubuntu-24.04，GCC 13.3.0）264/264，含三项 `[gate]` 用例，0 条项目警告；容器冒烟通过。39619246485（033026e）：Linux 有 1 项失败——T17 的恢复用例假定同一毫秒创建的两个任务按创建顺序领取，而领取顺序在 created_at 相同时落到随机 task_id；产品行为正确，由 72d8054 改为按 id 识别。39621420836（72d8054）：Windows、Linux、macOS 各 264/264，容器冒烟通过，全部任务成功 |
| 海外运行器上的在线闭环 | 三次运行的在线步骤（允许失败）都未完成下载：GitHub 运行器上酷我搜索正常（10 条），歌曲页 play_detail 返回 HTTP 500，mobi.s 返回 code 407 且无音源地址（UPSTREAM_FAILED），Windows、Linux、macOS 与容器相同。首次运行中 Windows 另因 `python -I` 忽略 PYTHONIOENCODING 而打印中文失败，033026e 改为脚本自行设置 UTF-8 输出，并把歌曲页不可用记为提示后继续 |
| 受阻与恢复条件 | 容器在必需步骤中只有冒烟（init、健康、发布端口、SIGTERM 收尾、同卷重建），容器内的下载、文件与重启闭环既不能在海外运行器在线完成，本机也没有 Docker。恢复：在能访问酷我音源接口的 Docker 主机上运行 `packaging/docker/e2e.sh music-service:<tag>`，或者把离线关口加入容器构建；通过后补记，T20 与 S1 改为已完成 |
| 已知限制 | 映射表中归 T20 的其余复验没有在关口里重新组合，仍由各自主责任务的用例在三平台 CI 中覆盖：A02／A03（T14）、A07／A08（T18）、A17／A19 失败路径／A20（T16、T17）、A22／A25／A26／A43／A44（T17）、A35～A37（T14）、A49（T10、T14）、A50（T17）；酷我标准品质没有凭证，A38／A39／A47 的凭证隔离与默认账号切换无法在酷我上实测，仍只有 T08／T13 的替身用例；Windows 上在线闭环的重启是强制结束（无控制台信号），优雅停机由离线关口与 Linux／容器的 SIGTERM 覆盖；本机没有 Docker，容器在线闭环只在 CI 运行，海外运行器可能被酷我按地区拒绝，因此在线结果单独记录、不作为必需检查；macOS 不在 S1 关口范围；标签写入、匹配与自动换源仍未上线，关口不把替身行为记成能力 |

### T19 已完成文件访问与票据：已完成（2026-10-21，提交 013b5be）

| 项目 | 记录 |
| --- | --- |
| 交付 | `application/media`：FileAccessService（按 owner、目标目录当前 ACL 与发布大小打开已发布文件，被移走或改写即标记 missing）、MediaTicketService（media-access 内部签发、只存 token 哈希、绑定一个目标与签发它的 access token、有界内存 handles.media_tickets 与每 owner 份额、过期哈希保留以区分 MEDIA_TICKET_EXPIRED 与 NOT_FOUND）、MediaAccessService（file 访问描述、ticket／bearer 两种 URL；asset 仅核验归属与有效期）。新端口 PublishedFiles（按偏移只读，LocalDestinationFiles 实现）；AssetService.describe；新错误码 FILE_MISSING、MEDIA_TICKET_EXPIRED（410）、DELIVERY_UNAVAILABLE（422）、FEATURE_UNAVAILABLE（501）、MULTIPLE_RANGES_UNSUPPORTED（400）。`media_server`（由 INTERFACE 改为静态库）：独立 cpp-httplib 监听与线程池上的 `GET/HEAD /media/v1/files/{file_id}`（files:read Bearer）、`/media/v1/tickets/{token}`、`/media/v1/assets/{asset_id}`（media:read，在线代理前为 501），CORS 预检与暴露头。控制入口新增 `GET /api/v1/files/{file_id}` 与 `POST /api/v1/media-access`，completed 任务的 result 带 links.file；admin/status 增加 media（listening、responses）。Finalizer 把 metadata_status 写入资产属性。ProcessHost 在 media.enabled 时于控制入口前启动媒体监听，info 声明 file_access 与 media_base_url，构建声明 media_server |
| 规则落实 | 只开放已发布文件，不读取工作目录；元数据与媒体响应都不含服务器路径。完整 GET 为 200（Content-Length、强 ETag、Accept-Ranges），单段 `a-b`／`a-`／`-n` 为 206 与 Content-Range，与文件无交集为 416 且 `Content-Range: bytes */总长`、无 JSON 体，多段为 400 MULTIPLE_RANGES_UNSUPPORTED，If-Range 不匹配返回完整 200，If-None-Match 命中 304，HEAD 忽略 Range；Bearer 文件为 attachment、`private, no-cache`，票据为 inline、`no-store` 与 no-referrer；Content-Disposition 给出 ASCII 回退名与 RFC 8187 编码的 UTF-8 名。每个响应在发头前取得 media_limits 的全局、本地文件与每 principal 额度，不足时以 JSON 429 CALLER_LIMIT_EXCEEDED／503 SERVICE_BUSY 拒绝，额度在内容提供结束、失败或客户端断开时归还；单流受 media_stream.max_stream 与写空闲限制，停机时在下一块结束。media-access：file 需要 files:read，asset 需要 media:read，他人或未知目标 404；file 的 direct 为 422 DELIVERY_UNAVAILABLE；ticket 期限为 ttl_seconds（15～300，缺省 lifetimes.media_access），bearer URL 的 expires_at 为 null；asset 先核验归属与有效期，再因在线代理／直链未启用返回 501。票据在每次请求开始时核验到期、principal 仍启用且签发它的 token 未撤销；路径中的票据在访问日志中写为 `<redacted>`。file_access 需要媒体监听，与 media_proxy 分开 |
| 覆盖的验收 | A23（其他 principal 的 file_id 在元数据、media-access 与媒体路由均为 404；未知与过期票据分别为 404 与 410 MEDIA_TICKET_EXPIRED（单元用例）；响应与日志不含路径和票据）；A24 的本地文件部分（206／416／多段／If-Range／If-None-Match／HEAD；同一 principal 额度为 1 时第二个请求 429，客户端中途断开后额度归还、随后请求成功）；A32 的票据部分（票据过期只需重新 media-access，期限不超过申请值；票据只读其目标文件）；A33（media_proxy 关闭时 asset 访问为 501，已完成文件照常读取）。另覆盖文件被移走或改写转 FILE_MISSING 且复制回去不复活、completed 任务的 links.file、ticket 的总量与每 owner 上限 |
| 真实样本（2026-10-21，Windows，匿名） | 调试构建的 music-service（media 与 file_access 启用）：下载酷我 rid 78932517 后，`GET /api/v1/files/{id}` 返回 2,829,338 字节、audio/mpeg、无路径；media-access 签发的票据 URL 以 `Range: bytes=0-15` 返回 206 与 `Content-Range: bytes 0-15/2829338`（ID3 头），完整读取与发布文件逐字节一致；服务日志中不出现票据 |
| Windows 证据（2026-10-21） | MSVC 19.51 /W4：0 条项目警告；`ctest` 260 项中 257 通过、3 项在线用例按设计跳过；媒体用例连续 5 次通过；GCC 16.2 在 -O2 严格警告下检查新增与改动的源码（含媒体与控制入口）0 条项目警告 |
| Linux 证据（CI 运行 39573491570） | ubuntu-24.04，GCC 13.3.0，glibc 2.39：0 条项目警告，260/260 通过；Windows 与容器任务同时通过 |
| 已知限制 | 未知 Range 单位或语法错误的 Range 由 cpp-httplib 在路由前以 416 拒绝，与 API「忽略未知单位」不同，尚无法在入口内覆盖；media 监听与 file_access 仍默认关闭，T20 随下载闭环启用；在线 asset 代理与直链（T34）未实现，asset 访问返回 501；token 撤销与 principal 停用只能离线执行，运行中的票据撤销路径只有代码检查、没有端到端用例；票据只在内存中，重启后需重新 media-access；资产 missing 只在读取时懒检查，没有后台扫描 |

### T17 持久调度与最小下载执行：已完成（2026-10-21，提交 fe6155c）

| 项目 | 记录 |
| --- | --- |
| 交付 | `downloads/scheduler`：DownloadScheduler（单个派发线程，按 downloads.workers 的执行槽领取；principal 轮转、同 principal 内最早到期优先；领取前查询入场账本的 readiness，跳过来源受限的任务；wake 与 1 秒周期扫描兜底；paused 只停止领取；停机 stop_claiming → wait_idle → stop_running）。`downloads/runner`：DownloadRunner 按执行阶段运行一次领取：固定绑定租用最新有效版本 → begin_attempt → 同源解析（手动目标按持久目标，auto 仍为 CAPABILITY_UNSUPPORTED）→ 详情取名（失败则用 source-id）→ downloading 流式写入 `.music-service-work/<task_id>/input.part` → processing 池中运行 AudioProcessor → Finalizer 发布。新端口 WorkFiles（工作文件追加、刷写、截断，LocalDestinationFiles 实现，基于新的 AppendFile 原语）与 DownloadQueue（wake、request_stop），DownloadService 提交后唤醒、取消进入 canceling 后通知 Runner。TaskRepository 新增 runnable、claim、begin_attempt、defer、checkpoint、requeue_interrupted，transition 可写进度、执行事实与错误；schema v2 增加 execution（公开执行事实）、checkpoint（私有续传证据）、attempt_used_ms。ProcessHost 组装下载与 processing 池，恢复后 requeue_interrupted、ready 后启动调度，停机先停领取再按 shutdown.soft 等待；`GET/PATCH /api/v1/admin/download-policy`，admin/status 增加 downloads（workers、running、paused、claimed），info 声明 downloads=true；任务快照返回 actual_track、actual_quality 与资源警告 |
| 规则落实 | 领取只换 execution_id、不计尝试；Runner 开始第一个上游操作前 begin_attempt，此时固定绝对 deadline，此后入场暂停、重新领取与重启都不延长。请求不能入场（含重定向下一跳、processing 池满）时先结束 I/O，再带执行世代与版本保存 waiting_retry（admission）、恢复阶段、未关闭的尝试与其已用运行时间、检查点，提交后才归还执行槽；尝试预算不含暂停时间。429／408／5xx 与 403／404／410（地址失效，下次重新解析）结束本次尝试为 upstream_retry（退避 retry.initial_backoff × multiplier^(n−1) 加至多 20% 抖动），尝试或截止耗尽则 failed；传输中断在尝试内按 downloads.transport_retries 重试。续传仅在 decide_resume 认可（同曲目品质、总长、强 ETag／Last-Modified）时发送 Range＋If-Range，206 的起点、总长或校验器不符则从零重下，200 覆盖旧前缀；检查点在刷写之后才记录 durable_bytes。进度按 event_stream.progress_interval 节流提交，未知长度时 total_bytes／eta_ms／stage_progress 为 null，速度为 byte/s、ETA 为毫秒。验证失败可重试（截断、非音频）时清除检查点重下，否则 failed。取消：request_cancel 提交 canceling 后通知 Runner，Runner 停止 I/O、删除工作目录并转 canceled；任何写入遇到版本冲突时重新读取，若为本执行的 canceling 则完成取消，否则放手。停机：未在软预算内结束的执行以 service_shutdown 停止，保存检查点并转为立即到期的 waiting_retry（admission）。启动时 resolving／probing 从 resolving、downloading／processing 从 downloading（检查点显示传输完成时直接重新处理）恢复。领域迁移新增 processing → waiting_retry（重下或中断）。暂停策略只在内存，重启后恢复领取 |
| 覆盖的验收 | A04（真实服务：HTTP 创建后客户端断开，后台完成并可单独查询）；A22（旧执行的检查点在新领取后被拒）；A25（无 Content-Length 时进度不伪造总量）；A26（尝试耗尽为 failed；暂停计入绝对截止，到期为 REQUEST_DEADLINE_EXCEEDED；deadline 在暂停、重新领取与重试后不变）；A27（停机时停止领取、等待后要求停止，检查点保留 16 KiB 已刷写字节，下次以 Range 续传得到完整文件）；A43（来源达到下载额度时持久延后并归还执行槽，账本无残留租约；冷却来源的任务不被领取、其他来源照常推进）；A44（两次 admission 延后 attempt 保持 1、execution_id 改变，随后真实 503 结束尝试，下一次为 attempt 2）。另覆盖完整下载与发布、同名换候选名、传输中断续传、200 覆盖部分文件、HTML 正文不发布、principal 轮转、暂停不领取、取消停止传输并清理、崩溃遗留的运行中任务按阶段重新排队，以及 admin/download-policy 的权限、校验与 admin/status |
| 真实样本（2026-10-21，Windows，匿名） | 调试构建的 music-service serve：`POST /api/v1/downloads` 酷我 rid 78932517 标准品质后不保持连接，约 1.1 秒后 completed（attempt 1），文件「蓝心羽 - 晴天.mp3」2,829,338 字节，audio.verify 实测 176,796 ms、completeness=full，仅 METADATA_NOT_WRITTEN；PATCH 暂停后 admin/status 显示 paused=true。样本与数据目录只在本机临时目录 |
| Windows 证据（2026-10-21） | MSVC 19.51 /W4：0 条项目警告；`ctest` 254 项中 251 通过、3 项在线用例按设计跳过；GCC 16.2 在 -O2 严格警告下检查新增的调度、Runner、仓储、文件原语与测试源码 0 条警告 |
| Linux 证据（CI 运行 39573491570） | 与 T19 同一次推送：ubuntu-24.04，GCC 13.3.0：0 条项目警告，260/260 通过（含调度与 Runner 用例）；Windows 与容器任务同时通过 |
| 已知限制 | 文件名规则 naming-v1 为「歌手 - 标题.扩展名」（无详情时为 source-id），尚无配置模板；探测阶段被跳过（解析结果与处理验证已足够）；默认账号凭证的固定绑定沿用 CredentialService.lease，本任务未另做凭证切换的执行用例（T20 复验 A39）；暂停策略不持久化；数据库在有限重试后仍不可写时，执行保持运行阶段并交给下次启动的 requeue；没有孤立工作目录清理与失败工作文件的保留期清理（T31）；Retry-After 只经入场账本冷却生效，任务级退避不读取它（T29）；auto 换源与匹配仍不支持 |

### T18 事件、SSE 与 HTTP 同步：已完成（2026-10-21，提交 c20ca47）

| 项目 | 记录 |
| --- | --- |
| 交付 | `application/events`：TaskEventHub（按 principal 订阅；event_limits 的全局／每 principal 订阅上限，超出为新错误码 CALLER_LIMIT_EXCEEDED／429；每订阅事件数、排队字节与单事件大小上限；同任务未发送版本被较新版本替换、旧版本忽略；溢出或超大事件关闭该订阅并要求重同步；发布只持短锁、不等待订阅者）与 PublishingTaskRepository（任务仓储装饰器，在创建、批次、取消、领取与 Runner 状态写入提交返回后发布，幂等重放、复用与失败写入不发布）。`event_server`（由 INTERFACE 改为静态库）：独立 cpp-httplib 监听与线程池上的 `GET /events/v1/downloads`，发头前完成 Bearer、downloads:read、查询参数与订阅额度检查并用 JSON 信封返回 401／403／400／429（Retry-After）／503；成功返回 text/event-stream、Cache-Control: no-store、X-Accel-Buffering: no，首帧 stream.ready {instance_id, sync_required:true}，之后 task.updated {task_id, version, status, progress, updated_at} 与注释心跳；CORS 预检只放行配置的 Origin。ProcessHost 在 events.enabled 时组装事件中心、装饰任务仓储并在控制入口前启动 SSE 监听，停止时先关闭全部订阅；info 声明 progress_sse、progress_polling 与 events_base_url，启动行输出 events_base_url |
| 规则落实 | 只有已提交的公开版本会通知；发布失败只关闭受影响流，不回滚任务。每条连接一个 worker，等待以 0.5 秒切片进行，客户端断开后由写循环及时发现并释放订阅额度。心跳间隔复核 token 与 scope，撤销后结束流；连接达到 max_connection_seconds 正常结束；写空闲使用 write_idle_seconds；服务停止时关闭全部流。不发送事件 id、不重放，Last-Event-ID 不当作恢复成功，每次连接都以 stream.ready 要求客户端重新同步。HTTP 查询不依赖 SSE；SSE 当前默认关闭，T20 起随下载闭环默认启用 |
| 覆盖的验收 | A06 的事件部分（跨 principal 无事件泄漏）；A07（订阅满额 429 时控制 API 的 info 与下载创建仍及时返回，关闭的流释放额度）；A08（慢订阅者有界：1000 次发布不阻塞，超限关闭并要求重同步，其他订阅不受影响）；A09（断线期间取消，重连后 stream.ready 并经 HTTP 查到终态）；A10（先订阅、后查询，事件与 HTTP 快照按 task_id／version 合并）；A11（迟到的旧版本不覆盖终态，队列内旧版本被替换且终态保留）；A21（停止服务结束流，重启后 instance_id 变化，持久状态经 HTTP 同步，无重复任务）；A28（未启用 SSE 时 info 声明降级，下载创建、查询、取消全部可用）。另覆盖 401／403／400／404、CORS 预检、心跳、连接期限、撤销结束流、批次每个新建任务只通知一次、重放与复用不通知 |
| Windows 证据（2026-10-21） | MSVC 19.51 /W4：0 条项目警告；`ctest --preset windows-msvc-debug` 217/217 通过（3 项在线用例按设计跳过）；SSE 集成用例连续 6 次无失败（最长为 10 秒连接期限用例）；GCC 16.2 在 -O2 严格警告下检查改动的源码与测试 0 条警告 |
| Linux 证据（CI 运行 39527819025） | ubuntu-24.04，GCC 13.3.0，glibc 2.39：0 条项目警告，217/217 通过；Windows 与容器任务同时通过 |
| 已知限制 | 进度事件按 progress_interval_ms 节流与 progress 字段的实际内容随 T17 Runner 写入；SSE 默认关闭，T20 启用并在容器冒烟中验证；没有持久事件日志，断线期间的变化只能通过 HTTP 同步取得；代理无缓冲与浏览器 fetch 客户端由 T20 及前端仓库按协议验证 |

### T16 安全发布与最小重启恢复：已完成（2026-10-21，提交 57d470e、7124d68）

| 项目 | 记录 |
| --- | --- |
| 交付 | `downloads`（由 INTERFACE 改为静态库）：`finalizer` 的 Finalizer 把已验证的工作文件发布到目标目录（发布意图、按确定性候选名预留、不覆盖的移动、资产记录与 completed 同事务提交），`recovery` 的 Recovery 在启动时按发布意图与文件证据收尾，`resume_policy` 的 decide_resume 判定半文件能否续传。`application/ports/storage` 新增 DestinationFiles 端口（存在、SHA-256 指纹、不覆盖移动、清理工作目录），发布仓储新增 relocate、带任务快照的 pending 与 in_status，complete 把 file_id 写入结果；`infrastructure/filesystem` 的 LocalDestinationFiles 在目标根内实现该端口；`application/events` 新增 PublishingPublicationRepository，发布的开始、完成与放弃提交后通知 SSE。新增任务错误码 FINALIZATION_FAILED（500）。ProcessHost 在注册目标目录之后、启动监听之前运行恢复，失败按 recovery 步骤退出，逐任务输出 startup.recovery_task 并汇总 startup.recovery |
| 规则落实 | 只发布验证阶段完成、除标签外没有失败阶段且大小与验证时一致的文件，否则 AUDIO_INVALID。候选名为「stem (n).ext」，按 path_key（ASCII 小写）跳过已被资产或其他意图占用的名字，并跳过磁盘上已存在的未知文件（磁盘存在性遵循文件系统自身的大小写规则），最多 100 个候选。证据（大小、sha256、期望路径、容器、属性、结果）在移动前随意图落库；移动使用 rename_no_replace，预留后出现的文件同样不被替换而换下一个候选，目标在移动前离线则不预留即返回 DESTINATION_UNAVAILABLE；其他移动失败放弃预留、任务转 failed（FINALIZATION_FAILED），工作文件保留以便诊断。资产 ETag 为强校验器 "sha256-<前 32 位十六进制>"。finalizing 先到时取消返回 TASK_FINALIZING，完成后 TASK_TERMINAL；取消先提交则发布放弃。恢复以文件与证据而非记录的阶段为准：目标文件与证据一致则补提交，仍在工作目录则重新预留并移动，产物缺失或被改动且尝试次数未用尽转 waiting_retry、否则 failed，目标离线则保留意图待下次启动；没有意图的 finalizing 任务转 failed，canceling 任务收尾为 canceled 并清理工作目录。续传只在资源身份（曲目、品质）、总长度一致且有强校验器（强 ETag、Last-Modified 或内容哈希）时从持久检查点继续，检查点之后未落盘的尾部截掉；弱 ETag、长度未知或证据不足一律重下，不盲目追加 |
| 覆盖的验收 | A12 基础（发布先到时取消被拒并完成，取消先到时不发布）；A17 基础（目标写入失败任务转 failed 且不完成，预留释放，工作文件保留；目标离线不预留）；A19 基础（验证失败、跳过或验证后被改动的文件不发布）；A20（模拟移动后退出：重启依意图补提交，文件仍在工作目录则完成移动，产物缺失或被改动则重试或失败、绝不完成，目标离线时保留到恢复可用，服务重启后经 ProcessHost 完成）；另覆盖不覆盖未知文件、跨资产与意图的大小写不敏感预留、竞争出现的文件、候选名与证据编解码、续传判定的各拒绝原因 |
| Windows 证据（2026-10-21） | MSVC 19.51 /W4：0 条项目警告；`ctest --preset windows-msvc-debug` 233/233 通过（3 项在线用例按设计跳过）；GCC 16.2 在 -O2 严格警告下检查改动的源码与测试 0 条警告。一次全量运行中 SSE 限额用例因另一进程占用同名临时目录而在清理时失败，单独复跑 5 次均通过，与本任务改动无关 |
| Linux 证据（CI 运行 39551904012、39552791186） | ubuntu-24.04，GCC 13.3.0，glibc 2.39。首次运行 2 项失败：父路径是普通文件时 Linux 返回 ENOTDIR，存在性检查把它当作文件系统错误，在预留前返回而未把任务转 failed（Windows 报告为不存在）；命名用例假定磁盘上的未知文件大小写不敏感。由 7124d68 把 ENOTDIR 视为不存在、并让用例只经资产验证大小写不敏感后，0 条项目警告、233/233 通过，Windows 与容器任务同时通过 |
| 已知限制 | Finalizer 尚未被 Runner 调用，下载、处理与发布的串联、running 阶段任务的恢复与续传的实际 Range 请求归 T17；恢复不清理没有任务的孤立工作目录；只用本地文件系统替身模拟崩溃窗口，更多发布窗口与磁盘故障由 T31／T32 扩大；资产的文件访问与 Range 读取归 T19 |

### N06 Bilibili 首阶段：已完成 Windows 样本验收（2026-10-23）

本轮原生接入 5 项能力，复用公共路由、凭证、队列与处理器，不新增平台专用存储或执行器。Bilibili 16 组测试／1094 条断言通过，相关 Provider／音频回归 133 组通过、5 组显式跳过，公共目录／下载集成 40 组通过。匿名搜索、稳定 cid、标准 M4A 真实下载、独立完整解码、取消和重启回读通过。真实账号、高品质／无损及其他目录能力保留缺口；详见 [验收记录](../../research/bilibili-acceptance.md) 和 [平台说明](../../../src/providers/bilibili/README.md)。
