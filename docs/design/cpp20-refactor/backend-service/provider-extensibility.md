# 多平台适配与扩展设计

更新：2026-10-19。状态：设计基线，未实现。首个真实平台为酷我；支持范围持续扩展，不把当前平台清单作为产品或协议上限。平台排期见[开发顺序](../platform-roadmap.md)，字段、路由和执行预算分别以[模型](data-models.md)、[API](api-contract.md)和[运行时](runtime-and-tasks.md)为准。

## 1. 扩展目标

新增一个使用现有能力的平台，应只增加平台包、构建注册项、配置描述与契约样本。控制路由、任务状态机、SQLite 表、文件发布及客户端不增加按平台名称判断的分支。只有出现新的通用能力或资源形态时才扩展公共契约，并先说明它为什么不能用现有类型表达。

“支持平台”按能力分别记录：目录、单曲搜索、集合、歌词、同源音源、账号验证等可以独立交付。只有目录能力的平台也能接入；不能把搜索成功、获得试听或第三方工具列出了站点，当作整曲下载已经完成。

扩展数量与运行资源分开管理：平台注册表不设固定数量枚举，单次查询、连接、队列、缓存和外部进程仍有预算。安装一百个平台不意味着启动一百套线程池、同时探测一百个平台或默认搜索全部来源。

## 2. 平台身份与实现身份

| 对象 | 规则 |
| --- | --- |
| `source` | 稳定字符串，格式 `[a-z][a-z0-9_]{0,63}`；不能是需要修改公共枚举的封闭列表 |
| `TrackRef`／集合 ref | 继续使用 source + 不透明 id；平台私有 hash、分页信息和资源字段留在版本化上下文中 |
| `implementation_id`／版本 | 标识原生实现及其版本；更换实现不改公开 source、已有曲目或任务身份 |
| 同平台多种协议 | 在平台包内按操作选择策略；同一 source 的同一能力只能有一个生效的顶层绑定，避免重复结果和随机路由 |
| 原曲与音源 | original_track 保存用户目标，actual_track 保存实际音频曲目；不能用目录平台名覆盖真实下载来源 |
| 跨源匹配证据 | ISRC、艺术家、标题、时长、专辑和版本可作证据；ISRC 可缺失或错误，不直接成为全局主键 |

`kuwo`、`netease`、`qq` 等是来源；`yt-dlp`、`spotDL` 等只是参考项目，既不是平台身份，也不作为运行依赖。YouTube 与 YouTube Music 的同一视频采用 `youtube` 的规范身份，音乐搜索入口作为该适配器的查询策略。

## 3. 能力接口与注册

沿用小接口组合：ITrackSearch、ITrackDetail、ICollectionItems、IDiscovery、ILyrics、ILinkParser、IAssetResolver、ICredentialVerifier 等；不引入要求所有平台实现全部功能的巨型 Provider 基类。公共接口只使用项目 DTO、Result 和 ProviderContext，不暴露 JSON／HTTP 库类型。

一个 `ProviderBundle` 包含 descriptor、实现版本、能力工厂、配置校验器、auth_profiles 授权工厂表，以及可选的私有上下文迁移和文件处理适配注册项。`ProviderRegistry` 提供 `describe(source)` 与受类型约束的 `require<Capability>(source)`；授权能力通过 source + auth_profile + kind 精确绑定。由描述声明和实际工厂共同决定能力，不能仅靠字符串宣称“支持”。

注册顺序：编译清单 → 校验配置及 source 唯一性 → 校验能力绑定和依赖 → 构建不可变注册表 → 按需创建实现。生成的 C++ 注册表直接引用工厂符号，避免依靠全局静态构造器被链接器裁剪后的偶然注册。首版变更配置与实现均重启生效。

| 维度 | 含义 |
| --- | --- |
| supported | 当前实现是否有该能力；与一次网络请求是否成功无关 |
| enabled | 部署是否启用该平台／能力 |
| availability | 当前调用方最近已知的可用状态与原因，允许 unknown；状态带检查时间，不隐含实时探测 |
| requirements | 能力所需凭证类型／scope、地区、依赖工具与传输形态；凭证和账号状态按 principal 投影 |
| pagination／quality hints | 分页语义与可能的品质，不承诺每个账号和曲目均可获得 |

缺失可选依赖或凭证只影响相应能力，不阻止其他平台运行。启用的 bundle 结构错误、重复 source 或不兼容协议版本在启动校验时失败；网络失败、限流和账号条件不足通过能力状态呈现。

## 4. 只用原生实现

所有平台都由本项目自己的 C++ 代码完成协议解析、签名、解密与下载，与服务同版本编译，共享 HTTP、秘密、配额与文件基础设施。**不引入任何外部第三方下载服务或工具**：不调用 yt-dlp、spotDL、签名器、其他语言 SDK、FFmpeg 等外部程序，不经过第三方解析服务或代理接口；自己解析与下载是本项目存在的意义。

| 规则 | 说明 |
| --- | --- |
| 实现方式 | 每个平台一个独立 `music_service_provider_<source>` target，只链接实际用到的编码／加密库（以源码依赖固定版本，见构建与依赖） |
| 无法原生实现的能力 | 明确记为受阻或不支持并关闭，不以外部工具补齐；例如签名依赖平台客户端组件、只能靠第三方服务取得的地址 |
| 第三方开源项目 | 只作为协议与字段映射的参考资料，不作为运行依赖，也不打包其可执行文件 |

首版静态注册原生实现；不加载任意 C++ 动态库，也不承诺跨编译器 C++ ABI 或热卸载。

## 5. 目录与实际音源的关系

目录源与实际音频来源通过独立能力表达。目录能力本身不声明可下载音频；具体跨源组合属于 S4，只有通过对应平台的真实验证后才能声明支持。

```text
链接／搜索 → 目录能力 → 原曲元信息
    → MatchService（用户手动选择，或显式启用的自动策略）
    → 已原生接入并验证的来源的 IAssetResolver
    → 公共下载 Runner → 处理／发布
```

无匹配选择且自动换源关闭时，目录源的同源解析返回 CAPABILITY_UNSUPPORTED；不得偷偷转到另一平台。手动选择先核验匹配上下文，再检查实际目标的音源能力，不能在检查原曲 `resolve_audio=false` 时提前挡住合法选择。自动模式只有在服务开关、请求允许及匹配条件同时满足时才可处理目录源的能力缺失。

任务和资产同时保留 original_track、actual_track、匹配选择／证据摘要与实际品质，不能将另一来源的音频宣称为原平台音频。集合仍由客户端分页选择后分批提交，不增加完整后台枚举或目录同步删除功能。

## 6. 凭证与账号

凭证从 Cookie 扩展为 `cookie`、`oauth2`、`api_key`，具体输入 schema 和 scope 由能力声明约束；平台私有字段通过指定的 auth_profile 校验，不开放任意请求头注入。无账号请求只在该能力允许时使用匿名上下文。

服务应用的 client_id／client_secret 与用户账号凭证分别管理。应用秘密由管理配置引用 SecretStore；用户 token／Cookie 按 principal + source + credential_id 隔离。应用级 token 仅用于已验证可用的公共能力，不能代替用户授权。

CredentialService 是 application/accounts 的用例协调者。它选择 [AccountBinding 与 AuthIdentityKey](data-models.md#账号绑定与授权身份)、租用秘密、合并刷新并提交结果；平台包只实现授权协议，infrastructure 提供通用网络、SecretStore 和持久化。公共服务不按 source 分支选择 token 端点或解析平台字段。

### 授权能力注册与调用

auth_profiles 以 source + auth_profile + kind 为键，注册输入 schema、所需 app_profile、版本及下列独立可选工厂；不是要求所有平台实现的统一登录基类。

| 端口 | 输入与输出 | 责任 |
| --- | --- | --- |
| `ICredentialVerifier` | 授权快照、操作上下文 → valid／invalid／unknown、账号／scope 证据 | 有界校验，不更新秘密或任务 |
| `ICredentialRefresher` | 固定账号的过期或将过期快照、所需应用租约 → 替换秘密、到期时间与 scope 证据 | 平台刷新协议；无通用递归 CredentialService 调用 |
| `IApplicationTokenProvider` | 应用 profile 快照、scope 请求、操作上下文 → 应用 token 结果 | 仅已实现的应用授权流程；不能代替用户授权 |

这些端口位于 application/ports/providers，具体实现随 `providers/<source>/auth` 的平台 target 编译和注册；通用 OAuth 编码可以复用，平台端点、参数和响应解释留在平台包。Registry 的类型化授权访问按 profile 返回对应工厂；普通 resolve/search 能力工厂不隐式负责刷新。

启动校验重复 profile、kind/schema 冲突、声明能力却无工厂及应用 profile 类型不匹配。auth_methods 的 supports_verify／supports_refresh／supports_application_token 由实际绑定生成；缺少可选秘密或部署工具只使依赖能力不可用。新增平台授权协议不修改通用 CredentialService、HTTP 路由或 SQL 表。

用户刷新按稳定 AccountBinding 合并；应用 token 获取按 app_profile、有效 scope 请求与配置版本合并，同一条目只允许一个操作在途。多个 principal 可共用应用 token 获取，但业务数据仍按完整身份键隔离；每位等待者保留自己的 deadline。适配器只取得本次不可变快照，使用公共上游入场端口，不访问 Repository 或全局当前账号。

用户刷新结果只进入 SecretStore 提交路径：先写新 secret_ref，再按原凭证版本、应用配置版本及未撤销条件提交。成功才发布新 AuthIdentityKey，竞争失败丢弃候选并回收孤立秘密，不能覆盖人工更新或复活已删除账号。应用 token 则按配置版本条件替换受限内存条目，并使用独立 app_token_id／version；不会回写管理配置或用户凭证，重启后重新获取。持久化边界见[秘密提交](internal-contracts.md#62-凭证和秘密提交)。

获取应用 token 与用户刷新是有方向的依赖：先取得所需应用租约，再进入用户刷新；应用 token 工厂不能反向等待用户刷新。同一短请求池中的验证／刷新不能提交子任务后同步等待该池；等待者留在受限协调状态，协议工作按[入场规则](runtime-and-tasks.md#上游入场与租约)独立派发。轮换 refresh token 的请求一旦发送，不因超时或重复 401 自动重放；结果不确定时返回明确的刷新失败，按平台已验证的恢复方式处理。

S1 可只实现 Cookie 导入与校验；没有刷新工厂时明确要求外部更新凭证。扫码、短信、OAuth 跳转和授权码交换仍按需要新增独立能力，不由声明 supports_refresh 暗示已经支持完整登录。

## 7. 传输、处理与责任归属

Provider 只产出内部资源计划和证据。资源计划按类型表达 `HttpAsset`（今后新增的传输形态同样由服务自己实现），包含实际来源、请求条件、指纹、有效期及所需处理；API 不返回秘密或可执行命令。文件处理扩展通过受控 processor_id 和版本化私有参数绑定，不在 Runner 中增加 按 source 分支的处理逻辑。

ResolveService 选择和验证计划，TransferExecutor 按计划类型选择实现，AudioProcessor 按处理能力执行，Finalizer 统一发布。任务、幂等、取消、重试、恢复、SSE、文件归属和大小上限只有公共模块负责。

执行快照记录 implementation_id、context_schema_version、processing plan 版本和恢复证据，不持久化明文 token／短期签名 URL。升级后可按稳定 TrackRef 重新解析；不能迁移的私有上下文显式失败，不把旧缓存盲交给新实现。平台移除／禁用后，历史与已发布文件仍可读取，新解析失败；恢复任务按当前能力给出终态错误，不静默换源。

## 8. 大量平台的运行方式

- Registry 只保存轻量 descriptor 和工厂；业务对象按需初始化，缓存由公共预算管理。不开每平台线程池，不在启动时或读取 providers 列表时逐一联网登录、探测。
- Provider 列表分页，支持能力和 enabled 筛选。默认搜索使用配置的 default_sources；用户可选择更多已安装来源分批查询。单请求来源数与 fan-out 是可配置预算，不是支持平台数量上限。
- downloads/Scheduler 管理持久任务和执行槽；runtime/admission 是进程内上游额度的唯一管理者，按 principal、实际 source、origin、流量类别和共享应用 rate-limit group 发放租约。多个实现或多个名义来源共用上游应用配额时仍共用限流，不能因换 adapter 或刷新 token 获得新的配额。分阶段入场、延后和释放见[运行时](runtime-and-tasks.md#上游入场与租约)。
- 上游 429 遵守 Retry-After；退避与熔断按平台／操作／账号等实际作用域执行。用户凭证失效不熔断其他账号；单平台超时不阻塞其他平台。HTTP 短请求、下载、外部进程仍使用独立有界执行额度。
- 缓存、游标、上下文和在途查询合并统一包含完整 AuthIdentityKey，再加入操作、实体／查询、地区和实现上下文版本；不只用凭证版本代替账号身份。跨源任务按每次实际请求来源计量，不能只统计 original_track.source。

## 9. 新平台交付模板与验收

每个平台包至少包含 descriptor、能力实现、protocol／parsing、去凭证 fixtures、契约用例、配置／凭证说明及固定研究依据。能力状态按“计划 → 离线契约通过 → 在线验证通过 → 发布”记录，未实现的候选只在开发清单中，不伪装成运行时已安装 Provider。

协议包验证与产品下载集成分开：酷我 T12 验平台协议，持久下载归 T20；其他平台复用已完成的下载基线。第二个真实平台扩展关口为 T41，优先使用 5sing，受阻时可改用已验收的其他原生来源；候选平台不反向依赖 T41。精确前置与分步交付见[任务清单](../development-tasks.md#单个平台的任务模板)。

| 验收 | 要证明的行为 |
| --- | --- |
| 最小扩展 | 添加第二个平台不修改通用路由、任务状态机或增加平台专用数据库表 |
| 能力组合 | 目录型、仅链接解析型、目录加音源型均可注册；不支持的能力正确返回 422 |
| 声明一致 | 重复 source、声明却没有绑定、协议版本不兼容在注册时发现 |
| 多源规模 | 用模拟 descriptor 注册至少 100 个来源，列表可分页；默认搜索仍有界，未选择来源不联网、不创建专属线程 |
| 凭证隔离 | 跨 principal 及同 principal 同平台两个 version=1 的账号均不串用；刷新、撤销、更新竞争遵守版本条件 |
| 授权扩展 | 第二个具有不同授权协议的模拟平台仅增加 bundle 工厂与样本；缺少绑定、重复 profile 在启动发现；未实现刷新不对外宣称支持 |
| 额度归属 | 目录解析、实际音源和重定向使用同一入场账本；共享应用组不随账号 token 版本重置，实际结束后才释放 |
| 目录换源 | 原曲与音频目标分别保留；策略关闭不换源，手动选择可用，误匹配不伪装同源下载 |
| 故障隔离 | 一个来源限流或卡住，其他来源和控制入口仍可处理；取消后实际 I/O 结束 |
| 恢复与升级 | 私有上下文版本变化和平台禁用均明确处理；已发布文件不依赖 Provider 继续存在 |
| 目标部署 | 启用的原生／外部组合分别验证 Windows、Linux、容器；缺少可选工具不影响基础包 |

S0／S1 先完成稳定 source、能力注册、酷我和一组模拟契约；S2 用第二个真实平台与模拟目录源验证扩展边界；目录跨源下载按现有 S4 匹配能力交付。不会为“未来很多平台”预先建立空实现、热加载框架或独立分布式服务。
