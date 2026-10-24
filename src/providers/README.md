# 平台适配模块

每个平台独立成包，包含能力描述、协议构造、解析、配置与契约样本。新增平台通过构建注册项接入，不修改公共 source 枚举、业务路由或任务状态机。

- registry：ProviderRegistry 与生成的内置清单。CMake 缓存变量 `MUSIC_SERVICE_PROVIDERS` 选择编入的平台，默认清单见 [CMakeLists.txt](CMakeLists.txt)；每项需要 `<source>/CMakeLists.txt` 定义 `music_service_provider_<source>`，并在 `providers/<source>/bundle.hpp` 提供 `make_bundle()`。清单由 `registry/builtin_bundles.cpp.in` 生成，直接调用这些函数，不依赖静态构造器注册。
- kuwo：匿名单曲及集合能力、歌手搜索／详情／作品分页、官方专辑封面、歌词，以及 standard／high／lossless 音源；详情直连 JSON API，见 [kuwo/README.md](kuwo/README.md)。
- fivesing：第二个平台，匿名单曲搜索、详情、链接、歌词与 standard／high 音源，作品类型＋编号复合身份、登记的明文 API 端点、同一文件的主备地址（T22），见 [fivesing/README.md](fivesing/README.md)。
- netease：网易云目录、授权、音源与歌词，见 [netease/README.md](netease/README.md)。
- qq：QQ 音乐推荐、榜单、搜索、目录与原生音源，见 [qq/README.md](qq/README.md)。
- kugou：匿名推荐、分类、榜单、四类搜索、歌手资料与歌曲／专辑分页、集合、链接、LRC、专辑封面和按官方 hash 映射的音源，见 [kugou/README.md](kugou/README.md)。
- qianqian：千千匿名单曲／专辑搜索、集合、分类及热门歌单、链接、歌词和三档音源，见 [qianqian/README.md](qianqian/README.md)。
- jamendo：匿名三类搜索、官方精选歌单搜索与推荐、原生歌曲分类、官方歌曲榜、集合详情与分页、歌词及标准 MP3 下载，见 [jamendo/README.md](jamendo/README.md)。
- bilibili：视频搜索、分 P 详情／链接、固定 cid 的独立 AAC 音轨与 Cookie 校验，见 [bilibili/README.md](bilibili/README.md)。
- migu：匿名四类搜索、实时推荐／分类／榜单、普通与数字专辑、歌单分页、歌词、试听识别及原生音源，见 [migu/README.md](migu/README.md)。
- 其他平台：在实际接入时创建，不预建空平台包，不调用外部 helper。

平台需要凭证校验时，在自身 auth 目录实现可选 ICredentialVerifier，并通过 bundle 的 auth_profiles 绑定。当前 9 个平台未使用应用令牌交换或 OAuth 刷新；0.1.24 已提供下述 OAuth 协议端口，完整授权协调与缓存仍待 T24。平台协议只返回校验结果；账号选择和凭证存储属于 application/accounts。所有上游请求共用入场端口，不建立平台私有线程池或 semaphore。

## OAuth 授权基础（0.1.24）

这部分是可测试的原生协议扩展接口，当前没有真实平台绑定，不能据此宣称已支持 OAuth 登录。

| 组成 | 已实现范围 |
| --- | --- |
| AppProfile | 类型化 source、auth_profile、version、client_id、client_secret_ref、redirect_uri；最多 32 项。未启用构建能力时拒绝非空配置，启用的测试环境校验必需字段和平台绑定 |
| OAuthApplication／OAuthGrant | 只读应用快照和授权结果；秘密使用现有可清零的 SecretBytes，协议端口不直接持久化 |
| IOAuth2 | 授权 URL、授权码交换、刷新接口；工厂绑定在平台的 oauth2 授权配置上 |
| ProviderRegistry | 注入应用快照，核对应用 ID、source、auth_profile 及必要配置；按授权配置惰性初始化并共享实例，失败不缓存，不允许使用另一配置的应用凭证 |
| HttpRequest.allow_redirects | 默认为 true；设 false 后，301／302／303／307／308 均明确失败，传输层与上游协调层共同禁止跟随，防止 POST 令牌正文被转发 |

工厂须为并发调用安全；用户授权码、令牌和刷新状态只保存在本次调用内，不放入共享实例。令牌交换与刷新请求须设置 allow_redirects=false，结果由未来账号协调层进行版本比较、持久化与并发刷新合并。

正式服务没有启用 app_profiles 构建能力，也没有授权会话、回调路由、实际秘密装配或刷新协调；当前仍只接受空 app_profiles。注册表的 supports_authorization_code／configured 描述内部协议工厂，不新增对外授权路由。当前 9 个平台没有 OAuth 工厂，公开 supports_refresh 仍为 false。应用令牌缓存、真实平台协议和在线登录验收继续属于 T24。

离线验证覆盖应用配置能力开关、必需字段、跨平台／跨授权配置隔离、并发单次初始化、失败后重试，以及本地 TLS 上同源／跨源的禁止重定向；不包含真实账号登录。

2026-10-24 验证：Windows MSVC Debug／Release 构建通过，全量 CTest 599 项中 593 通过、6 个在线／样本用例按配置跳过、0 失败；此前 app_profiles 误放行导致的配置测试失败已修复。Release 0.1.24 实际运行 check-config：空应用配置退出码 0，非空未启用配置退出码 2。日志为 out/oauth-foundation-debug-build.log、out/oauth-foundation-release-build.log、out/oauth-foundation-ctest.log 和 out/oauth-foundation-release-check.json。

## 注册规则（T07）

`ProviderRegistry::build` 一次报告全部问题，任何一项都使启动失败：

- 契约版本与 `provider_contract_version` 不一致；source 重复；配置了未编入的 source；配置的 implementation 与 bundle 的执行方式不同；options 未通过 bundle 校验器（无校验器时只接受空对象）。
- 能力声明与工厂不一致：声明却无工厂、有工厂却未声明、空工厂、同一键重复，或工厂产出的端口与该能力键的端口不同；本构建没有端口的能力键。
- 授权：auth_profile 名称非法或重复；能力引用未注册的 profile；需要凭证却未列出 profile；`verify_credentials` 列出的 profile 没有校验器，或校验器所属 profile 未在 `verify_credentials` 中声明。supports_verify 由实际工厂绑定生成；supports_refresh／supports_application_token 保留 API 字段，当前均为 false。

已编入但未出现在 `providers` 配置中的平台按 ProviderConfig 缺省处理（启用、native、空 options），但不会自动进入 `catalog.default_sources`。

工厂收到的 ProviderSetup 含 source、已校验的 options、共享的上游客户端（`UpstreamHttp`：统一入场、TLS 与重定向规则）及配置的 rate_limit_group；平台包不自建连接或计数。

注册表构建后不可变。describe／list 只读描述，不创建对象、不联网、不建线程；`require<Interface>(source)` 首次调用时创建该能力对象并复用，工厂失败不缓存。未注册、禁用、不支持分别返回 UNKNOWN_PROVIDER、PROVIDER_DISABLED、CAPABILITY_UNSUPPORTED。providers 列表按 source 排序分页（1～100），游标绑定 principal、筛选条件与注册表代次：换调用方或筛选为 CURSOR_MISMATCH，重启后为 CURSOR_EXPIRED。可用状态（availability）与调用方凭证摘要的投影在 T08／T10 加入。
