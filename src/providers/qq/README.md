# QQ 音乐原生适配器

版本 0.1.7，source 为 `qq`。协议在 C++20 服务内实现，复用公共 HTTP／TLS、上游配额、凭证、缓存、游标、任务与文件服务；不依赖参考项目的运行时、外部下载程序或第三方解析服务。生产端没有 Python 依赖，Python 仅作为独立验收客户端。

## 能力和使用条件

| 能力 | 凭证与范围 |
| --- | --- |
| 公共推荐、分类、分类歌单 | 匿名；返回 QQ 实时接口的数据，`catalog_kind=live`；公共推荐不等于个性化每日推荐 |
| 排行榜列表、详情、曲目 | 匿名；保留排名顺序、更新时间和声明总数；原始榜单 ID 有而详情缺失时计入 `unavailable_count` |
| 单曲、歌单、专辑、歌手搜索 | 有效网页 Cookie；中文／英文、游标分页；未登录的上游可能返回空成功，因此搜索先检查登录状态 |
| 单曲／歌单／专辑详情；歌单／专辑／歌手歌曲；歌手专辑 | 可匿名，存在默认 Cookie 时使用；保留稳定实体身份、重复位置和原始专辑曲号 |
| 网页链接解析 | 离线识别 `y.qq.com/n/ryqq/songDetail|playlist|albumDetail|singer/<id>` 及旧 `/n/yqq/song|album/<mid>.html`；短链接、移动端纯查询参数链接尚不支持 |
| 歌词和封面 | 匿名行级 LRC、独立译文；无歌词明确 unavailable；可信专辑 MID 构造封面地址；复用标签嵌入和 `.lrc`／封面侧文件输出 |
| 音源 | `standard` 128 kbps MP3、`high` 320 kbps MP3、`lossless` FLAC；实际可用性受账号和歌曲权限限制；strict 不静默降级，allow_lower 只选同档或更低档 |
| Cookie 校验 | `web_cookie`；返回 valid／invalid；网络与未知上游错误单独返回；不据登录成功推断会员或购买权限 |

严格 Hi-Res、加密 QMC／MFLAC、本地 QQ 文件解密、个人歌单和扫码登录不属于本次实现。高品质和无损的成功下载需要相应有权限样本，不能用标准品质的验收替代。

## 接入现有服务

新构建默认包含 QQ。已有 CMake 缓存须显式更新来源列表后重编译：

```powershell
cmake --preset windows-msvc-debug "-DMUSIC_SERVICE_PROVIDERS=kuwo;fivesing;netease;qq"
cmake --build --preset windows-msvc-debug
```

通过现有凭证 API 新建 `{source:"qq", cookie:本机读取的内容, label:自定名称}`，调用 `/api/v1/credentials/{credential_id}/verify`，再将其设为 `/api/v1/credential-defaults/qq`。Cookie 不放进服务配置、URL、命令行参数或代码。当前校验要求 `uin` 以及 `qqmusic_key` 或 `qm_keyst`。刷新需用户更新 Cookie，本版本不自动刷新。

公共路径与已有平台相同，搜索用复数 `sources`，其他目录用 `source`：

```text
GET /api/v1/search/tracks?sources=qq&q=小星星&limit=20
GET /api/v1/search/playlists?sources=qq&q=轻音乐&limit=20
GET /api/v1/search/albums?sources=qq&q=周杰伦&limit=20
GET /api/v1/search/artists?sources=qq&q=周杰伦&limit=20
GET /api/v1/recommendations/playlists?source=qq&limit=20
GET /api/v1/categories?source=qq
GET /api/v1/categories/playlists?source=qq&category_id=10000000&limit=20
GET /api/v1/charts?source=qq
GET /api/v1/charts/tracks?source=qq&id=62&limit=20
POST /api/v1/links/parse  {"text":"https://y.qq.com/n/ryqq/songDetail/002MicCm2pZIuc"}
POST /api/v1/downloads   {"track":{"source":"qq","id":"002MicCm2pZIuc"}}
```

下载遵守既有 Bearer、destination、Idempotency-Key 和品质参数契约。搜索失败时总错误与 `meta.source_results[].error` 分别描述聚合结果和 QQ 的具体原因。若希望默认搜索 QQ，需要在配置中显式增加 `providers.qq`，并将 `qq` 加入 `catalog.default_sources`；注册新平台不会自动改变用户原有默认来源。

每页 1～50 项，最大起始偏移 10000。上游 page_num／begin 按请求窗口推进，短页不会错误地按可见数推进；总数是上游声明值，未提供原始 ID 清单时不会虚构缺失曲目数。曲目列表变化会使相应游标过期，客户端从第一页重读。推荐校验有序 ID 集合，播放量等展示字段变化不影响游标；歌单和榜单还校验更新时间／期次，其他集合校验实体及总数。这些校验不保证上游同数目重排时的原子快照。

## 协议与资源边界

2026-10-23 通过 QQ 自有 HTTPS 端点核实，参考线索为 `guohuiyuan/music-lib@6353508e84cd357b5bc3acf32d95a16e01bbe20c`：

| 入口 | 原生调用 |
| --- | --- |
| `u.y.qq.com/cgi-bin/musicu.fcg` | `music.search.SearchCgiService.DoSearchForQQMusicDesktop`、`music.UserInfo.userInfoServer.GetLoginUserInfo`、`playlist.HotRecommendServer.get_hot_recommend` |
| 同一 RPC 入口 | `musicToplist.ToplistInfoServer.GetAll/GetDetail`、`music.srfDissInfo.aiDissInfo.uniform_get_Dissinfo` |
| 同一 RPC 入口 | `music.musichallAlbum.AlbumInfoServer.GetAlbumDetail`、`AlbumSongList.GetAlbumSongList`、`AlbumListServer.GetAlbumList`、`musichall.song_list_server.GetSingerSongList` |
| 同一 RPC 入口 | `music.pf_song_detail_svr.get_song_detail_yqq`、`vkey.GetVkeyServer.CgiGetVkey` |
| `c.y.qq.com` | `fcg_get_diss_tag_conf.fcg`、`fcg_get_diss_by_tag.fcg`、`fcg_query_lyric_new.fcg`，显式请求 `outCharset=utf-8` |

会话密钥只放入 secret_headers 的 Cookie；不写入 RPC authst、URL 或日志。匿名目录请求不附 Cookie。音频文件名使用 `file.media_mid`，精确匹配请求歌曲、文件名及成功状态；拒绝其他曲目、未知错误、外站路径或非空加密 ekey。vkey 地址仅用于后端传输，不作为客户端直链，也不把 Cookie 转发给 CDN。104003 表示当前账号不可访问该音质，不会把凭证整体标记为失效。最终文件仍由公共处理器检查格式、品质和时长，不转码、不提升品质。

响应大小不超过 4 MiB，JSON 深度不超过 32；数组、文本、标识、页数和时长都有上限。未知响应和超时返回明确错误，不以空列表替代失败。

## Windows 验收

离线回归见 [qq_test.cpp](../../../tests/unit/providers/qq_test.cpp)。在线验收只读取本机 Cookie 文件，不输出账号、Cookie 或 CDN 签名地址：

```powershell
python packaging/e2e/qq_e2e.py --binary out/build/windows-msvc-debug/src/server/music-service.exe --cookie-file .local-secrets/qq-cookie.txt --report out/qq-live-final-report.json
```

客户端建立独立临时数据目录和中文下载目录，验收完成后移除；不改用户正常服务。它检查匿名／无效／有效凭证、三页搜索、中英文关键词、推荐和目录两页、两个榜单、完整样本歌单与专辑遍历、严格品质与允许降级、真实下载、标签／侧文件、复用和重启 SHA-256 回读。仅对目录超时／限流有限重试，逐次写入报告，保留网络波动证据。结果和账号限制记录在[平台路线图](../../../docs/design/cpp20-refactor/platform-roadmap.md)。
