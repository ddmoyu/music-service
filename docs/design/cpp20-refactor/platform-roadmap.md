# 音乐平台开发顺序

更新：2026-10-20。酷我优先；按由易到难和公共能力复用安排后续平台，持续扩大支持范围。当前为设计决策，尚未实现 C++ 适配器，也未执行本轮平台在线可用性测试。

## 排序口径

以原生 C++ 实现“搜索 → 单曲详情／链接解析 → 音源解析 → 后台下载 → 文件获取”的工作量为主，同时计入后续品质覆盖、凭证、协议算法、资源身份和必要音频处理的复杂度。同档优先复用前面已完成的基础设施。这里的难度是依据固定提交源码作出的工程估计，不是平台人气、音源数量或当前接口成功率排名。

范围不局限于现有 Go 与 Lanyue 研究中的远端来源，也不设固定平台数量上限。首个闭环平台确定为酷我，已研究平台的首批扩展为 5sing、Jamendo、千千；同时纳入 Spotify 和更多海外来源。新增候选按能力、依赖和验证难度插入计划，不必等全部旧平台完成。本地音乐扫描继续按后续功能管理。

平台可按目录、搜索、链接解析、歌词或音源能力独立接入。原生适配与可选外部工具的接入边界见[多平台扩展设计](backend-service/provider-extensibility.md)；数量不限不等于单次请求无限并发。

## 已研究平台的原生开发顺序

| 顺序 | 平台／source | 难度 | 首次接入重点与排序依据 |
| --- | --- | --- | --- |
| 1 | 酷我 `kuwo` | 低 | 先做标准品质：查询参数、JSON、rid 与平台直连接口即可形成主要链路，已检查基础路径无本地 AES／RSA 请求加密，但歌词端点使用 XOR + Base64 参数编码。参考实现的搜索、详情、歌单、歌词和备用取流大多使用明文 HTTP 端点（search.kuwo.cn、m.kuwo.cn、wapi.kuwo.cn、newlyric.kuwo.cn 等），只有 mobi.kuwo.cn 取流为 HTTPS；T12 须逐端点确认 HTTPS 可用性，仍需明文的端点按[传输规则](backend-service/runtime-and-tasks.md#httplib-适配规则)登记且不带秘密。核对实际品质与试听状态，高品质、复杂歌词后补。依据：[Go 搜索][kuwo-song]、[Go 取流][kuwo-core]。 |
| 2 | 5sing `fivesing` | 低 | 搜索及取流请求较直接；新增作品编号与类型的复合身份、主备地址处理。元数据缺失和时长估算必须保留未知语义，不能把翻唱等作品当作同一录音。依据：[搜索][fivesing-song]、[详情与取流][fivesing-core]。 |
| 3 | Jamendo `jamendo` | 较低 | 参考适配器使用网站 API、SHA-1 构造的 x-jam-call 头和直链字段；需补齐详情元数据、区分 download／stream 字段及实际品质。首次引入小范围摘要协议，不能把该实现描述成无签名的官方开放 API。依据：[协议与模型][jamendo-core]、[取流][jamendo-download]。 |
| 4 | 千千 `qianqian` | 较低 | 排序参数、时间戳与 MD5 签名后请求详情和 tracklink；比前三者多一套确定的签名向量。核对权限、返回结构和音质，歌单搜索不作为首次闭环的前置条件。依据：[协议与取流][qianqian-core]。 |
| 5 | JOOX `joox` | 中 | 基础路径以 GET、JSON／JSONP 和直链选择为主；地区、Cookie 与音质字段增加联调成本，集合还涉及网页数据。使用有效配置凭证，不复制参考代码里的固定会话 Cookie；地区／账号样本不足时记录为阻塞，不宣称已完成。依据：[详情、区域与集合][joox-core]。 |
| 6 | QQ 音乐 `qq` | 中 | 搜索相对简单；取流新增 songmid／文件名映射、vkey、CDN 地址和账号权限处理。先完成标准品质及外部提供的 Cookie，逐端点确认签名要求，再扩展高品质。依据：[SDK 搜索核验](../../research/lanyue/npm-sdks.md)、[vkey 取流][qq-download]。 |
| 7 | 酷狗 `kugou` | 中偏高 | 同一曲目有多套品质 hash，取流包含 MD5、Tracker／移动端接口、权限与候选分支。重点防止 hash、实际文件格式和请求品质不一致；App 凭证与高品质分支分步接入。依据：[取流分支][kugou-download]、[Lanyue 协议拆解](../../research/lanyue/README.md#酷狗一个歌曲-id-携带三套-hash)。 |
| 8 | Bilibili `bilibili` | 中偏高 | 搜索视频后通过 bvid／cid 解析独立 DASH 音轨；增加视频分 P 身份、Cookie、请求头和音轨选择，按实际端点需要接入 WBI。先直接保存独立音轨；UP 主不等于演唱者，不默认作为跨平台音乐匹配候选。依据：[Go 详情与音轨][bilibili-core]、[Lanyue 音轨流程](../../research/lanyue/README.md#bilibili搜索视频再获取-dash-独立音轨)。 |
| 9 | 网易云 `netease` | 高 | 原生迁移需逐端点选择 Linux／EAPI／WeAPI 包装，处理 MD5、AES、部分路径的 RSA、Cookie、权限和品质映射。先迁移闭环必需协议并用固定向量核验，再扩展其他包装和个人化能力。依据：[协议算法][netease-crypto]、[取流][netease-download]、[SDK 核验](../../research/lanyue/npm-sdks.md)。 |
| 10 | 咪咕 `migu` | 高 | 搜索并不复杂，但计划覆盖的取流有多资源标识、重定向、品质候选与加密音频；需把 FileKey、解密、格式验证纳入文件处理。明文标准品质可先完成，但不能据此标记全部品质已支持。依据：[取流与音频解密][migu-download]。 |
| 11 | 汽水 `soda` | 最高 | 分享页可作为搜索／预览研究入口；整曲目标还涉及 Cookie、应用签名器、官方客户端组件或远端签名服务，以及部分音频解密。跨平台部署和签名依赖边界先验证，再接整曲；只返回试听不算下载闭环完成。依据：[取流][soda-download]、[本机签名器][soda-signer]、[音频处理][soda-crypto]。 |

难度接近的平台仍可能因当前账号、地区或接口变动而改变实际工期。单个平台无法完成在线验证时，保留原因和样本缺口，继续表中下一平台；调整实现顺序须记录依据，不能将阻塞平台标为支持。

## Spotify 与开放扩展清单

Spotify 已纳入计划：先接搜索、单曲／专辑／歌单目录及链接解析，再与已验证音频来源组合。公开 Web API 面向元信息、歌单及播放控制，本计划不将其声明为原始整曲下载接口。[Spotify Web API](https://developer.spotify.com/documentation/web-api)

GitHub 的 [spotDL](https://github.com/spotDL/spotify-downloader) 读取 Spotify 信息后匹配 YouTube 音频；本项目参考其匹配和字段映射，保留 original_track 与 actual_track，不把替代音源标成 Spotify 原音。目录接入放在 S2，用第二个原生平台验证注册机制后即可开始；音频匹配与自动换源按 S4 的显式策略交付。

| 后续优先候选 | 接入目标 | 进入实施前的检查 |
| --- | --- | --- |
| Spotify `spotify` | 原生目录 Provider；OAuth／应用凭证、分页、链接解析；同源 resolve_audio 不声明支持 | 当前应用权限、配额模式、地区、scope 与端点样本；目录能力难度预计中等 |
| YouTube／YouTube Music `youtube` | 先用可选 yt-dlp adapter 提取独立音轨，作为 Spotify 匹配目标之一 | 稳定视频身份、实际音轨与品质、凭证、过期地址、工具运行时和取消；分段流另验证 |
| SoundCloud `soundcloud` | 搜索／链接、曲目和集合、已验证的音源能力 | 官方接口或 extractor 的账号要求、分页与实际传输形态 |
| Bandcamp `bandcamp` | 曲目／专辑链接、元信息和可用音频 | 整曲／试听、具体可下载资源与格式；不假定存在全站搜索能力 |
| Audius `audius` | 曲目／歌单／作者及音源，选择原生 API 或外部 adapter | 接口鉴权、分页、节点／地址变化及目标环境传输 |
| 其他来源 | 持续加入候选，每个平台按已实现能力开放 | 优先 API／协议明确、可复用公共传输且可提供稳定样本的来源，不设最终封闭清单 |

[yt-dlp 的站点清单](https://github.com/yt-dlp/yt-dlp/blob/master/supportedsites.md) 可作为发现候选的入口，包含上述多个来源；该清单不保证当前可用，也不表示搜索、集合、歌词、整曲下载等能力都齐全。每个新来源需单独建立 descriptor、域名／extractor 范围、能力样本和验证状态，不通过一个通用下载入口宣称支持全部站点。

## 与服务分期的关系

平台顺序与 S0～S5 是两个维度；不要求完成全部平台才交付某个服务阶段，也不要求补齐某个平台的全部功能才接入下一平台。

本页是平台优先顺序的唯一维护位置。具体任务、技术前置和逐平台验收见[开发任务拆分](development-tasks.md#后续平台任务)。5sing 是优先候选，不是所有平台的必经实现：第二平台关口 T41 可采用其他已验收原生来源；Spotify、YouTube 和后续开放来源分别验收。

平台可以先交付目录、解析或歌词，再补音源与其他能力；服务阶段的范围、完成条件和精确前置统一见[任务关口](development-tasks.md#总体顺序与交付关口)，本页不另列阶段表。所有平台持续验证声明的运行环境，发布组合按部署矩阵管理。

## 每个平台的完成条件

实施步骤与责任归属见[平台任务模板](development-tasks.md#单个平台的任务模板)，具体扩展行为见[扩展验收](backend-service/provider-extensibility.md#9-新平台交付模板与验收)。协议包通过只证明协议能力；产品声明还需要对应集成关口。酷我协议在 T12 验证，下载、恢复和文件获取在 T20 验证，不能相互作为前置。

首次能力、品质与算法范围以本页的平台行说明为准；未知或未验证能力保持未声明。默认品质、试听与换源策略引用[公共模型](backend-service/data-models.md#品质解析和探测)，不沿用参考实现的静默降级或试听成功逻辑。

## 配套算法的接入顺序

基础 HTTP／TLS 与 JSON → Jamendo 所需 SHA-1 → 千千及后续端点所需 MD5／编码 → 网易云所需 AES 与按端点使用的 RSA → 咪咕音频处理 → 汽水签名与音频处理。算法按实际端点清单获取，TLS 与协议加密库仍分别选型；已有摘要能力可以复用，不能因为某个平台排在后面就遗漏前面端点已需要的算法。

## 依据与验证边界

排序以本仓库的 [Go 研究](../../research/go-music-dl/README.md)、[Lanyue 研究](../../research/lanyue/README.md)及 `music-lib` 固定提交 `7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec` 为依据。2026-10-19 同时核对了[上游能力说明](https://github.com/guohuiyuan/music-lib)，其能力声明不能代替本项目的实现与在线验证。后续若更换端点或研究提交，更新对应行的难度判断和证据。

[kuwo-song]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/kuwo/song.go
[kuwo-core]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/kuwo/kuwo.go
[fivesing-song]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/fivesing/song.go
[fivesing-core]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/fivesing/fivesing.go
[jamendo-core]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/jamendo/jamendo.go
[jamendo-download]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/jamendo/download.go
[qianqian-core]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/qianqian/qianqian.go
[joox-core]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/joox/joox.go
[qq-download]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/qq/download.go
[kugou-download]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/kugou/download.go
[bilibili-core]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/bilibili/bilibili.go
[netease-crypto]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/netease/crypto.go
[netease-download]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/netease/download.go
[migu-download]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/migu/download.go
[soda-download]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/soda/download.go
[soda-signer]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/soda/localsigner.go
[soda-crypto]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/soda/crypto.go
