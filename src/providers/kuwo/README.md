# 酷我适配器

首个开发平台，source `kuwo`，实现 kuwo-native 0.1.2。能力均为匿名（不登记 Cookie profile，不发送 Cookie）。B 阶段补充 `www.kuwo.cn`／`kuwo.cn` 的 `/playlist_detail/{id}`、`/album_detail/{id}` 链接，返回 playlist／album，沿用现有集合详情与分页；仅接受默认端口，不展开短链。

| 能力 | 端点（全部 HTTPS） | 说明 |
| --- | --- | --- |
| `search_tracks` | `www.kuwo.cn/search/searchMusicBykeyWord` | pn／rn 分页，游标为页码字符串；每页 1～50；TOTAL 给出总数 |
| `track_detail` | `www.kuwo.cn/play_detail/{rid}` 页面 | 读取隐藏字段的歌名、歌手、专辑与页面状态里的时长；页面须指向同一 rid；430／404 或无歌曲信息为 NOT_FOUND |
| `parse_links` | 无网络 | `www.kuwo.cn`／`kuwo.cn` 的 `/play_detail/{rid}` 与 `m.kuwo.cn/newh5app/play_detail/{rid}` |
| `resolve_audio` | `mobi.kuwo.cn/mobi.s`（br=128kmp3） | 仅标准品质；返回的 CDN 地址限 `*.kuwo.cn` 默认端口，统一改用 HTTPS |

- 所有请求经 `ProviderSetup::upstream`（统一入场、TLS、重定向规则）；无法入场返回可重试的 SERVICE_BUSY，details 给出受限维度。
- 音源证据：返回 rid 与请求不同或 `type` 非 0 时是替代短片，报告为 preview 且不声明品质；同 rid、`bitrate`=128、`format`=mp3 时报告标准品质；`type` 为 0 且时长为正时报告上游声明的整曲，`type` 缺失或无法读取时完整性保持未知（附 full_length_unverified 警告），字节层面的核验在 T15。
- 搜索结果不按标题猜测版本（Live 等），未知字段保持为空。
集合能力（T23，全部 HTTPS、匿名，2026-10-22 实测）：

| 能力 | 端点 | 说明 |
| --- | --- | --- |
| `search_playlists`／`search_albums` | `search.kuwo.cn/r.s`，ft=playlist／album | 单引号伪 JSON，`parsing::legacy_json` 转为 JSON；pn／rn 分页，应答页码须与请求一致 |
| `playlist_detail`／`playlist_tracks` | `nplserver.kuwo.cn/pl.svc?op=getlistinfo` | 未知 pid 返回另一个空歌单，按 id 不符判 NOT_FOUND；total 为歌单声明的曲目数 |
| `album_detail`／`album_tracks` | `search.kuwo.cn/r.s?stype=albuminfo` | 无 albumid 为 NOT_FOUND；songnum 为曲目数 |
| `charts` | `wapi.kuwo.cn/api/pc/bang/list` | 树的叶子；id 是 ksong.s 的榜单号 |
| `chart_detail`／`chart_tracks` | `kbangserver.kuwo.cn/ksong.s` | 必须带 encode=utf8，否则返回 GBK；时长取 song_duration；`{}` 为 NOT_FOUND |
| `categories`／`category_playlists` | `wapi.kuwo.cn/api/pc/classify/playlist/getTagList`／`getTagPlayList` | 只列 digest 10000 的歌单标签（专区条目不列歌单）；未知分类为空页 |
| `recommend_playlists` | `.../getRcmPlayList?order=hot` | wapi 的页码从 1 开始，游标仍从 0 开始 |

每次调用只请求一个上游页，游标为页码字符串；集合 id 为十进制数字，非法 id、其他来源的 id 和非法游标不发请求。歌手与歌词尚未实现（歌词接口需要 zlib 与 GB18030 解码）。

- 参考实现使用的 `m.kuwo.cn/newh5/singles/songinfoandlrc` 在 2026-10-20 对所测歌曲均返回查询失败，`www.kuwo.cn/api/www/music/musicInfo` 拒绝请求，因此详情改读歌曲页面。高品质、歌词与凭证能力尚未实现，不声明。

目录：`protocol/` 构造请求，`parsing/` 解析响应（纯函数，`collections.*` 为集合；契约测试使用 `tests/fixtures/providers/kuwo` 的脱敏样本），`client.*` 是共用的上游请求与状态码映射，`capabilities.cpp` 与 `collections.cpp` 分别是单曲与集合的能力对象，`bundle.cpp` 注册。
