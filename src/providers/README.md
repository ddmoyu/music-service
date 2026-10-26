# 平台适配模块

每个平台独立成包，包含能力描述、协议构造、解析、配置与契约样本。新增平台通过构建注册项接入，不修改公共 source 枚举、业务路由或任务状态机。

- registry：ProviderRegistry 与生成的内置清单。CMake 缓存变量 `MUSIC_SERVICE_PROVIDERS` 选择编入的平台，默认清单见 [CMakeLists.txt](CMakeLists.txt)；每项需要 `<source>/CMakeLists.txt` 定义 `music_service_provider_<source>`，并在 `providers/<source>/bundle.hpp` 提供 `make_bundle()`。清单由 `registry/builtin_bundles.cpp.in` 生成，直接调用这些函数，不依赖静态构造器注册。
- kuwo：匿名单曲及集合目录、歌手搜索／详情／作品分页、官方专辑封面、歌词，以及支持可选 Cookie 的 standard／high／lossless／hires 音源；详情直连 JSON API，见 [kuwo/README.md](kuwo/README.md)。
- fivesing：第二个平台，匿名单曲搜索、详情、链接、歌词与 standard／high 音源，作品类型＋编号复合身份、登记的明文 API 端点、同一文件的主备地址（T22），见 [fivesing/README.md](fivesing/README.md)。
- netease：网易云目录、授权、音源与歌词，见 [netease/README.md](netease/README.md)。
- qq：QQ 音乐推荐、榜单、搜索、目录与原生音源，见 [qq/README.md](qq/README.md)。
- kugou：匿名推荐、分类、榜单、四类搜索、歌手资料与作品目录、LRC、封面和按官方 hash 映射的音源；另支持可选 Cookie 的官网 standard MP3 取流，见 [kugou/README.md](kugou/README.md)。
- qianqian：千千匿名单曲／专辑搜索、集合、分类及热门歌单、链接、歌词和三档音源，见 [qianqian/README.md](qianqian/README.md)。
- jamendo：匿名三类搜索、官方精选歌单搜索与推荐、原生歌曲分类、官方歌曲榜、集合详情与分页、歌词及标准 MP3 下载，见 [jamendo/README.md](jamendo/README.md)。
- bilibili：视频搜索、分 P 详情／链接、固定 cid 的独立 AAC 音轨与 Cookie 校验，见 [bilibili/README.md](bilibili/README.md)。
- migu：匿名四类搜索、实时推荐／分类／榜单、普通与数字专辑、歌单分页、歌词、试听识别及原生音源，见 [migu/README.md](migu/README.md)。
- 其他平台：在实际接入时创建，不预建空平台包，不调用外部 helper。

平台需要凭证校验时，在自身 auth 目录实现可选 ICredentialVerifier，并通过 bundle 的 auth_profiles 绑定。平台协议只返回校验或授权结果；账号选择、凭证存储与刷新协调属于 application/accounts。所有上游请求共用入场端口，不建立平台私有线程池或 semaphore。

Cookie 保存或替换后，application/accounts 使用独立有界执行器自动校验。网易云、QQ、酷我与 Bilibili 返回独立的 MembershipInfo（类型、平台名称、到期时间）；未知保持 unknown／null，会员不复用 OAuth scopes。查询、取消及展示规则见 [API 契约](../../docs/design/cpp20-refactor/backend-service/api-contract.md#cookie-自动校验与会员)，验证范围见 [验收记录](../../docs/research/credential-membership-acceptance.md)。

2026-10-24 Cookie 取流核查及补齐：网易云、QQ、酷我、咪咕、Bilibili 和酷狗的 `resolve_audio` 已声明 optional `web_cookie` 并在解析请求中使用秘密 Cookie；播放与下载共用该能力。咪咕和酷狗没有校验工厂，登录／会员未知不影响取流代码携带 Cookie。千千、5sing、Jamendo 尚未注册用户 Cookie profile，当前取流为匿名；不能把能返回 CREDENTIALS_REQUIRED 视为已支持账号接入。CDN 不自动继承平台 Cookie。酷狗账号取流当前限 standard MP3，所测 VIP 歌只返回试听；完整接入矩阵及前端发现规则见 [API 文档](../../docs/design/cpp20-refactor/backend-service/api-contract.md#各平台-cookie-取流接入状态)。此项源码核查不等同于九个平台都完成真实账号联调。

## OAuth 登录、回调与自动刷新（0.1.25）

通用流程已接入正式服务，当前 9 个内置平台尚未绑定 IOAuth2；它们的 supports_authorization_code／supports_refresh 仍为 false，调用登录入口会明确返回能力未支持。Jamendo 的匿名目录与下载不依赖 OAuth，继续保留。自动化测试通过模拟协议工厂验证整条流程，不代表真实平台账号已完成在线授权验收。

| 组成 | 已实现范围 |
| --- | --- |
| AppProfile | source、auth_profile、version、client_id、client_secret_ref、redirect_uri；最多 32 项。服务装配 SecretStore 应用秘密，保留被配置引用的秘密 |
| OAuthApplication／OAuthGrant | 只读应用快照和授权结果；秘密使用现有可清零的 SecretBytes，协议端口不直接持久化 |
| IOAuth2 | authorize_url(state, code_challenge)、exchange(owner, code, code_verifier, context)、refresh(snapshot, context)；工厂绑定在平台 oauth2 profile 上 |
| ProviderRegistry | 注入应用快照，核对应用 ID、source、auth_profile 及必要配置；按授权配置惰性初始化并共享实例，失败不缓存，不允许使用另一配置的应用凭证 |
| HttpRequest.allow_redirects | 默认为 true；设 false 后，301／302／303／307／308 均明确失败，传输层与上游协调层共同禁止跟随，防止 POST 令牌正文被转发 |
| OAuthService | 创建／查询／取消会话、一次性浏览器入口、应用专属回调、身份复核、有界后台交换与刷新扫描 |
| CredentialService／SQLite | 刷新前持久化 pending，按版本条件提交 secret、到期时间和 scopes；失败或重启中的刷新要求重新授权 |

工厂须支持并发调用；用户授权码、令牌和 verifier 只保存在本次调用内。授权 URL 必须使用 HTTPS，附带 state、code_challenge 和 code_challenge_method=S256；交换时发送对应 verifier 与配置中的同一 redirect_uri。平台必须确认支持 PKCE，不能忽略 challenge 后宣称登录可用。令牌端点只允许 HTTPS，并设置 allow_redirects=false；所有上游访问遵守 OperationContext 的截止时间及取消信号，不自动重试已发送的授权码或 refresh token。参考 [RFC 7636](https://www.rfc-editor.org/rfc/rfc7636) 与 [RFC 9700](https://www.rfc-editor.org/rfc/rfc9700)。

会话最多 128 个、每个服务身份最多 8 个；有效期 10 分钟，到期后保留 1 分钟用于查询。成功或取消不会立即腾出会话槽位。会话只存内存，重启后重新创建。浏览器入口凭一次性 ticket 设置 HttpOnly／SameSite=Lax Cookie，HTTPS 配置增加 Secure，再跳转到平台；回调同时校验 state、应用和 Cookie。禁止重复参数、重放、任意回跳地址或返回原始平台错误。创建、查询和取消需要 credentials:write，回调前后复核发起服务 token 和身份。

OAuth 使用独立的 2 个工作线程、32 项队列，满额返回 SERVICE_BUSY。授权码交换包含排队在内最多 30 秒，并受会话到期限制；控制请求不等待上游。自动刷新扫描每 5 秒最多选取每应用 2 个到期凭证，刷新时机为到期前 min(60 秒, 有效期的 1/10)。每次刷新预算 30 秒，SQLite 条件领取保证同一版本仅一次在途。刷新期间更新或撤销凭证，旧结果被丢弃；正在刷新时只改标签也会结束旧刷新并要求重新授权。失败保留 reauthorization_required，未知结果与进程重启不会重放旧 token。

自动刷新仅作用于本流程产生并绑定应用 ID／version 的已启用凭证；手工替换 secret 会清除应用绑定，之后须重新登录才能恢复自动刷新。改用其他应用身份时递增 app_profiles.version 并重新授权。获取租约不会同步等待刷新；短期令牌或上游缓慢可能使已过期凭证暂时不可用，查询凭证可看到 refresh_state。登录成功后新账号自动启用和选中，同时停用同平台原账号并清理旧秘密。

数据库仅支持当前 schema 8，不执行历史迁移；非当前开发数据需使用新的 data_dir，备份恢复也只接受当前结构。应用 token 缓存、application 模式租约及真实平台 OAuth 协议仍属于 T24 后续范围。配置和 API 示例见[配置说明](../../config/README.md#oauth-应用配置与秘密)及 [API 契约](../../docs/design/cpp20-refactor/backend-service/api-contract.md#oauth-登录会话)。

2026-10-24 验证：Windows MSVC x64 Debug／Release 构建通过；全量 CTest 607 项中 601 通过、6 项在线／样本门控跳过、0 失败。最终 OAuth／迁移专项 14 项、947 个断言通过，覆盖浏览器绑定、跨应用／跨账号隔离、PKCE 标准向量、拒绝／异常／取消／到期、参数重复、队列过载、日志脱敏、刷新轮换、存储失败及跨连接重启恢复。Release 实测 Jamendo 匿名歌曲 49292 的详情与 MP3 下载成功，产物 310,500 字节。证据为 out/oauth-flow-debug-build.log、out/oauth-flow-release-build.log、out/oauth-flow-ctest.log、out/oauth-flow-targeted.log、out/oauth-release-smoke-report.json；不包含真实平台 OAuth 账号授权验收。

## 注册规则（T07）

`ProviderRegistry::build` 一次报告全部问题，任何一项都使启动失败：

- 契约版本与 `provider_contract_version` 不一致；source 重复；配置了未编入的 source；配置的 implementation 与 bundle 的执行方式不同；options 未通过 bundle 校验器（无校验器时只接受空对象）。
- 能力声明与工厂不一致：声明却无工厂、有工厂却未声明、空工厂、同一键重复，或工厂产出的端口与该能力键的端口不同；本构建没有端口的能力键。
- 授权：auth_profile 名称非法或重复；能力引用未注册的 profile；需要凭证却未列出 profile；`verify_credentials` 列出的 profile 没有校验器，或校验器所属 profile 未在 `verify_credentials` 中声明。supports_verify 由实际工厂绑定生成；supports_refresh／supports_application_token 保留 API 字段，当前均为 false。

已编入但未出现在 `providers` 配置中的平台按 ProviderConfig 缺省处理（启用、native、空 options），但不会自动进入 `catalog.default_sources`。

工厂收到的 ProviderSetup 含 source、已校验的 options、共享的上游客户端（`UpstreamHttp`：统一入场、TLS 与重定向规则）及配置的 rate_limit_group；平台包不自建连接或计数。

注册表构建后不可变。describe／list 只读描述，不创建对象、不联网、不建线程；`require<Interface>(source)` 首次调用时创建该能力对象并复用，工厂失败不缓存。未注册、禁用、不支持分别返回 UNKNOWN_PROVIDER、PROVIDER_DISABLED、CAPABILITY_UNSUPPORTED。providers 列表按 source 排序分页（1～100），游标绑定 principal、筛选条件与注册表代次：换调用方或筛选为 CURSOR_MISMATCH，重启后为 CURSOR_EXPIRED。可用状态（availability）与调用方凭证摘要的投影在 T08／T10 加入。
