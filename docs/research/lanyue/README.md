# Lanyue 多平台音乐功能实现拆解

Lanyue（揽乐）通过“平台插件注册表 → 统一网关 → 搜索聚合／音源解析 → 主进程下载队列”实现多平台音乐搜索与下载。它是 Electron 桌面应用，主要业务运行在 Node.js 主进程，前端通过 IPC 调用业务接口。

它与 [Go Music DL 功能实现拆解](../go-music-dl/README.md) 适合对照阅读：前者更适合研究桌面应用的任务队列、流式播放和插件能力声明，后者更适合研究 Go 平台协议实现及 Web 服务。本篇沿用相同的拆解方式，重点追踪实际调用链和实现边界。

## 分析范围与版本

| 项目 | 分析基准 |
| --- | --- |
| 整理日期 | 2026-10-19 |
| 仓库 | [maliang04110233-dot/Lanyue](https://github.com/maliang04110233-dot/Lanyue) |
| 分析提交 | `53309e73c73316b7dd2ce721be4d2b0aede327a3` |
| 应用版本 | `package.json` 声明 `1.0.37`，包名为 `music-downloader` |
| 运行方式 | Electron 主进程、preload 桥接、浏览器渲染进程；Vite 构建 |
| 已注册来源 | 网易云、QQ 音乐、Bilibili、酷狗、酷我、咪咕、5sing、汽水 |
| 主要范围 | 单曲与专辑搜索、歌手、歌单详情与分类入口、排行榜、下载、续传、播放、换源 |

版本依据：[package.json][package]、[锁文件][lock]。下文源码链接固定到分析提交。

本文依据源码静态分析，没有启动该应用、安装其依赖、登录平台或实测当前下载成功率。表格中的“支持”表示存在实现与接线；接口稳定性、登录权限、可取得的音质与完整时长仍需单独验证。源码注释或项目介绍与实际分支不一致时，以执行路径为准。

补充研究：[搜索使用的 NPM SDK](npm-sdks.md)，分析锁定的网易云／QQ 发布包、实际请求与加密方式，并核实 QQ Cookie 传递路径。

### 先看几个关键结论

| 问题 | 源码中的实际情况 |
| --- | --- |
| 是否用一个聚合接口搜索全部平台 | 应用分别调用 8 个平台适配器，再合并结果；网易云与 QQ 的部分请求委托给第三方 NPM SDK |
| 全平台搜索怎样并发 | `gateway.fanOut()` 最多同时执行 4 个平台调用；这是单次聚合的限制，不是全应用的总请求限制 |
| 是否所有平台都支持全部功能 | 否。歌曲搜索和取流是必需能力，专辑、歌单、分类和榜单按平台分别声明 |
| 搜索成功是否意味着可以下载 | 否。下载时另外解析音源地址；权限、试听、过期地址和平台接口错误都会导致失败 |
| 是否全部直接调用平台服务 | 否。酷我高品质路径会先尝试 `kw-api.cenguigui.cn` 第三方解析服务 |
| 是否正在自动跨平台换源 | 当前生产接线中 `crossSourceEnabled()` 固定返回 `false`；候选匹配与换源实现仍保留 |
| 是否自动恢复中断文件 | 恢复尚未启动的排队任务；异常退出时正在下载的任务转为失败。传输层另有同一次调用内的 Range 重试 |
| 普通下载是否依赖 FFmpeg | 不依赖其转码能力。音频直接流式写盘，标签由 node-id3／可选 Python Mutagen 处理；额外转码与裁剪使用 FFmpeg |

依据：[搜索门面][facade-search]、[并发聚合][gateway-fanout]、[酷我实现][kuwo]、[换源开关][fallback-policy]、[下载队列][queue]、[下载传输与标签][downloader]。

## 阅读目录

- [整体架构与调用入口](#整体架构与调用入口)
- [统一模型与插件能力](#统一模型与插件能力)
- [单曲搜索与多平台聚合](#单曲搜索与多平台聚合)
- [八个平台如何搜索和获取音源](#八个平台如何搜索和获取音源)
- [专辑与歌手](#专辑与歌手)
- [歌单详情与歌单分类](#歌单详情与歌单分类)
- [排行榜与首页推荐](#排行榜与首页推荐)
- [分享链接解析](#分享链接解析)
- [下载队列与状态管理](#下载队列与状态管理)
- [流式下载与断点续传](#流式下载与断点续传)
- [音频标签歌词与格式处理](#音频标签歌词与格式处理)
- [在线播放与直链刷新](#在线播放与直链刷新)
- [换源匹配与当前关闭的路径](#换源匹配与当前关闭的路径)
- [与 Go 项目和 C++20 重构的关系](#与-go-项目和-c20-重构的关系)
- [实现边界与验证重点](#实现边界与验证重点)
- [源码阅读顺序](#源码阅读顺序)

## 整体架构与调用入口

```text
Electron 渲染进程：搜索页、首页、播放器、下载列表
                    │ window.musicAPI / IPC
                    ▼
preload.js + shared/ipcContract.js
                    │ 白名单与参数契约
                    ▼
main/ipc/*：搜索、集合详情、下载、播放等入口
                    │
                    ▼
api/index.js：业务门面
    ├── searchMusic / searchAlbum / recommendations
    └── ResolveTrackService：本源取流、试听判断、换源策略
                    │
                    ▼
gateway.js：能力检查、Cookie 注入、异常收敛、聚合调度
                    │
                    ▼
pluginRegistry.js → platforms/*.js
    ├── NeteaseCloudMusicApi / qq-music-api
    └── request.js / Node http、https → 平台 API、分享网页、解析服务

下载队列 → 解析音源 → downloader.js → .tmp → 正式音频
                                   → 标签与歌词 → SQLite 历史

在线播放 → proxy-play → streamRegistry → lanyue-stream://令牌
                                   → streamProtocol → 上游音频流
```

| 文件或目录 | 职责 |
| --- | --- |
| `src/main/index.js` | 应用生命周期、窗口、服务组装、队列初始化 |
| `src/main/preload.js` | 向页面暴露受约束的 `musicAPI`，把方法映射为 IPC 调用 |
| `src/shared/ipcContract.js` | IPC 通道、可调用窗口、方法名称和参数规则 |
| `src/main/ipc/search.js` | 歌曲、专辑、歌手、首页、集合详情与链接解析入口 |
| `src/api/index.js` | 平台加载、Cookie 接入、聚合搜索和音源服务组装 |
| `src/api/pluginRegistry.js` | 平台发现、manifest 校验、能力与策略派生 |
| `src/api/gateway.js` | 对平台的统一调用出口 |
| `src/api/platforms/` | 八个平台的参数组装、请求、响应转换和取流实现 |
| `src/api/recommendations.js` | 首页区块、排行榜、歌单及歌手相关编排 |
| `src/api/services/resolveTrackService.js` | 从歌曲模型解析可播放／可下载的音源 |
| `src/main/downloadQueue.js` | 并发额度、状态、任务恢复、重试与取消 |
| `src/utils/downloader.js` | 流式 HTTP 下载、临时文件、传输重试、标签写入 |
| `src/main/streamProtocol.js` | Electron 播放协议、Range 转发和过期直链刷新 |
| `src/utils/history.js` | SQLite 下载历史与已落盘文件索引 |

源码入口：[应用初始化][main]、[preload][preload]、[IPC 契约][ipc-contract]、[搜索 IPC][ipc-search]。

### 对外服务与出站客户端

桌面页面主要调用 IPC，没有要求先启动一个 REST 音乐服务。出站请求主要由 Node 的 `http`／`https` 和两套平台 SDK 完成；本项目不靠 curl 实现这些请求。

仓库另外实现了可选 MCP HTTP 服务：启用后监听 `127.0.0.1`，默认端口 `39217`，通过 `POST /mcp` 接收 JSON-RPC，并检查 Bearer token、Origin 和速率。它是外部工具接入桌面业务的一条通道，不能据此把普通搜索链路理解为 REST 调用。[MCP 服务][mcp-server]、[MCP 启停接线][mcp-ipc]

我们的 C++ 方案仍然需要 HTTP Server 对外提供接口、HTTP Client 请求平台。可以复用 Lanyue 的业务分层，把 IPC 入口替换为 HTTP 控制器；无需复制 Electron 的通信方式。

## 统一模型与插件能力

### 歌曲和音源是两种数据

`asSong()` 保留原对象的额外字段，再统一基础字段类型；约定 `duration` 的单位为**毫秒**，单位换算由平台适配器完成。不是所有历史映射路径都显式调用 `asSong()`，但下游按这个模型处理。[歌曲模型][song-shape]

| 模型 | 常见字段 | 作用 |
| --- | --- | --- |
| 歌曲 | `id`、`source`、`title`、`artist`、`album`、`cover`、`duration` | 搜索展示、集合曲目、待下载对象 |
| 平台附加信息 | `albumMid`、QQ 的 `numId`／`pay`、网易云 `fee` 等 | 详情跳转、权限提示、匹配策略 |
| 音源成功结果 | `url`、`ext`，可选 `br`、`size`、`requestedBr`、`referer`、`via` | 真正传输前使用的地址及属性 |
| 音源错误结果 | `error`、`code`、`fatal`，可能补充 `status`／错误分类 | 展示失败、判断重试与换源 |
| 下载任务 | 歌曲字段加 `taskId`、`quality`、`status`、`progress`、`addedAt`、`savePath` | 队列状态和持久化 |

音源服务会再经过共享 DTO 的归一化；不能假定每个平台都提供码率、体积或有效期。[共享 DTO][dto]、[音源服务][resolve]

同一平台 ID 与跨平台 ID 要分开理解：网易云是数值 ID，QQ 主要使用 songmid，B 站使用 bvid，酷狗把普通／无损／高品质 hash 拼进 ID，5sing 则把歌曲编号和作品类型拼在一起。因此去重键至少应包含 `(source, id)`。

### 注册表怎样发现平台

`loadPlatformPlugins()` 扫描 `platforms/` 中的 `.js` 文件，读取导出的 manifest，校验后按 `policies.order` 和平台 ID 排序注册。它要求平台提供 `id`、`name`、`search()`、`getUrl()`、合法来源域名与顺序策略。[插件注册表][registry]

可选能力由方法是否存在推导，例如：

```text
searchAlbum       → album
getAlbumSongs     → albumSongs
searchSinger      → singer
getPlaylistSongs  → playlistSongs
getTopList        → topList
getCategoryPlaylists → categoryPlaylists
verifyCookie      → cookie
```

manifest 还包含显示信息、来源域名、分享链接正则和聚合策略。前端的平台下拉、能力列表、允许的来源、探测平台和换源候选由注册表派生，减少在多个文件分别维护平台名单。

这是随应用打包的模块自动发现，并非任意用户上传 JS 后就可以安全热插拔的插件平台；新增平台涉及协议实现、构建与测试，首页具体区块仍有独立配置。

### 平台能力矩阵

“专辑”列包含搜索与曲目获取；“歌手”列包含搜索、歌曲与专辑。推荐歌单与歌单曲目是独立能力。

| 平台 | 单曲搜索／取流 | 专辑 | 歌手 | 推荐歌单 | 歌单曲目 | 分类歌单 | 榜单曲目 | Cookie 校验 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 网易云 | 有 | 有 | 有 | 有 | 有 | 未声明 | 有 | 有 |
| QQ 音乐 | 有 | 有 | 有 | 有 | 有 | 有 | 有 | 有 |
| Bilibili | 有，视频搜索与独立音轨 | 未声明 | 未声明 | 未声明 | 未声明 | 未声明 | 有，`ranking` | 有 |
| 酷狗 | 有 | 有 | 有 | 有 | **未声明** | 未声明 | 有 | 未声明 |
| 酷我 | 有 | 未声明 | 未声明 | 未声明 | 未声明 | 未声明 | 未声明 | 未声明 |
| 咪咕 | 有 | 未声明 | 未声明 | 未声明 | 未声明 | 未声明 | 未声明 | 未声明 |
| 5sing | 有 | 未声明 | 未声明 | 未声明 | 未声明 | 未声明 | 未声明 | 未声明 |
| 汽水 | 有 | 未声明 | 未声明 | 未声明 | 未声明 | 未声明 | 未声明 | 未声明 |

依据各平台 manifest：[网易云][netease]、[QQ][qq]、[B 站][bilibili]、[酷狗][kugou]、[酷我][kuwo]、[咪咕][migu]、[5sing][fivesing]、[汽水][soda]。B 站的 `ranking` 与音乐平台的 `topList` 是不同能力。酷狗能返回推荐歌单卡片，但该提交没有接入对应的歌单曲目方法，不能因此标为完整歌单支持。

### 网关承担的公共职责

`createPlatformGateway()` 集中处理平台存在性、可选能力、Cookie 注入、异常日志和缺省返回。歌曲搜索失败常被收敛为 `[]`；专辑搜索缺省返回空集合及总数；取流则保留结构化错误供策略判断。[平台网关][gateway]

网易云等路径将 Cookie 作为调用参数传入；应用还会通过 `qqMusic.setCookie()` 更新 QQ SDK 的全局状态。进一步核验锁定的 SDK 发布包后，可以确认 `api()` 通过 `ownCookie=1` 将该 Cookie 传给请求器；Lanyue 网关中否定此路径的注释不准确。迁移为多用户服务时，应将每条请求的凭证显式隔离，避免依赖全局单例。[Cookie 组装][facade]、[SDK 核验](npm-sdks.md)

## 单曲搜索与多平台聚合

### 一次搜索经过哪些函数

```text
renderer/views/search.js：doSearchByType('song', page, keyword, source)
  → musicAPI.searchMusic(keyword, source, page)
  → preload：ipcRenderer.invoke('search-music', ...)
  → main/ipc/search.js
  → api/index.js：searchMusic()
  → gateway.search(platformId, keyword, page)
  → platforms/<平台>.js：search()
  → 平台响应转换为统一歌曲数组
```

依据：[前端搜索][ui-search]、[搜索 IPC][ipc-search]、[搜索门面][facade-search]。

### 单平台搜索

指定平台时检查 ID 是否已注册，再把关键词、页码和 Cookie 交给适配器。各平台分页大小不同，例如网易云、QQ、酷狗、酷我通常请求 30 条，B 站、咪咕与汽水使用 20 条量级；应用没有把它们改造成完全相同的上游分页协议。

空白关键词会返回明确错误；未知平台也返回错误。平台实际请求异常则可能被网关收敛为空数组。因此“没有结果”与“平台故障”没有始终在普通搜索响应中区分开。

### 全平台搜索

`source === 'all'` 时，代码执行：

```text
registry.getAll()
  → fanOut(全部平台, id => gateway.search(id, keyword, 1))
  → 每个平台按 aggregateLimit 截断
  → 按注册顺序 flatMap 合并
  → { songs, source: 'all', error: null }
```

| 顺序 | 平台 | 聚合时最多保留 |
| --- | --- | --- |
| 10 | 网易云 | 10 |
| 20 | QQ 音乐 | 10 |
| 30 | Bilibili | 5 |
| 40 | 酷狗 | 5 |
| 50 | 酷我 | 5 |
| 60 | 咪咕 | 5 |
| 70 | 5sing | 5 |
| 80 | 汽水 | 5 |

所以当前配置一次聚合最多保留 **50 条**，这是上限而非保证值。全平台歌曲搜索固定取每个平台**第 1 页**，不转发传入的 `page`；不能把下一页理解为跨平台全量搜索的下一页。[聚合实现][facade-search]

合并阶段没有做跨平台同曲去重，也没有统一相关度排序。平台内部结果顺序可以来自上游排名，整体顺序来自平台顺序；它并非搜索一个共享音乐数据库后统一打分。

### 并发与错误隔离

`fanOut()` 启动最多 4 个异步 worker，共享一个递增下标。每个 worker 等待一个平台调用结束后领取下一个；结果写回原下标，因此完成顺序不会打乱平台顺序。[并发实现][gateway-fanout]

这里的 worker 是异步函数，不是四个操作系统线程。网络等待主要由 Node 异步 I/O 处理。它能隔离单个平台抛错，但有三个边界：

- 限制是每次 `fanOut()` 独立计算；多个前端搜索同时发起时，总在途请求会增加。
- 调用方等待整批结束才合并返回；没有逐个平台向搜索页面推送结果。
- `gateway.search()` 已吞掉的异常会成为正常的空数组；`fanOut` 的 `ok: true` 只证明函数正常返回，不证明该平台请求成功。

普通 HTTP 请求还有自己的超时和重试，因此整批延迟受慢平台、等待后续 worker 和重试影响，不能简单视为一次固定超时。

### 前端缓存与过期结果

`searchCache.js` 按“搜索类型、平台、关键词、页码”缓存，容量 20，TTL 为 5 分钟；达到上限时删除 Map 中最早的键。读取不会刷新键的位置，所以严格说是带过期时间的插入顺序淘汰，不是完整 LRU。[搜索缓存][search-cache]

搜索页面用请求序号防止先发出的慢响应覆盖新搜索。旧响应仍可能写入缓存，但不会覆盖当前列表。这不等同于取消上游请求，旧搜索仍会消耗网络与解析资源。[页面结果处理][ui-search]

可选自然语言搜索在普通搜索之前增加关键词改写，再合并搜索结果；它没有取代各平台适配器，属于额外业务编排。[自然语言搜索服务][nl-search]

## 八个平台如何搜索和获取音源

下面列的是**该提交实际调用的接口或 SDK 方法**，并不表示这些接口是官方承诺稳定的开放 API。

| 平台 | 搜索入口 | 音源获取入口 | 关键行为 |
| --- | --- | --- | --- |
| 网易云 | NPM SDK `ncm.search()` | `ncm.song_url()` | Cookie、音质码率链、空地址权限错误 |
| QQ | SDK `safeQQApi('search', …)` | POST `u.y.qq.com/cgi-bin/musicu.fcg` | vkey 请求、文件名与 `sip + purl` 拼接 |
| Bilibili | `api.bilibili.com/x/web-interface/search/type` | `view` → `x/player/playurl` → DASH audio | 搜索视频，直接取独立音轨 |
| 酷狗 | `songsearch.kugou.com/song_search_v2` | 移动端 playInfo → trackercdn 备选 | hash 音质选择、MD5 参数、品质降级 |
| 酷我 | `www.kuwo.cn/search/searchMusicBykeyWord` | 高品质第三方 relay → antiserver MP3 | relay 结果限制为酷我域并预检 |
| 咪咕 | `pd.musicapp.migu.cn/…/search_all.do` | `c.musicapp.migu.cn/strategy/listen-song/v2.3` | HEAD 读取跳转地址，实际使用标准品质参数 |
| 5sing | `search.5sing.kugou.com/home/json` | `mobileapi.5sing.kugou.com/song/getSongUrl` | 作品类型与 ID，逐档尝试主备地址 |
| 汽水 | `api.qishui.com/luna/search/track` | `music.douyin.com/qishui/share/track` | 从分享网页 `_ROUTER_DATA` 提取音频地址 |

### 网易云：协议细节委托给 SDK

`neteaseSearch()` 调用 `ncm.search({keywords, limit: 30, offset, cookie})`，取 `body.result.songs`，转换歌名、歌手、专辑、毫秒时长与 `fee` 等字段。[网易云适配器][netease]

`neteaseGetUrl()` 按请求品质构造码率链：无损为 `999000 → 320000 → 128000`，高品质为 `320000 → 128000`，标准为 `128000`。有 URL 时返回地址、扩展名、实际码率、体积和本次请求码率。

要注意，遇到明确的 `url === null` 会直接按 `fee` 返回 VIP 或版权错误；异常或没有形成该结果时才继续循环。因此它不是“所有失败都必定尝试完低品质”的无条件降级链。

本仓库没有在该适配器内重写网易云完整请求加密。实际请求构造由 `NeteaseCloudMusicApi` 提供；锁文件版本为 `4.32.0`。进一步分析发布包确认，这条默认搜索调用走 eapi，核心为 MD5 加 AES-ECB；C++ 迁移还需实现 Cookie、客户端头、JSON 与表单编码等细节。[依赖锁定][lock]、[SDK 实现拆解](npm-sdks.md)

### QQ：搜索用 SDK，取流直接组装 vkey JSON

搜索通过 `qq-music-api` 的 `search` 路由，参数包括 `key`、`pageNo`、`pageSize: 30`、`t: 0`。歌曲使用 songmid 作为 ID，另保留数值编号、专辑 mid 和付费信息，秒数转为毫秒。[QQ 适配器][qq]

取流流程是：

1. 从 Cookie 读取 QQ 账号和鉴权字段。
2. 根据品质选择文件前缀：`F000/.flac`、`M800/.mp3`、`M500/.mp3`。
3. 组装 `vkey.GetVkeyServer / CgiGetVkey` 请求，带 songmid、filename、guid、uin 等参数。
4. 以 **JSON 请求体** POST 到 `musicu.fcg`，设置 Referer 和 Cookie。
5. 从 `midurlinfo` 找 `purl`，从 `sip` 选 CDN 前缀，拼成最终 URL。
6. 区分回包结构异常、鉴权过期、会员限制等情况。

`qqGetUrl()` 本身没有像酷狗那样在失败后递归尝试所有较低品质；统一品质名称也不代表各平台有相同降级行为。[QQ 取流][qq-url]

取流路径自己设置 Cookie 请求头，与 `safeQQApi()` 调用 SDK 的路径不同。锁定的 SDK 客户端也会发送其保存的 Cookie；发送凭证与拥有某个资源权限是两件事，私有／会员集合仍应分别验证。

### Bilibili：搜索视频，再获取 DASH 独立音轨

搜索指定 `search_type=video`，去除标题里的 HTML，高层将 UP 主写入 `artist`，将视频时长转换为毫秒。这一字段不一定是真正的演唱者。[B 站适配器][bilibili]

取流先通过 bvid 调用 `view` 得到 cid、aid，再请求 `x/player/playurl` 的 DASH 信息。对 `dash.audio` 按 bandwidth 排序：标准品质取低带宽项，其他品质取高带宽项，返回音轨 URL、`m4a` 扩展名和 B 站 Referer。

这里直接保存上游独立音轨，并不是先下载完整视频再用 FFmpeg 抽音频。缺少独立音轨时返回登录或无音轨错误，没有通用视频解复用回退；“lossless”选择也不等于平台必定返回无损编码。

B 站模块另有 WBI 辅助方法用于相关请求，不能因此把所有请求概括为都走 WBI；这里 `bilibiliGetUrl()` 调用的是代码中直接组装的 `x/player/playurl` 路径。

### 酷狗：一个歌曲 ID 携带三套 hash

搜索请求 `song_search_v2`，读取 `FileHash`、`SQFileHash`、`HQFileHash`，将 ID 编码为 `普通hash::无损hash::高品质hash`。下载时拆开，优先选择请求品质对应的 hash。[酷狗适配器][kugou]

取流先调用移动端 `getSongInfo.php?cmd=playInfo`，没有地址再尝试 trackercdn；后者包含基于 hash 和固定字符串计算的 MD5 key。无损仍没有地址时降为高品质，再降为标准。

这是同平台内的接口备选和品质降级，与“到 QQ 搜索另一首同名歌”不同。其 `ext` 主要依据请求品质生成；当高品质 hash 缺失而回退到普通 hash 时，名称与真实容器可能不一致。C++ 实现应检查文件内容，而不只相信请求品质或后缀。

### 酷我：高品质路径存在第三方解析依赖

搜索走酷我域的 `searchMusicBykeyWord`，将页码转成从 0 开始的 `pn`，转换 `MUSICRID`、歌名、歌手与时长。[酷我适配器][kuwo]

当品质为 `lossless` 或 `hq` 时，先请求 `kw-api.cenguigui.cn` 的歌曲解析；接受前检查返回 URL 是否属于 `kuwo.cn` 域，再用 `testAudioLink()` 预检，成功结果标记 `via: 'relay:cenguigui'`。

relay 未成功时，或原本请求标准品质时，再通过酷我 `antiserver.kuwo.cn/anti.s` 获取 MP3 地址并预检。它是“第三方服务辅助拿到平台 CDN URL”，第三方参与地址解析，不代表音频文件一定从该第三方域名传输。

因此如果我们要求运行时只访问音乐平台，不能原样复制这一高品质路径；应把 relay 独立为可选解析后端，并明确其依赖与来源。

### 咪咕：搜索与音源接口分开，实际取标准品质

搜索使用客户端搜索接口，`searchSwitch` 只启用歌曲等指定结果，页大小为 20；统一 ID 使用 `contentId`。搜索没有可靠时长时写 `duration: 0`。[咪咕适配器][migu]

音源请求对 `listen-song/v2.3` 发 HEAD，读取响应 `Location`，然后预检跳转地址。代码虽定义 `QUALITY_TONE`，实际请求参数仍使用固定 `TONE_FLAG='PQ'`、`RESOURCE_TYPE='2'`；不能因存在 SQ/HQ 映射就宣称实现了对应音质下载。

### 5sing：作品类型参与取流，主备地址逐个预检

搜索结果包含作品编号与类型，统一 ID 形如 `songId_type`，下载时拆出两者交给 `song/getSongUrl`。[5sing 适配器][fivesing]

响应中的地址按 `sq`、`hq`、`lq` 区分品质。根据请求档位确定起点，逐档尝试 `${q}url` 与 `${q}url_backup`，对候选做音频预检，返回第一个可用地址；因此同时包含品质降级和 CDN 主备地址选择。

歌词另走 `song/newget` 读取 `dynamicWords`，将合法时间轴行整理成 LRC；不能把歌词接口误当作音频下载接口。

### 汽水：搜索 API 加分享网页数据提取

搜索走 `api.qishui.com/luna/search/track`，设置客户端参数、cursor、count，从 `result_groups` 里的 track 对象转换歌曲。[汽水适配器][soda]

下载请求分享页，扫描 `_ROUTER_DATA` 的 JSON 对象，读取 `loaderData.track_page.audioWithLyricsOption` 中的 URL、时长与歌词。提取逻辑处理字符串、转义和花括号深度，不是直接用一次贪婪正则截取完整 JSON。

分享数据缓存 5 分钟、最多 50 项；取流参数 `_quality` 在该实现中未用于挑选不同音质。返回地址会做预检，并按文件大小与分享时长估算码率；低于 64 kbps 的隐含码率被判为疑似片段。

这是启发式排除部分不完整资源，不是对音频完整性的证明。该实现没有呈现 Go 项目那套完整客户端签名器与加密音频处理流程，不能把“汽水有下载函数”理解为已覆盖所有整曲、品质和加密文件。

## 专辑与歌手

专辑搜索沿用歌曲相似的入口：前端 `searchAlbum` → 搜索 IPC → 门面 → 网关 → 平台。支持的平台为网易云、QQ、酷狗；全平台时仅查询具备 `album` 能力的平台。[专辑聚合][facade]、[推荐与歌手编排][recommendations]

| 平台 | 专辑搜索 | 专辑曲目 |
| --- | --- | --- |
| 网易云 | SDK `search` 的专辑类型 | SDK `album()` 后映射歌曲 |
| QQ | smartbox 专辑结果，加命中歌手的 `singer/album` | SDK `album/songs` |
| 酷狗 | 移动端专辑搜索接口 | 移动端 `album/song` |

QQ 不是简单把歌曲搜索的类型切换成专辑：它调用 `smartbox_new.fcg`，第一页接收专辑建议，同时获得歌手 mid，再按歌手查询专辑；对非录音室专辑做筛选，去重并补发布日期。因此其结果集与普通“按专辑名全文搜索”并不完全等价。[QQ 专辑实现][qq-album]

全平台专辑合并按 `mid` 去重，再按 `publishTime` 字符串倒序排列。去重键没有包含 `source`，不同平台同值 ID 存在被误合并的可能；返回 `total` 是本次合并条数，而非各平台完整搜索总数。

歌手也支持这三个平台。聚合歌手搜索用 `mid + '@' + source` 去重，比专辑聚合的标识处理更完整。旧调用未显式传平台时，部分歌手详情逻辑还会按数字／非数字 mid 猜测平台优先级；原生重构应强制保留来源，避免猜 ID。[歌手编排][recommendations]

## 歌单详情与歌单分类

### 歌单关键词搜索的边界

该提交的主要搜索类型是歌曲、专辑、歌手；在业务门面、网关能力和 IPC 契约中，没有发现与 Go 项目相当的通用“歌单关键词搜索”入口。

已有的推荐歌单、分类歌单、歌单链接识别与歌单曲目获取不能替代关键词搜索。我们仍需从 Go 项目或平台接口补齐这一能力。[搜索页面][ui-search]、[业务门面导出][facade]、[能力定义][registry]

### 歌单曲目获取

```text
getPlaylistSongs(platform, id, limit)
  → recommendations.getPlaylistSongs()
  → gateway.getPlaylistSongs()
  → 平台 manifest 中的同名方法
  → 统一歌曲数组
```

| 平台 | 实现方式 | 边界 |
| --- | --- | --- |
| 网易云 | manifest 的 `getPlaylistSongs` 指向 `neteaseGetPlaylistDetail`，调用 SDK `playlist_detail` | 对返回的 `playlist.tracks` 截断；没有在该方法中按全部 trackIds 再分批补齐 |
| QQ | `qqGetPlaylistSongs()` 调用 SDK 歌单接口，映射 songmid、歌手与专辑 | 受接口、账号权限与 limit 影响；SDK 客户端可传递所保存的 Cookie |
| 酷狗 | 有推荐歌单列表，但未导出标准 `getPlaylistSongs` | 不能由推荐卡片存在推断曲目详情也可用 |

依据：[网易云集合][netease]、[QQ 集合][qq]、[酷狗 manifest][kugou]、[集合编排][recommendations]。

批量下载通常是先取得歌曲数组，再通过队列入口逐曲排队；没有一种可绕过每首歌曲取流步骤的“歌单文件下载协议”。

### 分类实现的实际范围

网关提供 `getCategoryPlaylists` 能力，当前只有 QQ manifest 实现。首页配置预设区块与分类 ID，例如官方 `10000000`、经典 `136`、情歌 `148`、KTV `141`，调用 QQ `songlist/list` 获取列表。[首页区块][recommendations]、[QQ 分类方法][qq]

它没有实现一个跨平台分类目录 API 来动态列举所有标签、分组与热度。预设若干 QQ 分类入口与 Go 项目的“分类目录 → 分类歌单”抽象存在范围差异。

## 排行榜与首页推荐

| 平台 | 榜单实现 | 是否统一动态目录 |
| --- | --- | --- |
| 网易云 | 四个名称映射到固定歌单 ID，再复用歌单曲目方法 | 首页主要使用固定映射 |
| QQ | 调用 SDK 的榜单方法，首页取指定 topId | 当前首页主要是预设热歌榜 |
| Bilibili | `getRanking` 获取视频排行，映射为歌曲展示模型 | 视频排行，语义不同于音乐榜单 |
| 酷狗 | 名称／数字 rankId → `m.kugou.com/rank/info`，分页累积 | 模块另有 `kugouGetRankList()`，未形成全平台统一目录入口 |

酷狗榜单每页固定数量，通过循环请求多个页满足 limit，直到达到所需数量或返回空页；不是简单对一个第一页响应执行 `slice(0, 100)`。[酷狗榜单][kugou-rank]

`recommendations.js` 提供两种首页调用：旧的整体聚合方法，以及按区块名调用的 `getHomeSection()`。整体方法先并发获取网易云榜单，再并发获取其他推荐数据；它并没有统一经过歌曲搜索的 `fanOut()` 四并发门槛。[首页编排][recommendations]

因此评估后端压力时，要分别检查搜索聚合、首页批量请求和下载调度，不能把“四并发”当作整套应用统一的上游限流器。

## 分享链接解析

平台 manifest 定义 `linkPatterns`，注册表汇总后供 `parseMusicLink()` 使用，输出 `{platform, type, id}`。短链接只先识别已知短链主机，后续再处理展开。[链接解析器][link-parser]

`getSongByLink()` 对 `type === 'song'` 调用 `gateway.getSongDetail()`；歌单、专辑、歌手先返回识别结果，再由页面进入相应详情流程。链接识别与详情可用性是两个阶段，匹配到正则不代表该平台拥有对应详情能力。[链接业务处理][facade]

这与搜索的关系是：输入框先尝试识别链接，需要时进入详情；普通文本才按关键词搜索。它不是把分享链接作为关键词交给全部平台搜索。

## 下载队列与状态管理

### 从点击下载到完成

```text
页面构造歌曲下载 DTO
  → add-to-queue / add-playlist-to-queue
  → 队列去重与已下载文件检查
  → pending
  → processQueue：检查总并发和单平台并发
  → downloading
  → 质量策略：允许／降级／跳过
  → getDownloadUrlSmart：解析本源音频 URL
  → 规划目录与文件名，检查目录权限及磁盘空间
  → downloadFileWithRetry：流式写入 .tmp 并落成正式文件
  → 获取歌词，嵌入标签，写 .lrc
  → done，保存路径与历史；失败则 error
```

依据：[入队载荷][enqueue-payload]、[队列 IPC][queue-ipc]、[任务执行][queue]、[品质策略][quality-policy]。

### 并发额度

| 配置 | 缺省值 | 约束 |
| --- | --- | --- |
| `concurrency` | 3 | 合法范围 1～10 |
| `perSourceConcurrency` | 2 | 合法范围 1～10，且不超过全局并发 |
| `maxAttempts` | 2 | 合法范围 1～5，**包含首次尝试** |
| 进度事件节流 | 120 ms | 终点进度不按普通间隔过滤 |
| 已完成任务保留 | 200 项 | 避免下载队列不断累积完成记录 |

`pickSchedulable()` 按队列顺序找满足平台额度的 pending 任务，避免某个平台占满全部任务槽。`_processing` 用于标识异步任务仍在执行，防止重试等待阶段再次被当成新任务启动。

单平台统计按**任务原始 `source` 且状态为 downloading**计算；它不是独立覆盖每个 HTTP 请求的信号量，重试等待和假设启用后的换源仍需另行核对限流口径。当前跨源关闭，不能据此声称已实现按实际下载源统一限流。[队列调度][queue]

### 去重、暂停与取消

入队先检查相同 `id + source` 且非 done 的任务；这也会命中 error 项，因此已有失败任务通常应走重试入口。随后查询历史并检查文件是否存在，成功下载过且文件仍在时返回 `alreadyDownloaded`；显式强制重下可跳过这层检查。[队列 IPC][queue-ipc]、[历史索引][history]

暂停只阻止启动新任务，不打断正在传输的任务。取消则设置 `_cancelRequested`，通过 task token 找到在途 HTTP request 并 destroy；未启动的任务直接移出队列。移除和清空在途任务也会先触发取消，减少孤儿下载。

取消并不意味着任何时刻都能撤销已经完成的副作用：例如文件刚落盘后才检测到取消，代码可能保留文件而停止后续标签、历史和通知处理。应按完成阶段解释取消结果。[任务阶段检查][queue]、[传输取消][downloader]

### 持久化和重启恢复

队列保存到用户数据目录的 `queue.json`，普通更新合并为约 500 ms 一次写入，终态等路径可立即写，使用原子 JSON 文件工具。损坏文件按工具约定备份处理，恢复时校验任务并限制数量。[队列持久化][queue]、[原子文件工具][atomic-file]

恢复规则如下：

| 原状态 | 重启后的行为 |
| --- | --- |
| `pending` 且未超时 | 仍为 pending，初始化后调用 `resumeRestoredQueue()` 调度 |
| `pending` 排队超过 24 小时 | 转为 error，提示可重试 |
| `downloading` | 转为 error，提示应用异常关闭 |
| `done`／`error` | 保留相应终态信息 |

因此“自动恢复待下载队列”成立，“重启后所有正在下载的文件自动接着上次字节位置下载”不成立。传输器的本次调用内续传是下一节的独立机制。[恢复函数][queue-restore]、[恢复接线测试][test-resume]

下载历史使用 `node:sqlite` 的 `DatabaseSync`，保存成功／失败信息，另维护已落盘资产索引。文件检查有异步版本，但 SQL 操作本身并未因此变成异步数据库访问；不能把整个主进程概括为完全没有阻塞操作。[历史模块][history]

## 流式下载与断点续传

### API 请求与文件下载分开

普通 `request.js` 主要用于 JSON／文本响应，将响应体收集后解析 JSON，失败时保留文本。它处理超时、最多 5 次重定向、部分网络错误／429／5xx 重试和 AbortSignal，并通过 HTTP 状态上下文帮助网关保留错误信息。[API 请求器][request]

整首音频使用独立下载器，不能把“API 请求器可用”当作“大文件下载已正确实现”。下载器使用：

```text
Node HTTP IncomingMessage
  → 可选限速 Transform
  → fs.WriteStream(savePath + '.tmp')
  → 完成且长度检查通过
  → rename(savePath + '.tmp', savePath)
```

这避免把整首音频收集到一个大 Buffer。Node stream 的 `pipe()` 提供流量背压；封面等小资源另有 Buffer 下载路径。[流式下载器][downloader-transfer]

### 状态检查与下载进度

下载器接受 200／206；拒绝 HTML 内容类型，以减少把 CDN 错误页面保存为音频。`Content-Length` 存在时计算总长度、已接收字节和百分比，并对短响应报错；长度未知时不能提供同等准确的进度。

任务层以节流后的进度事件更新速度与剩余时间估计。限速设置转换成每秒字节数，交给每条下载流；它不是一个对所有下载共享的全局带宽令牌桶。

拒绝 HTML 只是基础检查，无法证明响应就是完整、格式正确的音频。该路径没有对每一种容器执行完整解码验证。[下载实现][downloader-transfer]

### 两层重试解决不同问题

| 层次 | 触发条件 | 行为 |
| --- | --- | --- |
| 传输层 `downloadFileWithRetry` | 识别出的瞬时网络故障等 | 同一 URL 重试；从 `.tmp` 大小计算 Range 偏移；默认额外最多重试 2 次 |
| 任务层 `processOneSong` | 下载错误文本匹配 HTTP 403／404／410 | 等待约 500 ms，再走一次解析与下载流程，以获得新地址；总次数由 `maxAttempts` 控制 |

所以 `MAX_RETRY=2` 的队列常量与传输器 `maxRetry=2` 的含义不同：前者是包含首次的最大尝试数，后者是额外重试次数。[任务重试][queue]、[传输重试][downloader-retry]

### Range 续传的具体规则

1. 初次请求从偏移 0 开始。
2. 瞬时故障后保留临时文件，重试读取它的大小，设置 `Range: bytes=N-`。
3. 返回 206 且偏移大于 0 时追加写入。
4. 返回 200 表示上游重发整个文件，改为覆盖写，避免“旧前缀 + 整文件”损坏。
5. 取消或重试耗尽后清理临时文件。

该偏移只在**这一次 `downloadFileWithRetry()` 调用内部**保存。新一次调用从 0 开始，不会自动根据上一次进程残留的 `.tmp` 恢复。[Range 与重试][downloader-transfer]、[下载器测试][test-downloader]

当前代码主要依据 206、偏移与长度处理，未见 ETag／If-Range 资源身份校验和严格的 Content-Range 起点核对。重定向时还会清理临时文件并沿用 options，续传遇到重定向的组合需要额外验证。它是可参考的实现，不能直接作为所有 CDN 场景下可靠续传的证明。

### 重定向与凭证

API 请求器和下载器分别实现重定向处理，都会检查目标并限制次数；跨主机／域族时有凭证头清理逻辑。URL 防护还会检查公网 HTTP(S) 地址并把 DNS 解析结果绑定到连接，避免“检查的是一个地址、真正连到另一个地址”的问题。[请求器][request]、[下载器][downloader]、[URL 校验][url-guard]

这些分支并非完全同一套策略。若 C++ 抽象统一 HTTP 传输，应集中定义重定向、证书、凭证清理、超时与取消，而不是为 API、封面、探测、播放和下载各写一个略有差别的版本。

## 音频标签歌词与格式处理

### 普通下载直接保存上游编码

下载任务取得 URL 后直接保存文件，不会默认将所有音源转换成 MP3。QQ 无损、B 站 M4A、平台返回的 FLAC 等可保持各自容器；扩展名主要来自取流结果或响应头推断。

| 操作 | 实现 | 是否需要 FFmpeg |
| --- | --- | --- |
| 音频下载 | HTTP 流写盘 | 不需要 |
| MP3 标签 | `node-id3.update()` | 不需要 |
| M4A／FLAC 等标签 | 调用 `scripts/write_tags.py`，依赖 Python Mutagen | 不需要，但有 Python 环境依赖 |
| 独立歌词 | 写同名 `.lrc` | 不需要 |
| 格式转换、裁剪等附加功能 | `audioConvert.js` 查找并启动 FFmpeg | 需要该外部工具 |
| 附加音频探测 | `audioProbe.js` 中有 ffprobe 路径处理 | 与普通下载传输分开评估 |

依据：[标签入口][downloader-tags]、[Python 标签脚本][write-tags]、[音频转换][audio-convert]、[音频探测][audio-probe]。

### 标签与歌词的处理顺序

下载成功后获取歌词，给音频写入标题、歌手、专辑、封面与歌词，再写独立 LRC。MP3 使用 node-id3，歌词写入非同步歌词标签；封面通过 Buffer 获取后写入图片标签。

其他格式尝试检测 `py`／`python3`／`python` 是否带 Mutagen，再运行仓库 Python 脚本。M4A 使用 MP4 标签，FLAC 使用 Vorbis Comments 和 Picture；其他格式走通用分支，字段覆盖与封面支持并不与专门分支完全一致。

找不到 Python、脚本或标签写入失败通常记录日志并保留音频，不会一概把音频下载判为失败。`done` 因此不保证所有格式都成功写入封面和歌词。[标签写入][downloader-tags]、[任务完成处理][queue]

歌词网关统一返回 `{lrc, karaoke?}`；酷狗另有 KRC 解码与按标题找歌词的能力。歌词失败通常不阻止音频下载。[歌词网关][gateway]、[KRC 编解码][krc]

### 对轻量 C++ 实现的启发

可以把“保存原始音频”“读取／写入标签”“解复用”“转码”拆为四类能力。Lanyue 证明应用结构允许普通下载与 FFmpeg 转码解耦，但它用 Node、Electron、SDK 和可选 Python 组成，不能据此推断整个产品依赖很小。

我们的首版可以直接保存音频，使用选定的 C++ 元数据方案处理限定格式，额外转码作为可选能力；具体方案仍按[技术栈文档](../../design/cpp20-refactor/technology-stack.md)验证。迁移时无需为了复用这个设计而引入 Python。

## 在线播放与直链刷新

播放取流与下载共享音源解析入口，但实际传输不同。`proxy-play` 校验上游 URL 后，将它与 Referer、可选刷新回调登记到 `streamRegistry`，给前端返回 `lanyue-stream://…` 地址。[播放 IPC][stream-ipc]、[播放登记][stream-registry]

Electron 自定义协议处理器收到播放器请求后：

1. 根据令牌找回上游元信息。
2. 校验并规范化单段 Range，将合法请求转发到上游。
3. 跟随重定向、保留所需 Referer，按流返回响应。
4. 上游返回 206 时传递 Content-Range 等信息；返回 200 时按全量响应处理。
5. 上游返回 403 时尝试刷新 URL，沿用原 Range 重放，支持 seek 后重新取流；其他错误状态不会一律触发刷新。

依据：[播放协议][stream-protocol]、[刷新回调][stream-refresh]。

这种设计避免必须完整下载一首歌才能开始播放，并把浏览器不便设置的请求头放在主进程处理。自定义协议是 Electron 能力；我们的 C++ HTTP Server 若提供播放代理，需要自行实现对应 HTTP 路由、Range、响应流和取消，不能直接移植这个 scheme 注册调用。

## 换源匹配与当前关闭的路径

### 先区分三种回退

| 回退 | 示例 | 当前情况 |
| --- | --- | --- |
| 同平台品质降级 | 酷狗无损失败改高品质、标准 | 平台适配器中存在，照常可执行 |
| 同平台接口／地址备选 | 酷狗 playInfo 改 trackercdn；5sing 主地址改备用地址 | 平台适配器中存在 |
| 跨平台找另一条歌曲记录 | 网易云失败后搜索 QQ 同名同歌手 | 机制保留，生产总开关关闭 |

### 为什么“智能取流”不代表正在跨平台换源

`api/index.js` 创建 `ResolveTrackService` 时注入 `fallbackEnabled: crossSourceEnabled`。分析提交中这个函数固定返回 `false`。[生产接线][facade]、[开关源码][fallback-policy]

因此当前主要路径是：

```text
normalizeSong
  → 尝试原平台 getUrl
  → 基于体积、码率和声明时长识别明显试听片段
  → 成功返回，失败返回原平台错误
```

以前记住的 `_altSource` 与跨源候选搜索也被此开关挡住。设置里的“不参与换源的平台”只是候选排除清单，不会自动把总开关打开。单纯看到函数名 `getDownloadUrlSmart()`、测试覆盖了换源，或 README 描述智能换源，都不足以证明当前产品已启用它。

### 若重新启用，完整策略是什么

保留的实现顺序为：[音源服务][resolve]

1. 原平台未配置 Cookie、存在可用 `_altSource` 记忆且候选未被排除时，先尝试记住的替代源。
2. 尝试原平台，成功就返回。
3. 仅对允许换源的错误码／HTTP 状态寻找候选。
4. 搜索其他允许成为换源来源的平台，按标题、歌手和时长匹配。
5. 过滤用户排除平台，按健康度重排候选。
6. 逐个解析候选地址，执行试听判断与可选直链预检。
7. 返回 `matchedSong`、`matchedFrom` 等真实来源信息，全部失败则返回原平台错误。

允许尝试换源的情况包括会员、登录、鉴权过期、版权、资源移除、无音轨、CDN 空和平台接口变更等；普通网络中断不在该错误码白名单中。HTTP 403／404／410 也有对应判断。[换源错误码][fallback-codes]

### 候选怎样匹配

`matchMusic.js` 的算法不是音频指纹识别，也不是调用语言模型匹配。[歌曲匹配][match]

- 标题转小写、全角转半角、去掉非字母数字字符，再去除部分版本尾词。
- 歌手按常见分隔符拆分、归一化并排序。
- 标题不同直接 0 分；标题相同且歌手一致、时长差不超过 5 秒给 3 分；只有歌手一致给 2 分。
- 双方歌手都有值但不一致，只给 1 分；缺少歌手时，时长接近可给 2 分。
- 保留至少 2 分的候选，对未登录的付费候选略微降权，再按时长差排序，最终截断到前 5 项。

源码该截断处注释写“每源最多”，但调用方先把多平台结果摊平成一个数组再过滤，所以当前执行是**所有来源合计最多 5 个候选**。

候选搜索使用 `Promise.allSettled` 和 15 秒 Promise 超时竞争，没有复用网关四并发聚合器。相同匹配请求会共享在途 Promise；正结果缓存 10 分钟，负结果缓存 1 分钟，最多 200 项。超时竞争只使等待结束，不保证底层 HTTP 已被取消。

B 站 `fallbackSource: false`，因为 UP 主等元数据不适合直接当作歌曲歌手参与候选匹配；这表示它不作为目标候选来源，不能简单理解为 B 站原始歌曲永远不能触发到其他平台的匹配。

匹配时会剥离 Live、Remix、Cover 等尾词，仍可能把不同版本合在一起。C++ 重构应将版本信息作为约束保留，不能直接把这种文本命中视为相同录音。

### 健康度与直链探测

`sourceHealth.js` 保留每个平台最近 20 次取流结果，使用 `(成功数 + 1) / (样本数 + 2)` 平滑评分；无样本的平台先视为健康。主动探测另记一套窗口，仅用于展示，不进入候选健康排序。[源健康度][health]

这里的“成功”主要来自**音源解析结果**，不是整首文件下载完成或完整播放完成的统计。它与最终下载成功率不能直接等同。

`testAudioLink()` 先 HEAD，不可用或 403／405／501 时改用 `GET + Range: bytes=0-1`；读取响应头即停止响应流，检查状态、内容类型、体积并推断扩展名。[音频预检][request-probe]

候选探测对明确非音频、404、410 视为确定失败；超时或部分拒绝并不都判死。预检回答的是“这条 URL 当前是否像可用音频”，不能证明账号可持续访问、音频完整、真实无损或歌曲版本正确。

## 与 Go 项目和 C++20 重构的关系

### 两个项目各自更值得参考的部分

| 方面 | Go Music DL／music-lib | Lanyue |
| --- | --- | --- |
| 应用入口 | Web、CLI／TUI、桌面壳等 | Electron 桌面，IPC 为主，另有可选 MCP |
| 平台边界 | 应用与独立 Go 适配库分开 | 仓库内 JS 平台模块，网易云／QQ 部分依赖 NPM SDK |
| 能力表达 | Go 接口与平台工厂 | manifest、按方法推导能力、统一网关 |
| 歌单／分类研究 | 歌单关键词搜索、分类目录等更完整的参考 | 推荐与详情入口，通用歌单搜索／动态分类目录不完整 |
| 聚合搜索 | 见 Go 文档中的具体调用与排序 | 单次最多 4 个平台异步调用，歌曲聚合取第一页 |
| 下载组织 | 见 Go 下载链路与记录实现 | 主进程队列、全局／平台额度、持久化、进度与取消 |
| 标签处理 | 部分格式涉及 FFmpeg 等工具 | MP3 node-id3，其他格式可选 Mutagen |
| 跨平台换源 | 有对应手动／自动链路 | 候选与策略完整保留，但当前生产总开关关闭 |
| 汽水协议 | 客户端、签名与加密相关路径可供研究 | 搜索 API 加分享网页取流，覆盖范围不同 |

Go 一侧依据见[已有拆解](../go-music-dl/README.md)，这里的对比表示实现方式与覆盖范围，不是在线成功率排名。

### 适合迁移的设计

| Lanyue 设计 | C++20 对应建议 |
| --- | --- |
| manifest 与注册表 | 定义 `ProviderDescriptor`、能力集合与平台策略；首版显式注册 C++ 对象即可 |
| gateway | 平台调用统一出口，注入请求上下文和凭证，集中处理错误 |
| 歌曲与音源分离 | 搜索只返回歌曲元信息，播放／下载前再解析短期 URL |
| 有界聚合 | 使用独立上游工作池与每平台额度，限制多个用户合计的在途请求 |
| 下载队列 | 独立任务 ID、状态、全局与平台限额、进度、取消、持久化 |
| 传输重试与重新解析分开 | 网络恢复可重试传输，URL 失效可重新解析；资源变化时重置续传 |
| 原始流保存与转码分开 | 常规下载不要求 FFmpeg；标签、解复用、转码各自声明能力 |
| 来源追踪 | 保留原平台和实际音源平台，替代版本需要明确展示 |
| 策略依赖注入 | 对候选匹配、错误分类、恢复、Range 等使用本地可控数据验证 |

以上是本项目的迁移建议，不是 Lanyue 已存在的 C++ 类型或接口。

### 与 cpp-httplib 主方案怎么衔接

当前暂定 **cpp-httplib 主方案、Drogon 备选**。这份研究不改变选型。[重构设计入口](../../design/cpp20-refactor/README.md)

Lanyue 的 `async/await` 写法依赖 Node 异步网络 API；换成 C++20 的 `co_await` 语法不会自动让阻塞的 httplib Client 变成非阻塞。我们可以使用 httplib Server 工作线程接收请求，再由独立、限额明确的上游任务执行歌曲聚合，长下载交给专门队列。

例如多个前端搜索并发时，每个请求都可以等待自己的聚合结果；要额外控制全部请求合计的工作数、排队长度和超时。避免在同一个已被 HTTP 处理占满的线程池里提交子任务并同步等待，以免子任务无线程可执行。

服务器线程、上游调用与下载任务的并发数不应简单照抄这里的 4／3／2，而应按目标设备和业务测量。后续若这些容量需求更适合事件循环模型，再评估 Drogon；本篇没有要求先切换框架。

## 实现边界与验证重点

下面区分源码已经能确定的行为，以及仍需构造场景或在线验证的问题。

| 事项 | 已观察到的实现 | 验证或迁移时关注 |
| --- | --- | --- |
| 跨源换源 | 固定开关为 false，生产接线已注入 | 验证默认不替换音源；启用时显示真实来源与版本 |
| 歌曲聚合分页 | 全平台固定第一页，最多保留 50 条 | 设计清晰分页协议，避免前端“下一页”重复数据 |
| 平台失败信息 | 普通搜索异常常收敛为 [] | 返回每平台成功、无结果、失败及错误原因 |
| 并发范围 | 搜索按调用限 4，首页与匹配另有调度 | 加总并发、单平台额度与队列长度上限 |
| 请求取消 | 页面防旧响应覆盖不等于取消请求 | 将取消信号传到排队任务和在途 HTTP |
| 平台协议 | 两个 NPM SDK，其他直接请求／网页提取 | SDK 内协议也需研究，不能直接当作 C++ 代码复用 |
| 音质 | 多个平台允许降级或忽略请求档位 | 返回实际编码、码率、容器与降级理由 |
| 试听判断 | 根据大小、码率、时长做启发式判断 | 不应把缺少参数视为已证明完整 |
| 临时文件 | 默认固定为目标路径加 `.tmp` | 同名不同歌曲并发可能冲突，宜使用任务唯一临时路径 |
| Range | 支持 206 追加、200 覆盖 | 检查 Content-Range、ETag／If-Range、416 与重定向组合 |
| 文件完成语义 | 传输 rename 在标签之前，标签失败通常不失败任务 | 区分“音频已下载”和“元数据完整”，考虑标签原子更新 |
| 元数据与标签 | SQL、部分文件工具及 node-id3 有同步操作 | 重处理不要长期占用 HTTP 或主事件线程 |
| 下载去重 | 主要依赖平台 ID、来源和文件存在 | 需要时把品质、版本与真实资源 ID 纳入资产模型 |
| Cookie | QQ SDK 等存在全局凭证状态 | 多用户上下文隔离，避免请求间串号 |
| 第三方 relay | 酷我高品质链路明确使用外部服务 | 作为独立可选后端，验证可用性和返回内容 |

### 仓库测试能帮助读哪些边界

| 测试 | 可辅助理解的内容 |
| --- | --- |
| `test/gateway.test.js` | 能力缺省、Cookie 注入、异常隔离、聚合返回 |
| `test/fallback-policy.test.js` | 排除列表、本源不受影响、生产换源开关及接线 |
| `test/queue-resume.test.js` | 恢复 pending 后是否真正启动调度、暂停语义 |
| `test/queue-abort.test.js` | 取消与异步任务生命周期 |
| `test/downloader.test.js` | 200 正常保存、206 追加、Range 被忽略、HTML 拒绝、瞬时中断续传 |
| `test/stream-protocol.test.js` | 自定义播放协议与 Range 行为 |

本次只阅读相关测试及实现，没有执行其测试套件。测试描述同样需要对照生产接线，不能仅凭某个策略在隔离测试里成功，就认定产品默认启用了该策略。[网关测试][test-gateway]、[换源测试][test-fallback]、[恢复测试][test-resume]、[取消测试][test-abort]、[下载测试][test-downloader]、[播放测试][test-stream]

## 源码阅读顺序

1. [package.json][package]、[锁文件][lock]：先明确 Electron、Node、SDK 和标签工具边界。
2. [搜索页面][ui-search] → [preload][preload] → [IPC 契约][ipc-contract] → [搜索 IPC][ipc-search]：追一条真实用户请求。
3. [业务门面][facade] → [平台注册表][registry] → [网关][gateway]：理解能力、来源、凭证和聚合。
4. [网易云][netease]、[QQ][qq]、[酷狗][kugou]，再看其余平台：对照“SDK 委托”和“直接协议适配”。
5. [推荐编排][recommendations]：核对歌单、专辑、歌手、分类、榜单各自覆盖范围。
6. [音源服务][resolve] → [总开关][fallback-policy] → [候选匹配][match] → [健康度][health]：先看默认分支，再看保留机制。
7. [队列 IPC][queue-ipc] → [队列引擎][queue] → [传输器][downloader]：分清任务、解析、网络传输和文件完成。
8. [标签脚本][write-tags]、[历史存储][history]、[播放协议][stream-protocol]：补齐保存、播放和依赖分析。
9. 对照[Go Music DL 拆解](../go-music-dl/README.md)与[C++20 架构](../../design/cpp20-refactor/architecture.md)，形成首批平台协议与公共模块清单。

[package]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/package.json
[lock]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/package-lock.json
[main]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/main/index.js
[preload]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/main/preload.js
[ipc-contract]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/shared/ipcContract.js
[ipc-search]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/main/ipc/search.js
[facade]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/api/index.js
[facade-search]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/api/index.js#L73
[registry]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/api/pluginRegistry.js
[gateway]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/api/gateway.js
[gateway-fanout]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/api/gateway.js#L395
[song-shape]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/api/songShape.js
[dto]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/shared/dto.js
[ui-search]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/renderer/js/views/search.js#L211
[search-cache]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/renderer/js/searchCache.js
[nl-search]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/api/services/nlSearchService.js
[netease]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/api/platforms/netease.js
[qq]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/api/platforms/qq.js
[qq-url]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/api/platforms/qq.js#L373
[qq-album]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/api/platforms/qq.js#L240
[bilibili]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/api/platforms/bilibili.js
[kugou]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/api/platforms/kugou.js
[kugou-rank]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/api/platforms/kugou.js#L443
[kuwo]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/api/platforms/kuwo.js
[migu]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/api/platforms/migu.js
[fivesing]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/api/platforms/fivesing.js
[soda]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/api/platforms/soda.js
[recommendations]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/api/recommendations.js
[link-parser]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/utils/linkParser.js
[resolve]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/api/services/resolveTrackService.js
[fallback-policy]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/api/services/fallbackPolicy.js#L30
[fallback-codes]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/api/services/fallbackCodes.js
[match]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/utils/matchMusic.js
[health]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/utils/sourceHealth.js
[request]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/api/request.js
[request-probe]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/api/request.js#L396
[enqueue-payload]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/renderer/js/enqueuePayload.js
[queue-ipc]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/main/ipc/download/queue.js
[queue]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/main/downloadQueue.js
[queue-restore]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/main/downloadQueue.js#L313
[quality-policy]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/utils/qualityPolicy.js
[downloader]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/utils/downloader.js
[downloader-transfer]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/utils/downloader.js#L318
[downloader-retry]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/utils/downloader.js#L646
[downloader-tags]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/utils/downloader.js#L568
[write-tags]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/scripts/write_tags.py
[audio-convert]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/utils/audioConvert.js
[audio-probe]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/utils/audioProbe.js
[krc]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/utils/krcCodec.js
[url-guard]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/utils/urlGuard.js
[atomic-file]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/utils/atomicFile.js
[history]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/utils/history.js
[stream-ipc]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/main/ipc/download/stream.js
[stream-registry]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/main/streamRegistry.js
[stream-protocol]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/main/streamProtocol.js
[stream-refresh]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/main/streamRefresh.js
[mcp-server]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/main/mcp/mcpServer.js
[mcp-ipc]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/main/ipc/mcp.js
[test-gateway]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/test/gateway.test.js
[test-fallback]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/test/fallback-policy.test.js
[test-resume]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/test/queue-resume.test.js
[test-abort]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/test/queue-abort.test.js
[test-downloader]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/test/downloader.test.js
[test-stream]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/test/stream-protocol.test.js
