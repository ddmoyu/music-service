# Go Music DL 功能实现拆解

Go Music DL 通过平台适配器，把不同音乐平台的搜索、歌单、专辑、榜单和音频获取能力转换成统一模型，再在应用层完成并发聚合、页面展示、资源探测、下载和跨平台换源。

理解这个项目需要同时阅读两个仓库：`go-music-dl` 负责业务编排与界面，`music-lib` 负责音乐平台接口适配。只看应用仓库，能看到功能入口，但看不到多数上游接口的请求和解析细节。

## 分析范围与版本

| 项目 | 分析基准 |
| --- | --- |
| 整理日期 | 2026-10-19 |
| 应用仓库 | [guohuiyuan/go-music-dl](https://github.com/guohuiyuan/go-music-dl) |
| 应用提交 | `ddf2a4e550ca67086ff1f06edc5d15c7d4c8663d` |
| 适配库仓库 | [guohuiyuan/music-lib](https://github.com/guohuiyuan/music-lib) |
| 应用声明的适配库版本 | `v1.1.1-0.20261018010056-7cc4467b4e30` |
| 对应适配库提交 | `7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec` |
| 主要范围 | 单曲搜索、歌单搜索、专辑搜索、歌单分类、排行榜、下载、手动和自动换源 |

本文以源码静态分析为依据。平台支持表表示应用已暴露相应调用入口，不代表每个接口在任意账号、地区和时间均可成功；登录权限、上游接口变化和试听限制会影响实际结果。文中的改进方向属于基于当前实现的设计建议。

版本依据：[应用 go.mod][app-mod]。下文源码链接均固定到上述提交，避免主分支更新后内容与分析错位。

## 阅读目录

- [整体架构](#整体架构)
- [统一模型与能力分发](#统一模型与能力分发)
- [单曲搜索](#单曲搜索)
- [歌单搜索与详情](#歌单搜索与详情)
- [专辑搜索与详情](#专辑搜索与详情)
- [分享链接解析](#分享链接解析)
- [歌单分类](#歌单分类)
- [排行榜](#排行榜)
- [音频下载](#音频下载)
- [换源匹配](#换源匹配)
- [资源探测与自动换源](#资源探测与自动换源)
- [登录与本地音乐等关联能力](#登录与本地音乐等关联能力)
- [实现边界与复用建议](#实现边界与复用建议)
- [源码阅读顺序](#源码阅读顺序)

## 整体架构

```text
Web 页面                  TUI                 桌面与移动端外壳
    │                      │                         │
    │                      │                 启动本地服务并加载页面
    └──────────────────────┼─────────────────────────┘
                           ▼
                  go-music-dl 业务层
        搜索聚合、详情展开、资源探测、换源、文件保存
                           │
                  core 中的平台方法工厂
                           │
                           ▼
                    music-lib 适配层
      netease / qq / kugou / kuwo / migu / soda / apple …
                           │
                           ▼
             平台 Web 接口、移动端接口、网页数据
```

| 目录或文件 | 主要职责 |
| --- | --- |
| `cmd/music-dl/root.go` | Cobra 命令入口，启动 TUI，读取搜索源、输出目录、封面和歌词参数 |
| `cmd/music-dl/web.go` | Web 服务命令入口 |
| `internal/web/server.go` | Gin 服务、Go HTML 模板、静态资源、页面渲染与分页 |
| `internal/web/music.go` | 搜索、歌单、专辑、分类、榜单、探测、换源和下载路由 |
| `internal/web/templates/static/js/app.js` | 页面交互、播放器、探测队列、批量操作、自动换源 |
| `internal/cli/ui.go` | Bubble Tea 交互界面、搜索、详情、下载和 TUI 换源 |
| `internal/appshell/server.go` | 启动桌面端本地服务，等待健康检查，再提供页面地址 |
| `core/service.go` | 平台方法分发、Cookie 管理、相似度算法、HTTP 请求和 Range 下载 |
| `core/download.go` | 获取完整音频、解密、格式识别、元数据处理和文件保存 |
| `core/download_record.go` | 下载记录、去重索引和文件存在性检查 |
| `core/soda_client.go` | 汽水客户端、Cookie、本机或远程签名器的统一构造 |
| `music-lib/model/song.go` | 歌曲、歌单、分类、排行榜的统一数据结构 |
| `music-lib/<平台>/` | 各平台的请求参数、签名或加密、返回值解析及下载地址获取 |

Web 页面主要由 Gin 和 Go HTML 模板生成，JavaScript 负责交互和播放器状态。桌面应用会启动本地 Web 服务再加载页面，因此这些端可以复用大量业务代码。[Web 初始化][app-server]、[桌面服务启动][app-shell]

默认路由前缀为 `/music`，可通过启动配置调整。后面的路由示例均采用默认前缀。

## 统一模型与能力分发

### 统一模型

`music-lib` 将平台返回值转换成以下结构，应用层围绕这些结构工作。[数据模型][lib-model]

| 模型 | 主要字段 | 使用场景 |
| --- | --- | --- |
| `Song` | `ID`、`Name`、`Artist`、`Album`、`AlbumID`、`Duration`、`Source`、`URL`、`Ext`、`Cover`、`Extra` | 搜索结果、歌单曲目、榜单曲目、播放和下载 |
| `Playlist` | `ID`、`Name`、`Cover`、`TrackCount`、`PlayCount`、`Creator`、`Description`、`Source`、`Link`、`Extra` | 歌单和专辑的集合信息 |
| `PlaylistCategory` | `ID`、`Name`、`Group`、`Source`、`Count`、`Hot`、`Extra` | 分类目录和分类分组 |
| `TopList` | `ID`、`Name`、`Cover`、`UpdateFrequency`、`TrackCount`、`Source`、`Extra` | 榜单目录 |

专辑复用了 `Playlist` 结构，但专辑的搜索、详情和链接解析使用独立方法。`Creator` 在专辑场景通常用于承载歌手等展示信息，不能脱离上下文统一解释为歌单创建者。

`Extra` 保存平台专有信息，例如酷狗 hash、QQ songmid、咪咕资源参数、榜单排名。它使应用可以保留下载和解析所需的数据，减少把多个字段拼进 ID 的需求。复用这些模型时，应保留 `Extra` 的传递，不能只保留歌名和 ID。

### 能力接口与应用分发

`music-lib/provider/interface.go` 定义了 `SongSearcher`、`PlaylistProvider`、`AlbumProvider`、`PlaylistCategoryProvider`、`TopListProvider` 等接口。[能力接口][lib-provider]

应用层主要通过工厂函数选择具体方法，而不是统一依赖一个全功能 Provider 对象：

```text
GetSearchFunc(source)              → Search(keyword)
GetPlaylistSearchFunc(source)      → SearchPlaylist(keyword)
GetAlbumSearchFunc(source)         → SearchAlbum(keyword)
GetPlaylistDetailFunc(source)      → GetPlaylistSongs(id)
GetAlbumDetailFunc(source)         → GetAlbumSongs(id)
GetPlaylistCategoriesFunc(source)  → GetPlaylistCategories()
GetCategoryPlaylistsFunc(source)   → GetCategoryPlaylists(id, page, limit)
GetTopListsFunc(source)            → GetTopLists()
GetTopListSongsFunc(source)        → GetTopListSongs(id)
GetDownloadFunc(source)            → GetDownloadURL(song)
```

例如 `GetSearchFunc("netease")` 读取网易云 Cookie，再返回 `netease.New(cookie).Search`。不支持的来源返回 `nil`，调用方据此跳过或显示错误。汽水走 `NewSodaClient`，额外接入签名器。[平台工厂][app-service]

### 应用暴露的平台范围

| 来源 | 单曲搜索 | 歌单搜索 | 专辑搜索 | 歌单分类 | 排行榜 |
| --- | --- | --- | --- | --- | --- |
| 网易云 `netease` | 有 | 有 | 有 | 有 | 有 |
| QQ `qq` | 有 | 有 | 有 | 有 | 有 |
| 酷狗 `kugou` | 有 | 有 | 有 | 有 | 有 |
| 酷我 `kuwo` | 有 | 有 | 有 | 有 | 有 |
| 咪咕 `migu` | 有 | 有 | 有 | 有 | 有 |
| 千千 `qianqian` | 有 | 有 | 有 | 有 | 无 |
| 汽水 `soda` | 有 | 有 | 有 | 有 | 有 |
| Apple Music `apple` | 有 | 有 | 有 | 有 | 有 |
| JOOX `joox` | 有 | 有 | 有 | 有 | 无 |
| Jamendo `jamendo` | 有 | 有 | 有 | 无 | 无 |
| Bilibili `bilibili` | 有 | 有 | 无 | 无 | 无 |
| 5sing `fivesing` | 有 | 有 | 无 | 无 | 无 |
| 本地 `local` | 应用层处理 | 本地歌单搜索 | 无 | 无 | 无 |

该表依据 `core` 中的能力工厂和来源列表。是否出现在默认搜索选项中，是另一项独立配置；单曲默认来源排除了 Bilibili、JOOX、Jamendo、5sing 和本地来源。[来源列表][app-sources]

## 单曲搜索

### Web 调用链

```text
GET /music/search?q=晴天&type=song&sources=netease&sources=qq
    → 读取关键词和来源
    → 每个远端来源启动一个 goroutine
    → core.GetSearchFunc(source)
    → 平台 Search(keyword)
    → 转换为 []model.Song
    → 补充 Source 并加锁追加到 allSongs
    → 等待所有来源完成
    → 可选精确歌手过滤
    → renderIndex 过滤并分页展示
```

并发搜索使用 `sync.WaitGroup` 等待所有平台，用互斥锁保护共享结果切片。某个平台失败时，普通搜索分支直接跳过该平台，其他结果仍能展示。[搜索路由][app-search]

这个分支没有统一的跨平台去重、相关性重排和逐平台流式返回。合并顺序受各平台完成时间影响；页面在 `wg.Wait()` 之后生成，因此慢平台也会影响整次请求耗时。换源流程单独封装了较短的等待时间，不能将其超时参数套用到普通搜索。

### 平台接口差异

| 平台 | 单曲搜索路径或方式 | 关键参数 |
| --- | --- | --- |
| 网易云 | `/api/linux/forward` 转发到 `/api/cloudsearch/pc` | `s=关键词`、`type=1`、`limit=10` |
| QQ | `c.y.qq.com/soso/fcgi-bin/search_for_qq_cp` | `w=关键词`、`p=1`、`n=10` |
| 酷狗 | `songsearch.kugou.com/song_search_v2` | `keyword`、`page=1`、`pagesize=10` 等 |
| 酷我 | `www.kuwo.cn/search/searchMusicBykeyWord` | 由适配器构造关键词和分页参数 |
| 咪咕 | `pd.musicapp.migu.cn/MIGUM2.0/v1.0/content/search_all.do` | `text` 和搜索开关 |
| 汽水 | `api.qishui.com/luna/search/track` | `q`、`cursor`、`count`，附移动端设备参数 |
| Apple Music | `/v1/catalog/{storefront}/search` | `term`、`types=songs`、`limit=30` |

接口、请求头和响应解析由平台适配器维护。应用层不直接理解各平台原始 JSON。[网易云搜索][lib-netease-song]、[QQ 搜索][lib-qq-song]、[酷狗搜索][lib-kugou-song]、[汽水搜索][lib-soda-search]、[Apple 搜索][lib-apple]

### 网易云搜索的参数加密

网易云适配器先构造包含目标地址、HTTP 方法和搜索参数的 JSON，再通过 `EncryptLinux` 加密为 `eparams`，提交到 Linux 转发接口。歌曲、专辑和歌单搜索共用 `cloudSearch`，通过 `searchType` 区分。[网易云公共请求][lib-netease-core]

代码还实现了 WeAPI 和 EAPI 所需的参数加密。搜索使用的 Linux 包装与歌单详情、下载等请求使用的包装不同，复用时需要按接口选择对应函数。[网易云加密封装][lib-netease-crypto]

### 展示过滤与分页

`renderIndex` 可以按设置过滤时长已知且不足 60 秒的歌曲，以及名称含 Live、Remix、Demo 等配置关键词的版本。过滤完成后，再对已经获取到的结果切片分页。[展示过滤与分页][app-render]

这里的页面分页不等同于上游搜索分页。以网易云为例，搜索适配器固定请求首批 10 条结果；对合并结果翻页，不会自动从每个平台请求下一批。

## 歌单搜索与详情

### 搜索与展开分离

```text
GET /music/search?q=通勤&type=playlist
    → GetPlaylistSearchFunc(source)
    → SearchPlaylist(keyword)
    → []model.Playlist

点击歌单
    → GET /music/playlist?id=歌单ID&source=平台
    → GetPlaylistDetailFunc(source)
    → GetPlaylistSongs(id)
    → []model.Song
```

搜索阶段主要获取歌单名称、封面、创建者、曲目数量、简介和链接。进入详情后才获取歌曲，因此不会为每一个搜索结果都提前展开全部曲目。[歌单搜索与详情入口][app-playlist]

### 各平台搜索方式

| 平台 | 歌单搜索实现 |
| --- | --- |
| 网易云 | `cloudSearch(keyword, 1000, 10)` |
| QQ | `client_music_search_songlist`，使用 `query`、`page_no=0`、`num_per_page=20` |
| 酷狗 | `/api/v3/search/special` |
| 酷我 | `search.kuwo.cn/r.s`，`ft=playlist` |
| 咪咕 | `search_all.do`，通过 `searchSwitch` 开启 `songlist` |
| 汽水 | `/luna/search/playlist` |
| Apple Music | Catalog Search，`types=playlists` |

千千的搜索适配器会兼容 `data` 返回对象或空数组的情况；部分上游失败响应也会转换成空结果。因此，“搜索结果为空”不总能区分没有匹配项和平台接口失败。[千千歌单搜索][lib-qianqian-playlist]

### 网易云歌单展开

网易云请求 `/weapi/v3/playlist/detail` 获取歌单信息与 `trackIds`，请求中的 `n=0` 表示主要读取 ID 列表。随后按每批 500 个 ID 请求歌曲详情，再合并为最终结果。[网易云歌单详情][lib-netease-core]

```text
歌单 ID
    → 歌单元数据和 trackIds
    → 将歌曲 ID 划分为最多 500 个一批
    → fetchSongsBatch
    → 合并 Song 列表
```

这个方案可以继续补齐歌单歌曲，而不局限于详情接口直接返回的部分曲目。但批次请求失败会被跳过，最终可能返回部分曲目。其他平台的详情接口、分页方式和数量上限各不相同，不能把网易云的批处理能力推广为所有来源的行为。

### 详情页复用

歌单展开后使用普通 `Song` 列表，所以播放、下载、批量操作、换源和导入本地歌单都可以复用单曲逻辑。详情页附带的歌单名称、来源、封面等元信息，还会被用于导入本地集合。

## 专辑搜索与详情

专辑流程与歌单相似，但方法和接口独立：

```text
type=album
    → GetAlbumSearchFunc(source)
    → SearchAlbum(keyword)
    → []model.Playlist
    → 点击专辑
    → GET /music/album?id=专辑ID&source=平台
    → GetAlbumSongs(id)
    → []model.Song
```

| 平台 | 专辑搜索实现 | 详情实现示例 |
| --- | --- | --- |
| 网易云 | `cloudSearch`，`type=10` | `/weapi/v1/album/{id}` |
| QQ | `search_for_qq_cp`，`t=8` | `musicu.fcg` 中的专辑详情和歌曲请求 |
| 酷狗 | `/api/v3/search/album` | `/api/v3/album/info` 和 `/api/v3/album/song` |
| 酷我 | 集合搜索接口，`ft=album` | 专辑接口及页面数据回退 |
| 咪咕 | `searchSwitch` 开启 `album` | 资源详情与专辑歌曲查询 |
| 汽水 | `/luna/search/album` | 平台专辑详情 |
| Apple Music | `types=albums` | Catalog 专辑资源及曲目关系 |

应用中还有 `/album_jump`：当歌曲只带专辑名称、没有可直接跳转的专辑 ID 时，在同一来源搜索专辑，再根据名称和歌手选择候选，跳转专辑详情。这个匹配与跨平台单曲换源属于不同流程。[专辑入口][app-album]、[专辑匹配][app-album-match]

## 分享链接解析

当搜索词以 `http` 开头时，Web 搜索入口进入链接解析分支：

1. `DetectSource` 根据域名等字符串识别平台。
2. 尝试 `Parse`，解析单曲。
3. 未成功时尝试 `ParsePlaylist`。
4. 仍未成功时尝试 `ParseAlbum`。
5. 根据当前搜索类型，展示集合卡片或直接展开歌曲。

平台适配器负责提取真实 ID、处理平台链接格式，以及在实现支持时解析短链接。应用还会保留集合元信息，支持把解析出的歌单或专辑导入本地集合。[链接识别与工厂][app-service]、[搜索中的解析分支][app-search]

`DetectSource` 本身主要使用字符串包含判断，完整的链接类型和 ID 校验仍依赖平台解析器。另外，Cobra 根命令中的 `--url` 分支在当前提交仍打印“开发中”提示；Web 或 TUI 输入框中的链接解析能力，不能直接等同于该 CLI 参数已经实现。[CLI 参数入口][app-cli-root]

## 歌单分类

### 两个接口组成分类能力

```text
GetPlaylistCategories()
    → []PlaylistCategory
    → 展示语种、风格、场景等分组

GetCategoryPlaylists(categoryID, page, limit)
    → []Playlist
    → 点击歌单后复用 GetPlaylistSongs
```

Web 的 `/playlist_categories` 并发加载各平台分类，把结果按来源整理，再按分类的 `Group` 字段分组。缺少分组名时使用“其他”。部分平台失败不会阻止其余平台展示，并会生成部分加载失败提示。[分类页面加载][app-categories]

点击分类后访问 `/category_playlists`，读取 `source` 和 `category_id`，通过工厂调用对应平台的方法。

### 分类数据来源并不统一

| 平台 | 分类目录来源 | 分类下歌单来源 |
| --- | --- | --- |
| 网易云 | `/weapi/playlist/catalogue` | `/weapi/playlist/list`，传 `cat`，按热门排序 |
| QQ | `fcg_get_diss_tag_conf.fcg` | `fcg_get_diss_by_tag.fcg` |
| 酷狗 | `/api/v3/tag/list` | `/api/v3/tag/specialList` |
| 酷我 | `getTagList` | 分类用 `getTagPlayList`，推荐列表用 `getRcmPlayList` |
| 千千 | `/v1/tracklist/category` | `/v1/tracklist/list`，传 `subCateId` |
| JOOX | 从歌单页面数据提取分类 | 从页面返回的分类项目中选择并分页 |
| 咪咕 | 代码内置关键词和分组 | 以分类名称作为关键词搜索歌单 |
| 汽水 | 代码内置分类及频道 ID | 按 `sub_channel_id` 请求发现页歌单流 |
| Apple Music | 代码内置编辑账号分类 ID | `/v1/catalog/cn/apple-curators/{id}/playlists` |

网易云、QQ、酷狗和千千等把上游分类结构映射为公共模型。咪咕、汽水和 Apple 的目录则含内置定义；这些方案对外表现相似，但数据语义和稳定性不同。[网易云分类][lib-netease-playlist]、[QQ 分类][lib-qq-playlist]、[酷狗分类][lib-kugou-playlist]、[酷我分类][lib-kuwo-playlist]、[JOOX 分类][lib-joox-playlist]

### 咪咕使用关键词包装分类

咪咕适配器内置“华语、欧美、摇滚、民谣、运动、学习”等分类。获取某分类的歌单时，将分类 ID 当作搜索关键词调用歌单搜索；空分类默认使用“华语”。因此返回的是关键词匹配歌单，不能理解为上游独立分类接口的完整目录。[咪咕分类][lib-migu-playlist]

### 汽水使用多轮采样和本地分页

汽水发现页接口没有稳定游标，单次返回少量随机歌单。适配器会根据请求范围计算拉取轮次，并发请求后按 ID 去重，再对收集到的歌单切片分页。其 `page/limit` 是对本次采样结果的分页，不保证跨请求的固定排序和连续性。[汽水分类][lib-soda-playlist]

### Web 分类分页的实际范围

当前 `/category_playlists` 固定调用 `fn(categoryID, 1, 120)`，随后由 `renderIndex` 对返回列表分页。部分平台还会压低请求上限，例如网易云最多接受 100 条。因此页面翻页主要浏览已加载集合，并未把页面的页码直接传入上游分类接口。[分类路由][app-category-route]

## 排行榜

### 统一榜单目录和榜单歌曲

```text
GET /music/toplists
    → GetTopListsFunc(source)
    → GetTopLists()
    → []TopList

GET /music/toplist_songs?source=平台&toplist_id=榜单ID
    → GetTopListSongs(topListID)
    → []Song
    → 复用播放、下载、收藏和批量操作
```

榜单目录含名称、封面、更新频率、曲目数量和来源。适配器获取榜单歌曲时，还会把排名、榜单 ID 等写入 `Song.Extra`。项目没有重新计算平台热度排名。[榜单 Web 入口][app-toplist-route]

### 平台实现对照

| 平台 | 榜单目录 | 榜单曲目 |
| --- | --- | --- |
| 网易云 | `/api/toplist` | 复用歌单详情和歌曲批量获取 |
| QQ | `musicu.fcg`，`musicToplist.ToplistInfoServer.GetAll` | `fcg_v8_toplist_cp.fcg`，按页获取 |
| 酷狗 | `/api/v3/rank/list` | `/api/v3/rank/song` |
| 酷我 | 请求排行榜页面，解析 `window.__NUXT__` 中的 `bangMenu` | `kbangserver.kuwo.cn/ksong.s` |
| 咪咕 | 移动端 `rank-index/v2.0`，失败时尝试备用接口 | `rank-info/v1.0` |
| 汽水 | 内置官方榜单，再附加发现页推荐歌单 | `charts/{id}` 与歌单详情之间回退 |
| Apple Music | 内置国家或地区 Top 100 目录 | Apple RSS 榜单，用 iTunes Lookup 补充歌曲信息 |

酷我采用页面 SSR 数据解析，是因为榜单目录的 Web 接口存在额外校验。这个实现依赖页面数据结构，维护方式与纯 JSON 接口不同。[酷我榜单][lib-kuwo-toplist]

汽水榜单页混合了官方榜单和推荐歌单。两类项目使用统一的目录模型，但通过额外字段区分；不能把目录里所有项目都视为官方排名榜。[汽水榜单][lib-soda-toplist]

Apple 的目录来自内置地区定义，点击后才拉取歌曲。榜单数据还会经过 iTunes 查询补充专辑、时长和预览地址。[Apple 榜单][lib-apple-toplist]

### 热门单曲

`music-lib` 的 `GetHotSongs(limit)` 通常读取预设热歌榜再截取条数。例如网易云热歌榜 ID 为 `3778678`，QQ 为 `26`，酷我为 `16`。这项能力复用了榜单曲目获取逻辑，并非另外做一次关键词搜索。[网易云榜单][lib-netease-toplist]、[QQ 榜单][lib-qq-toplist]

## 音频下载

### 地址解析和文件处理是两个阶段

```text
Song 的来源、ID 和 Extra
    → 平台 GetDownloadURL 或 GetDownloadInfo
    → 音频 URL 和可能需要的解密信息
    → 构造带来源请求头的 HTTP 请求
    → 获取音频
    → 必要时解密
    → 识别实际格式
    → 可选获取歌词、封面并写入元数据
    → 返回浏览器或保存文件
    → 可选同步 WebDAV
```

搜索结果中的 URL 不一定存在，也不适合假定为永久地址。多数下载请求会根据 ID 再向平台解析。`BuildSourceRequest` 按平台设置 UA、Referer、Cookie，并在需要时转发 Range。[请求构造][app-source-request]、[下载核心][app-download]

### 平台地址解析和音质选择

| 平台 | 主要实现 | 限制和回退 |
| --- | --- | --- |
| 网易云 | 根据 Cookie 检查会员状态，尝试 EAPI 高音质地址并缓存 | 高音质路径不成功时回退 WeAPI，版权和权限不足可能没有 URL |
| QQ | `music.vkey.GetVkey / UrlGetVkey` 一次请求多种文件名，按优先级选择有效 `purl` | 会员和普通账号使用不同音质候选，依赖有效权限和接口响应 |
| 酷狗 | 根据 hash、资源权限和 Cookie 尝试高音质、Tracker 和常规路径 | 已有 URL 时直接使用，否则按候选链获取 |
| 咪咕 | 请求 `listen-url`，构建多种音质候选并探测可访问性 | `DownloadInfo` 标记是否加密及解密所需 FileKey |
| 汽水 | 先解析公开 Web 流，满足条件时尝试签名 App 流 | 整曲路径不可用时可能返回公开试听 |
| Apple Music | 返回歌曲的预览 URL，必要时重新解析歌曲 | 当前适配器未实现完整音频下载流程 |

网易云默认优先尝试的级别顺序为 `lossless`、`hires`、`exhigh`，也允许通过 `Extra` 指定某个级别。不能把“支持高音质解析”理解为每首歌曲都能得到最高音质。[网易云下载][lib-netease-download]

QQ 会员候选包含多个高音质文件前缀，再降到 FLAC、OGG、MP3 等；普通账号主要尝试 MP3 候选。选出有效 `purl` 后拼成音频地址。[QQ 下载][lib-qq-download]

### 汽水签名和加密音频

`core.NewSodaClient` 把 Cookie 与签名器配置合并。未配置远程服务时使用本机签名器；配置远程地址时使用远程签名服务。本机签名器需要可用的运行时和官方客户端组件，利用客户端中的 `bdms.node` 生成请求签名。[汽水客户端构造][app-soda-client]

`GetDownloadInfo` 返回 URL、格式、时长、音质和可能存在的 `PlayAuth`。获取音频后：

- 无 `PlayAuth`：作为明文音频处理。
- 有 `PlayAuth`：调用 `soda.DecryptAudio`，解析容器和加密采样信息后生成可播放数据。

整曲路径需要 Cookie 与可用签名器；公开流可能只包含试听片段。拿到一个可访问地址，并不能说明已经获得整首歌。[汽水取流][lib-soda-download]、[汽水音频解密][lib-soda-crypto]

### 咪咕的解密路径

`core.fetchSongAudio` 对咪咕单独调用 `FetchDecryptedMiguAudio`，先获取 `DownloadInfo`，下载文件后按 `Encrypted` 决定是否使用 `FileKey` 解密。[应用音频处理][app-download]、[咪咕下载信息][lib-migu-download]

需要按调用分支理解覆盖范围：上述处理位于完整音频获取路径；Web 普通代理分支主要通过 `GetDownloadFunc` 取得 URL 后转发，不能仅凭核心层存在解密函数就推断所有 Web 下载或播放分支都执行相同的解密过程。

### Web 下载模式

入口同时注册了 `GET /download` 和 `POST /download`。当前处理函数从 query string 读取歌曲 ID、来源、名称和选项，不能只凭 POST 方法就假定它支持 JSON 请求体。[Web 下载处理][app-web-download]

| 选项或条件 | 处理方式 |
| --- | --- |
| 普通请求 | 解析 URL 后代理音频，支持上游 Range 或并发分段路径 |
| `stream=1` | 用作播放，关闭该分支的内嵌元数据和本地保存选项 |
| `embed=1` 且没有 Range | 完整获取音频，获取封面、歌词并尝试内嵌 |
| `save_local=1` 且没有 Range | 去重检查后保存到服务所在机器的下载目录，返回 JSON |
| 汽水音频 | 获取完整数据，必要时解密，再通过 `ServeContent` 返回 |
| 配置 WebDAV | 在具体支持的分支中同步文件；某些分支需要完整缓冲音频 |

“保存本地”指应用服务所在设备的文件系统。在远程部署场景，它与浏览器下载到用户电脑是不同的目的地。相关保存操作还受写入保护检查约束。

### Range 与并发分段下载

`NewSourceRangeFetch` 先发送 `Range: bytes=0-3`，检查上游是否返回 `206` 和有效的 `Content-Range`。若支持，再根据用户请求确定字节范围。[Range 探测][app-range]

分段参数为：

| 参数 | 当前值 |
| --- | --- |
| 第一块 | 32 KB |
| 后续分块 | 256 KB |
| 最大同时请求块数 | 16 |

各块可以并发完成，但写给客户端时必须保持文件顺序，因此代码按序号缓存尚未轮到的分块，再顺序输出。上游不支持这套 Range 条件时，回到普通请求转发。[分段输出][app-parallel-range]

这提供了按字节续传和拖动播放的基础，不等同于一个持久化的下载任务恢复系统。

### 格式识别与元数据

完整下载路径优先依据文件特征识别 FLAC、MP3、OGG、M4A、WMA 等格式，再参考 Content-Type。普通流式分支还会参考 URL 后缀和平台解析得到的扩展名。

可选封面和歌词通过平台方法单独获取。MP3 直接写入 ID3v2.3；FLAC、M4A、WMA 等由 FFmpeg 写入元数据。FFmpeg 缺失或处理失败时返回原始音频，并在结果中附带提示。[元数据处理][app-metadata]

默认文件名模板为 `{artist} - {name}`，支持 `{album}`、`{source}`、`{id}`、`{ext}` 等变量。文件名和相对路径会经过清理。[文件名模板][app-download]

### 批量下载与去重

Web 的 `batchDownload` 读取所选歌曲，跳过本地歌曲，再按顺序 `await requestLocalDownload(...)`。因此批量过程由浏览器逐首驱动，并不是独立的服务端持久化任务队列。[前端批量下载][app-batch-download]

`DownloadWithDedupCheckWithTemplate` 负责去重与下载记录：

1. 以归一化后的歌手和歌名构造 `SongKey`。
2. 查看去重索引，并确认对应文件是否仍存在。
3. 存在则跳过并记录状态。
4. 否则下载保存，记录成功或失败。
5. 成功后记录文件相对路径，更新去重索引。

下载记录与持久去重索引使用 SQLite 保存。清空界面中的历史记录，不会同时清空去重索引。以歌手和歌名去重便于跨平台识别，但可能合并同名的现场版、重制版或不同音质版本。[下载记录与去重][app-download-record]

## 换源匹配

### 换源输入与结果

Web 路由为 `/switch_source`，关键输入如下：

| 参数 | 含义 |
| --- | --- |
| `name` | 原歌曲名，必填 |
| `artist` | 原歌手，用于检索和相似度计算 |
| `source` | 当前来源，用于排除同平台 |
| `duration` | 原时长，单位秒 |
| `target` | 可选，只在指定平台寻找 |

返回值包含新歌曲的 ID、来源、名称、歌手、专辑、时长、封面、Extra 和匹配分数。前端随后用新结果更新卡片和操作链接。[换源路由与算法][app-switch]

### 候选来源

指定 `target` 时，只检查该目标是否允许；未指定时，先把默认来源加入集合，再补上其他来源，去重后并发搜索。

当前会排除当前平台、汽水、5sing、本地来源和没有搜索方法的来源。构造来源列表的先后顺序不代表请求串行执行，也不保证某个平台优先胜出。

### 候选搜索

优先搜索“歌名 + 歌手”；若请求出错或没有结果，并且原歌手非空，再只搜索歌名。每次搜索最多等待 6 秒，一个平台最多采用返回结果中的前 8 条进行匹配。

这里用 goroutine 与超时选择控制等待时间，没有向底层搜索函数传入可取消的 Context。因此超时或提前选中结果后，已发出的部分平台请求仍可能继续运行。

### 文本相似度

`NormalizeText` 将文本转小写并保留字母、数字等字符，过滤标点和空白。`SimilarityScore` 按 Unicode 字符计算 Levenshtein 编辑距离：[相似度工具][app-similarity]

```text
相似度 = 1 - 编辑距离 / max(文本A字符数, 文本B字符数)

总分 = 歌名相似度 × 0.7 + 歌手相似度 × 0.3
```

任一歌手为空时，仅使用歌名相似度。这里是文本比较，没有使用音频指纹、语义模型或跨平台歌曲 ID 映射。简繁转换相关逻辑存在于其他文字归一化流程中，不能假定这个函数自动执行了完整的简繁和版本名归一化。

### 时长过滤

原曲和候选的时长都已知时，允许差异约为：

```text
max(10 秒, 原歌曲时长 × 15%)
```

代码中的百分比结果会转换成整数秒。如果任一时长未知，则放过这一项检查。以原曲 240 秒为例，允许差异为 36 秒，因此 230 秒候选可以继续参与匹配，180 秒候选会被排除。

### 排序和可访问性检查

候选首先按总分降序，同分时按时长差升序排列。之后有两条选择路径：

- 提前返回：某个平台返回的最佳候选分数达到 `0.98`，且两边时长已知时差异不超过 3 秒，通过探测即可返回，不必等其余来源。
- 常规选择：汇总候选后，最多取前 12 个，以最多 6 个并发检查可访问性，最后按原排序选出第一个有效项。

提前返回意味着选择可能受平台响应时间影响；常规选择会保留排序优先级，不会简单采用最先完成探测的候选。

`ValidatePlayable` 解析候选 URL，再请求 `bytes=0-1`，将 HTTP `200` 或 `206` 视为有效。它检查的是地址能否访问，没有完整解码音频，也没有充分确认内容长度和原曲一致。[可访问性探测][app-playable]

### Web 与 TUI 的差异

两端都采用重新搜索、相似度、时长过滤和探测，但编排不同：Web 有每次搜索 6 秒等待、较高置信度提前返回，以及限定数量的并发候选探测；TUI 当前在收集并排序候选后逐个探测。[TUI 换源][app-cli-switch]

此外，Web 的常规候选收集仅排除相似度小于等于零的项，`0.98` 是提前返回条件，不是所有换源结果的最低门槛。因此候选都较差时，仍可能选中一个分数偏低但地址可访问的版本。

## 资源探测与自动换源

### 探测流程

前端通过 `/inspect` 检查歌曲资源：

```text
歌曲卡片进入探测队列
    → /inspect?id=...&source=...&duration=...&extra=...
    → 获取下载地址
    → 请求音频前两个字节
    → 根据状态码判断可访问性
    → 从 Content-Range 或 Content-Length 获取文件大小
    → 结合原时长估算码率
    → 更新卡片大小、码率或失效状态
```

码率估算公式为 `文件字节数 × 8 / 时长秒数 / 1000`，属于根据体积和时长计算的平均值，并不是音频解码器读取的精确编码参数。[探测入口][app-inspect]

探测会区分明确失效与可重试错误。临时超时、限流和上游暂不可用等情况可以返回重试标记和延迟，避免一次网络波动就立即认定资源失效。

### 前端调度

当前前端设置总探测并发上限为 50、每来源上限为 32、每卡片最多重试 2 次，并根据来源压力调整调度与等待。页面切换时会清理队列、计时器和进行中的请求状态。[探测队列][app-js]

### 自动换源

开启自动换源后，前端收集明确无效的歌曲卡片，调用 `batchSwitchSource`，并标记尝试状态，减少对同一批失效项重复处理。它不必等整页所有探测完成才启动。

批量换源最多同时处理 3 首歌曲；每首歌曲内部还会触发多平台并发搜索和候选探测。因此“三首并发”并不等于只有三个上游请求。[自动换源][app-auto-switch]、[批量换源][app-batch-switch]

这个自动化位于页面的探测与更新流程中。核心下载函数本身没有把所有失败情况都自动转换为跨平台搜索重试。

## 登录与本地音乐等关联能力

### Cookie 和个人歌单

`CookieManager` 在内存中按来源保存 Cookie，用读写锁控制并发访问，并通过配置数据库持久化。虽然源码保留了旧的 JSON 文件名常量，当前 `Load` 和 `Save` 的主要路径使用数据库。[Cookie 管理][app-service]、[配置数据库][app-config]

个人歌单通过 `GetUserPlaylistsFunc` 分发，应用当前暴露网易云、QQ、酷狗和汽水。每日推荐入口暴露网易云、QQ、酷狗、酷我，并按平台并发获取。

扫码登录采用“创建二维码会话 → 轮询状态 → 保存 Cookie”的流程。暴露列表包含网易云、QQ、QQ 微信登录、酷狗和 Bilibili；汽水虽有工厂分支，但未列入可用扫码来源列表。不能只看到方法存在就判断界面已支持可用登录。

### 歌词与封面

歌词通过 `GetLyricFunc(source)` 获取，Web 负责格式转换、展示和下载；封面可通过代理或单独下载接口获取。下载时内嵌封面和歌词复用了这些资源获取能力。

### 本地音乐和自建歌单

`local` 由应用层处理：扫描下载目录、读取音频标签与封面、查找同名歌词和图片，并在需要时使用 `ffprobe` 补充信息。搜索结果再转换成统一 `Song`，从而复用播放和歌单操作。[本地音乐][app-local-music]

自建歌单保存跨平台歌曲条目；远端歌单和专辑可以通过详情页导入。来源失效后的恢复仍依赖原歌曲信息和换源匹配，统一模型使这类操作可以跨平台复用。[集合管理][app-collection]

## 实现边界与复用建议

### 当前实现需要准确理解的边界

| 边界 | 对使用或复用的影响 |
| --- | --- |
| 来源列表表示调用入口 | 不能替代接口存活性、账号权限和地区可用性验证 |
| 搜索结果直接合并 | 没有统一跨平台去重和稳定相关性排序，顺序可能受响应速度影响 |
| 页面分页主要是结果切片 | 不能据此推断已经接入所有平台的远端连续分页 |
| 部分错误转换为空结果 | 难以区分无匹配项、权限限制和上游失败 |
| 分类语义存在差异 | 咪咕是关键词包装，汽水是采样，Apple 是编辑账号目录 |
| 部分榜单目录内置 | 名称、ID 和分组变化需要维护；汽水目录还混入推荐歌单 |
| 换源采用文本和时长匹配 | 可能选错翻唱、现场版、重制版或同名曲 |
| 换源常规路径没有较高最低分 | 有效地址可能来自较低相似度候选 |
| 探测主要检查 HTTP 状态 | 可访问不代表可完整解码，也不代表整曲而非试听 |
| Apple 下载使用预览地址 | 支持搜索和榜单不代表已经支持完整音频下载 |
| 下载分支不同 | 普通代理、保存本地、元数据内嵌、加密音频处理的行为不完全一致 |
| 完整文件处理会缓冲音频 | 元数据写入、解密和部分同步路径需要考虑内存占用 |
| 批量下载由前端顺序驱动 | 不具备独立后台任务队列的持久恢复能力 |
| 下载去重以歌手和歌名为主 | 可能把不同版本或不同音质归为同一条目 |
| Web 和 TUI 分别编排换源 | 参数、超时和探测策略存在差异，维护时需分别核对 |

### 适合直接复用的部分

如果要在其他应用中接入同类功能，可以优先复用 `music-lib` 中的平台适配器、公共模型和按能力划分的接口，再参考应用中的详情展开与下载流程。平台请求细节和界面交互分开，有利于后续替换前端或增加新的来源。

### 可进一步改进的方向

以下属于设计建议，不是当前代码已经实现的能力：

1. 让统一搜索结果携带每来源的错误、耗时、总数和下一页游标，支持真正的远端分页与部分结果返回。
2. 建立歌曲版本字段，将 Live、Remix、翻唱、重制版等特征纳入匹配，并为换源设置最低置信度。
3. 明确区分“地址可访问”“音频可解码”“整曲可用”，对试听长度和实际媒体时长做补充检查。
4. 将 Web 和 TUI 的换源算法归入公共业务层，减少两套编排产生的行为差异。
5. 为搜索和探测传递 Context，提前返回或请求取消时终止后续工作。
6. 将批量下载改为服务端任务队列，保存任务状态、重试进度和文件结果，让任务不依赖页面存续。
7. 为不同来源记录分类和榜单的数据来源类型，区分实时目录、内置目录、搜索包装和采样结果。
8. 把音质、版本和真实资源标识纳入去重策略，避免仅依据歌手与歌名合并。

## 源码阅读顺序

| 顺序 | 源码 | 需要回答的问题 |
| --- | --- | --- |
| 1 | [model/song.go][lib-model] | 歌曲、歌单、分类和榜单如何统一 |
| 2 | [provider/interface.go][lib-provider] | 平台能力怎样拆分 |
| 3 | [core/service.go][app-service] | 应用如何根据来源选择具体方法 |
| 4 | [internal/web/music.go][app-music] | 用户请求如何串起搜索、详情、分类和榜单 |
| 5 | [netease/netease.go][lib-netease-core] 与 [netease/playlist.go][lib-netease-playlist] | 一个完整平台如何处理搜索包装和歌单展开 |
| 6 | [qq/download.go][lib-qq-download] 与 [kugou/download.go][lib-kugou-download] | 不同平台如何选择音质并获得 URL |
| 7 | [core/download.go][app-download] | 获取音频后如何解密、识别格式和保存 |
| 8 | [core/service.go 的 Range 部分][app-range] | 播放与下载如何按字节范围获取内容 |
| 9 | [music.go 的换源部分][app-switch] | 多来源候选如何打分、筛选和探测 |
| 10 | [app.js][app-js] | 探测、自动换源、批量下载和界面更新如何协作 |
| 11 | [internal/cli/ui.go][app-cli-switch] | TUI 与 Web 共用哪些底层能力，又有哪些差异 |

仓库已有测试可辅助理解行为边界，例如 `internal/web/switch_source_test.go` 验证高置信度提前返回及并发探测仍保留排序优先级；`core/service_categories_test.go`、`core/service_toplists_test.go` 和 `core/service_album_test.go` 检查能力工厂和来源列表。[换源测试][app-switch-test]

上述测试可作为阅读线索；本文未执行这些测试，也未对各平台的登录和下载链路做在线回归验证。

[app-mod]: https://github.com/guohuiyuan/go-music-dl/blob/ddf2a4e550ca67086ff1f06edc5d15c7d4c8663d/go.mod
[app-server]: https://github.com/guohuiyuan/go-music-dl/blob/ddf2a4e550ca67086ff1f06edc5d15c7d4c8663d/internal/web/server.go#L537
[app-shell]: https://github.com/guohuiyuan/go-music-dl/blob/ddf2a4e550ca67086ff1f06edc5d15c7d4c8663d/internal/appshell/server.go
[app-service]: https://github.com/guohuiyuan/go-music-dl/blob/ddf2a4e550ca67086ff1f06edc5d15c7d4c8663d/core/service.go
[app-sources]: https://github.com/guohuiyuan/go-music-dl/blob/ddf2a4e550ca67086ff1f06edc5d15c7d4c8663d/core/service.go#L1059
[app-music]: https://github.com/guohuiyuan/go-music-dl/blob/ddf2a4e550ca67086ff1f06edc5d15c7d4c8663d/internal/web/music.go
[app-search]: https://github.com/guohuiyuan/go-music-dl/blob/ddf2a4e550ca67086ff1f06edc5d15c7d4c8663d/internal/web/music.go#L775
[app-render]: https://github.com/guohuiyuan/go-music-dl/blob/ddf2a4e550ca67086ff1f06edc5d15c7d4c8663d/internal/web/server.go#L332
[app-playlist]: https://github.com/guohuiyuan/go-music-dl/blob/ddf2a4e550ca67086ff1f06edc5d15c7d4c8663d/internal/web/music.go#L926
[app-album]: https://github.com/guohuiyuan/go-music-dl/blob/ddf2a4e550ca67086ff1f06edc5d15c7d4c8663d/internal/web/music.go#L951
[app-album-match]: https://github.com/guohuiyuan/go-music-dl/blob/ddf2a4e550ca67086ff1f06edc5d15c7d4c8663d/internal/web/album_match.go
[app-cli-root]: https://github.com/guohuiyuan/go-music-dl/blob/ddf2a4e550ca67086ff1f06edc5d15c7d4c8663d/cmd/music-dl/root.go
[app-categories]: https://github.com/guohuiyuan/go-music-dl/blob/ddf2a4e550ca67086ff1f06edc5d15c7d4c8663d/internal/web/music.go#L156
[app-category-route]: https://github.com/guohuiyuan/go-music-dl/blob/ddf2a4e550ca67086ff1f06edc5d15c7d4c8663d/internal/web/music.go#L691
[app-toplist-route]: https://github.com/guohuiyuan/go-music-dl/blob/ddf2a4e550ca67086ff1f06edc5d15c7d4c8663d/internal/web/music.go#L728
[app-source-request]: https://github.com/guohuiyuan/go-music-dl/blob/ddf2a4e550ca67086ff1f06edc5d15c7d4c8663d/core/service.go#L792
[app-download]: https://github.com/guohuiyuan/go-music-dl/blob/ddf2a4e550ca67086ff1f06edc5d15c7d4c8663d/core/download.go
[app-soda-client]: https://github.com/guohuiyuan/go-music-dl/blob/ddf2a4e550ca67086ff1f06edc5d15c7d4c8663d/core/soda_client.go
[app-web-download]: https://github.com/guohuiyuan/go-music-dl/blob/ddf2a4e550ca67086ff1f06edc5d15c7d4c8663d/internal/web/music.go#L1139
[app-range]: https://github.com/guohuiyuan/go-music-dl/blob/ddf2a4e550ca67086ff1f06edc5d15c7d4c8663d/core/service.go#L1382
[app-parallel-range]: https://github.com/guohuiyuan/go-music-dl/blob/ddf2a4e550ca67086ff1f06edc5d15c7d4c8663d/core/service.go#L1463
[app-metadata]: https://github.com/guohuiyuan/go-music-dl/blob/ddf2a4e550ca67086ff1f06edc5d15c7d4c8663d/core/service.go#L1647
[app-batch-download]: https://github.com/guohuiyuan/go-music-dl/blob/ddf2a4e550ca67086ff1f06edc5d15c7d4c8663d/internal/web/templates/static/js/app.js#L8055
[app-download-record]: https://github.com/guohuiyuan/go-music-dl/blob/ddf2a4e550ca67086ff1f06edc5d15c7d4c8663d/core/download_record.go
[app-switch]: https://github.com/guohuiyuan/go-music-dl/blob/ddf2a4e550ca67086ff1f06edc5d15c7d4c8663d/internal/web/music.go#L1648
[app-similarity]: https://github.com/guohuiyuan/go-music-dl/blob/ddf2a4e550ca67086ff1f06edc5d15c7d4c8663d/core/service.go#L927
[app-playable]: https://github.com/guohuiyuan/go-music-dl/blob/ddf2a4e550ca67086ff1f06edc5d15c7d4c8663d/core/service.go#L820
[app-cli-switch]: https://github.com/guohuiyuan/go-music-dl/blob/ddf2a4e550ca67086ff1f06edc5d15c7d4c8663d/internal/cli/ui.go#L1439
[app-inspect]: https://github.com/guohuiyuan/go-music-dl/blob/ddf2a4e550ca67086ff1f06edc5d15c7d4c8663d/internal/web/music.go#L1012
[app-js]: https://github.com/guohuiyuan/go-music-dl/blob/ddf2a4e550ca67086ff1f06edc5d15c7d4c8663d/internal/web/templates/static/js/app.js
[app-auto-switch]: https://github.com/guohuiyuan/go-music-dl/blob/ddf2a4e550ca67086ff1f06edc5d15c7d4c8663d/internal/web/templates/static/js/app.js#L7781
[app-batch-switch]: https://github.com/guohuiyuan/go-music-dl/blob/ddf2a4e550ca67086ff1f06edc5d15c7d4c8663d/internal/web/templates/static/js/app.js#L8272
[app-config]: https://github.com/guohuiyuan/go-music-dl/blob/ddf2a4e550ca67086ff1f06edc5d15c7d4c8663d/core/config_store.go
[app-local-music]: https://github.com/guohuiyuan/go-music-dl/blob/ddf2a4e550ca67086ff1f06edc5d15c7d4c8663d/internal/web/local_music.go
[app-collection]: https://github.com/guohuiyuan/go-music-dl/blob/ddf2a4e550ca67086ff1f06edc5d15c7d4c8663d/internal/web/collection.go
[app-switch-test]: https://github.com/guohuiyuan/go-music-dl/blob/ddf2a4e550ca67086ff1f06edc5d15c7d4c8663d/internal/web/switch_source_test.go
[lib-model]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/model/song.go
[lib-provider]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/provider/interface.go
[lib-netease-core]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/netease/netease.go
[lib-netease-song]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/netease/song.go
[lib-netease-crypto]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/netease/crypto.go
[lib-netease-playlist]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/netease/playlist.go
[lib-netease-toplist]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/netease/toplist.go
[lib-netease-download]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/netease/download.go
[lib-qq-song]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/qq/song.go
[lib-qq-playlist]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/qq/playlist.go
[lib-qq-toplist]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/qq/toplist.go
[lib-qq-download]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/qq/download.go
[lib-kugou-song]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/kugou/song.go
[lib-kugou-playlist]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/kugou/playlist.go
[lib-kugou-download]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/kugou/download.go
[lib-kuwo-playlist]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/kuwo/playlist.go
[lib-kuwo-toplist]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/kuwo/toplist.go
[lib-migu-playlist]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/migu/playlist.go
[lib-migu-download]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/migu/download.go
[lib-qianqian-playlist]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/qianqian/playlist.go
[lib-joox-playlist]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/joox/playlist.go
[lib-soda-search]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/soda/search.go
[lib-soda-playlist]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/soda/playlist.go
[lib-soda-toplist]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/soda/toplist.go
[lib-soda-download]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/soda/download.go
[lib-soda-crypto]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/soda/crypto.go
[lib-apple]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/apple/apple.go
[lib-apple-toplist]: https://github.com/guohuiyuan/music-lib/blob/7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec/apple/toplist.go
