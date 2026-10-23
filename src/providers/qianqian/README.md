# 千千音乐原生适配器

source 为 `qianqian`，协议包版本 `0.1.11`。使用原生 C++20 完成参数排序、时间戳、MD5 签名、请求与解析，复用公共 HTTP／TLS、入场预算、缓存、身份绑定游标和持久下载任务。Python 仅用于独立验收客户端。

## 能力与条件

| 能力 | 当前范围 |
| --- | --- |
| 单曲、专辑搜索 | 匿名中文／英文搜索，分页；保留付费歌曲的目录信息，播放权限由音源接口单独判断 |
| 单曲详情 | TSID／assetId 稳定身份、真实演唱者、专辑、封面、时长和上游曲目编号；作词、作曲人员不冒充演唱者，缺少歌手信息时保留曲目并留空 |
| 专辑详情与曲目 | `P…` 专辑标识；完整列表限制 2000 个位置，按公共游标分页；保留顺序、重复位置和不可用占位 |
| 歌单详情与曲目 | 数字歌单 ID；显式发送 pageNo／pageSize，避免将默认前 20 首当成完整歌单；保留数组顺序，歌单位置不作为专辑曲号 |
| 分类、分类歌单 | 官方实时分类；先验证分类存在，再读取对应歌单 |
| 公共推荐歌单 | 官方首页“热门歌单”所用 `tracklist/list` 数据；`catalog_kind=live`，不是个性化每日推荐 |
| 网页链接 | `https://music.91q.com/song/T…`、`/album/P…`、`/songlist/数字`；离线解析并核对主机、端口和实体类型 |
| 歌词与封面 | 官方 LRC、无时间戳的纯文本歌词和图片，复用标签嵌入、独立侧文件及文件票据；无歌词明确 unavailable |
| 音源 | standard（128 kbps MP3）、high（320 kbps MP3）、lossless（FLAC）；仅对官方提供的有权限资源生效 |

共 13 项能力。歌单搜索的 `search type=6` 在 2026-10-23 返回 `23001`（不支持的搜索类型），保持未注册；榜单、歌手目录、Cookie／扫码登录、付费权限获取和 Hi-Res 也未注册。未知能力通过公共接口返回 `CAPABILITY_UNSUPPORTED`。有目录信息不等于有音源权限，VIP 无地址返回 `CREDENTIALS_REQUIRED`，当前版本不能导入千千 Cookie。

## 使用

新构建包含此平台。已有 CMake 缓存须把 `qianqian` 追加到当前 `MUSIC_SERVICE_PROVIDERS` 列表后重新配置、编译。平台注册不会改变已有 `catalog.default_sources`，默认聚合搜索需要自行将 `qianqian` 加入配置。

```text
GET /api/v1/search/tracks?sources=qianqian&q=许嵩&limit=20
GET /api/v1/search/albums?sources=qianqian&q=许嵩&limit=20
GET /api/v1/recommendations/playlists?source=qianqian&limit=20
GET /api/v1/categories?source=qianqian
GET /api/v1/categories/playlists?source=qianqian&category_id=2376&limit=20
GET /api/v1/playlists/tracks?source=qianqian&id=309735&limit=20
GET /api/v1/albums/tracks?source=qianqian&id=P10004353346&limit=20
POST /api/v1/links/parse {"text":"https://music.91q.com/song/T10065435650"}
POST /api/v1/downloads {"track":{"source":"qianqian","id":"T10065435650"},"quality":"standard"}
```

沿用公共 Bearer、destination、Idempotency-Key 和品质契约。`strict` 不静默降档；`allow_lower` 返回实际品质。`trail_audio_info` 始终标为 preview，默认拒绝；缺失的试听格式／品质信息会明确失败。试听优先使用自身有效期，未提供时沿用外层期限。无地址且上游标明 VIP／付费时报告权限限制，不自动换曲、换来源或取得付费权限。

每页 1～50 项，最大起始偏移 10000；短页按请求窗口推进。专辑游标校验完整列表摘要，歌单游标校验 ID、声明总数、标题和创建时间，推荐／分类歌单校验所属类别及总数。后两者不能检测同数目重排，不提供原子快照。上游 `trackCount` 与可见位置分开处理，未取得全量 ID 清单时不伪造 `listed_total` 或缺失总数。旧数字专辑别名、短链、带查询或 fragment 的分享链接暂不支持。

## 协议和验证边界

协议线索来自本仓库参考源码 `third_party/music-lib-source/qianqian`；逐项与[千千官网](https://music.91q.com/)及其当前网页脚本核对。生产请求仅使用千千官方 HTTPS 端点：

| 入口 | 用途 |
| --- | --- |
| `music.91q.com/v1/search` | type=1 单曲、type=3 专辑 |
| `music.91q.com/v1/song/info`、`song/tracklink` | 单曲信息、带期限的资源 |
| `music.91q.com/v1/album/info` | 专辑及完整曲目列表 |
| `music.91q.com/v1/tracklist/info` | 歌单与分页曲目 |
| `music.91q.com/v1/tracklist/category`、`tracklist/list` | 分类、分类歌单、首页热门歌单 |

签名使用官方网页公开常量，按键排序后对原始 UTF-8 参数及常量求 MD5，再做 URL 编码；不是用户凭证。每次请求重新生成秒级时间戳，不复用旧签名。错误签名 `22001`、不存在 `23010`、未知上游错误、HTTP 429、超时、取消各自保留错误语义；`23001` 不一律映射成登录失败。本次官方接口仍接受正确签名的旧时间戳，不能声称官方强制了某个签名有效窗口。

JSON 最大 4 MiB／32 层，歌词最大 256 KiB，文本、数组、标识和数字都有边界。资源地址检查 HTTPS、端口和官方主机；音源只接受 `audio*.dmhmusic.com`，不转发 Cookie，不向客户端暴露临时音源地址。文件仍经过公共 MP3／FLAC 验证器和标签处理器；返回格式字段不能替代真实字节验证。

官网忽略 `Accept-Encoding: identity` 仍返回 gzip。适配器显式请求保留压缩字节，在自身边界内使用已固定版本的 zlib 解码；压缩体和解压结果分别限长，校验 CRC、完整结束及取消／期限。公共传输只增加可选的 `preserve_content_encoding`，其他平台保持默认处理，音频下载不启用此选项。

离线用例见 [qianqian_test.cpp](../../../tests/unit/providers/qianqian_test.cpp)，在线客户端见 [qianqian_e2e.py](../../../packaging/e2e/qianqian_e2e.py)：

```powershell
python packaging/e2e/qianqian_e2e.py --binary out/build/qianqian-msvc-debug/src/server/music-service.exe --report out/qianqian-live-report.json
```

客户端使用独立临时数据／下载目录，检查搜索三页、发现与集合分页、整集合遍历、链接、歌词、无权限、三档实际下载、元数据和侧文件、SSE、幂等复用以及重启后的 SHA-256 回读。验收结论和环境见[千千验收记录](../../../docs/research/qianqian-acceptance.md)。
