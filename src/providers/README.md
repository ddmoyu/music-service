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

平台需要凭证校验时，在自身 auth 目录实现可选 ICredentialVerifier，并通过 bundle 的 auth_profiles 绑定。当前 9 个平台未使用应用令牌交换或 OAuth 刷新，对应端口与缓存留待 T24 按实际平台需求实现。平台协议只返回校验结果；账号选择和凭证存储属于 application/accounts。所有上游请求共用入场端口，不建立平台私有线程池或 semaphore。

## 注册规则（T07）

`ProviderRegistry::build` 一次报告全部问题，任何一项都使启动失败：

- 契约版本与 `provider_contract_version` 不一致；source 重复；配置了未编入的 source；配置的 implementation 与 bundle 的执行方式不同；options 未通过 bundle 校验器（无校验器时只接受空对象）。
- 能力声明与工厂不一致：声明却无工厂、有工厂却未声明、空工厂、同一键重复，或工厂产出的端口与该能力键的端口不同；本构建没有端口的能力键。
- 授权：auth_profile 名称非法或重复；能力引用未注册的 profile；需要凭证却未列出 profile；`verify_credentials` 列出的 profile 没有校验器，或校验器所属 profile 未在 `verify_credentials` 中声明。supports_verify 由实际工厂绑定生成；supports_refresh／supports_application_token 保留 API 字段，当前均为 false。

已编入但未出现在 `providers` 配置中的平台按 ProviderConfig 缺省处理（启用、native、空 options），但不会自动进入 `catalog.default_sources`。

工厂收到的 ProviderSetup 含 source、已校验的 options、共享的上游客户端（`UpstreamHttp`：统一入场、TLS 与重定向规则）及配置的 rate_limit_group；平台包不自建连接或计数。

注册表构建后不可变。describe／list 只读描述，不创建对象、不联网、不建线程；`require<Interface>(source)` 首次调用时创建该能力对象并复用，工厂失败不缓存。未注册、禁用、不支持分别返回 UNKNOWN_PROVIDER、PROVIDER_DISABLED、CAPABILITY_UNSUPPORTED。providers 列表按 source 排序分页（1～100），游标绑定 principal、筛选条件与注册表代次：换调用方或筛选为 CURSOR_MISMATCH，重启后为 CURSOR_EXPIRED。可用状态（availability）与调用方凭证摘要的投影在 T08／T10 加入。
