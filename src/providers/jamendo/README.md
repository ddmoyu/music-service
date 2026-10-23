# Jamendo 原生适配器

`source=jamendo`。服务内以 C++20 调用 Jamendo 网站 JSON 接口，复用公共传输、入场配额、缓存、身份游标、任务、音频验证与文件出口。匿名使用，无 Cookie、外部下载程序或第三方解析服务。

## 当前能力

| 能力 | 范围 |
| --- | --- |
| 歌曲、专辑、歌手搜索 | UTF-8 关键词、游标分页；总数未知时返回 null；多取一个观察项判断下一页 |
| 精选歌单搜索 | 对官网精选目录的名称和简介做关键词匹配；ASCII 忽略大小写，其他 UTF-8 文本按原文匹配；`catalog_kind=search_derived`，范围不含全站用户歌单 |
| 原生分类与分类歌曲 | 官网风格／情绪／主题标签，`lists=track`；通过 `categories/tracks` 读取歌曲 |
| 官方歌曲榜 | Best Of、Trending 和风格 Best Of 前 100 项；榜单目录、详情和曲目分页；保留官方顺序，变化时使旧游标失效 |
| 歌曲详情 | 标题、歌手、专辑、时长、可信封面；从真实专辑位置补曲号和总曲数 |
| 专辑详情与曲目 | 按上游 ID 清单分页，仅批量读取当前页详情；保留顺序和缺失位置 |
| 歌手详情与专辑 | 公开名称、简介、国家、专辑数及专辑分页；未知字段保持未知 |
| 公共推荐歌单 | 网站 `/api/playlists/featured`，`catalog_kind=live`；支持游标分页 |
| 歌单详情与曲目 | 推荐项与详情身份相同；重复曲目保留；缺失或明确不可用项计入当前页 `unavailable_count` |
| 链接 | 官网 track／album／artist／playlist 数字 ID，可含语言前缀和标题；明确的 `jamen.do/t/<id>`；拒绝身份查询参数、非官方域名和歧义路径 |
| 歌词 | 保留平台返回的纯文本或时间标签；无歌词返回 `unavailable=true`，不生成时间轴 |
| 下载 | 明确允许下载的 MP3 `mp32`，映射 `standard`；复用元数据标签和封面／歌词侧文件能力 |

共 20 项能力。全站用户歌单搜索、分类歌单、歌手全部歌曲、个人目录和登录尚未接入。官网歌手的 `mostpopular` 是热门子集，不注册为全量歌曲目录。歌词翻译和逐字结构未提供。

## 请求示例

所有请求沿用公共 API 的 Bearer 鉴权。来源凭证与本服务访问令牌是不同概念；Jamendo 本轮无需来源凭证。

```text
GET /api/v1/search/tracks?sources=jamendo&q=love&limit=20
GET /api/v1/search/albums?sources=jamendo&q=love&limit=20
GET /api/v1/search/artists?sources=jamendo&q=love&limit=20
GET /api/v1/search/playlists?sources=jamendo&q=best%20of&limit=20
GET /api/v1/categories?source=jamendo
GET /api/v1/categories/tracks?source=jamendo&category_id=23&limit=20
GET /api/v1/charts?source=jamendo
GET /api/v1/charts/detail?source=jamendo&id=genre%3A23
GET /api/v1/charts/tracks?source=jamendo&id=trending&limit=20
GET /api/v1/recommendations/playlists?source=jamendo&limit=20
GET /api/v1/playlists/detail?source=jamendo&id=500608490
GET /api/v1/playlists/tracks?source=jamendo&id=500608490&limit=50
GET /api/v1/albums/detail?source=jamendo&id=5987
GET /api/v1/albums/tracks?source=jamendo&id=5987&limit=50
GET /api/v1/artists/detail?source=jamendo&id=6934
GET /api/v1/artists/albums?source=jamendo&id=6934&limit=20
GET /api/v1/tracks/detail?source=jamendo&id=49292
GET /api/v1/lyrics?source=jamendo&id=49292
```

提交下载：

```json
{"track":{"source":"jamendo","id":"49292"},"quality":"standard"}
```

严格请求高品质、无损或 Hi-Res 返回 `QUALITY_UNAVAILABLE`。显式 `quality_policy=allow_lower` 才返回标准档。`mp32` 是 VBR 编码，解析阶段码率保持未知，实际格式、帧、时长及平均码率由公共处理器测量，不声称 320 kbps。

2026-10-23 核查的匿名下载字段仅含 MP3／OGG。两首完整 MP3 的独立实测平均码率约为 122 kbps 和 201 kbps；`mp32` 不能直接映射为本服务的 `high`。官方开发者 API 文档列出 FLAC，但需要自有 `client_id`，本匿名网站适配器未验证该途径；这不等于 Jamendo 平台完全没有无损资源。详见[音质核查记录](../../../docs/research/jamendo-acceptance.md#第二阶段发现能力与音质核查)。

## 协议与边界

- 使用网站 `/api/*`，**不是**官方开发者 `/v3.0` API。请求头 `x-jam-call` 为 `$` + 小写 SHA-1(path + 十进制 nonce) + `*` + nonce + `~`；查询参数不参与该摘要。版本头来自实测官网客户端。
- 仅使用详情中明确的 `isDownloadable` 和 `download` 地址。没有下载许可、没有 MP3 或只有 `stream` 时不会把播放地址转为下载地址，也不会猜测 FLAC 等地址。
- 下载地址必须是 HTTPS `*.storage.jamendo.com/download/track/<同一ID>/mp32/`，封面只接受已核实的 Jamendo 图片主机。拼图歌单不把其中一首歌的封面当作整张歌单封面。
- 集合清单上限 10,000 项，正文上限 4 MiB，JSON 深度 32；超限明确失败。搜索／推荐每页 1～50，曲目列表 1～100，歌手专辑 1～50。接口返回完整 ID 清单时只读取当前窗口的歌曲或专辑详情，不在后台展开整张集合的音频。
- 集合游标绑定有序 ID 与真实位置的摘要；内容变化返回 `CURSOR_EXPIRED`。上游推荐／搜索没有稳定快照标识，采用网站 offset 语义，连续请求期间上游内容变化可能使结果移动。
- 精选歌单搜索按每批 100 项读取完整目录，最多 1,000 项，超限或重复页明确失败；只筛选元数据，不读取整张歌单的曲目详情。`total=null`，`listed_total` 是精选范围内的匹配数，内容或顺序改变会使后续游标失效。
- 分类目录一次读取最多 100 项，不依赖其被上游忽略的 offset；返回达到上限则失败。分类歌曲每页 1～50，使用官网实时排序与 offset，期间目录变化可能使结果移动。
- 榜单 ID 为 `bestof`、`trending` 或 `genre:<分类ID>`；风格 ID 来自分类目录。每次读取前 100 项身份清单，仅补齐当前页详情；`total=null`，`listed_total` 是当前可见前 100 项数量，未提供的更新时间保持未知。
- HTTP 403 保留上游拒绝含义，不猜测为需要登录；429 保留限流与合法 Retry-After。错误不包含上游正文或带参数的资源地址。

## 验证

新构建默认清单包含 Jamendo。复用已有 CMake 缓存时，需要在保留原有平台选择的基础上，将 `jamendo` 追加到 `MUSIC_SERVICE_PROVIDERS`，再重新配置与构建；未配置 `catalog.default_sources` 时仍使用项目原有默认搜索来源，可通过 `sources=jamendo` 显式搜索。

离线用例标记为 `[jamendo]`；真实客户端为 [jamendo_e2e.py](../../../packaging/e2e/jamendo_e2e.py)：

```powershell
python packaging/e2e/jamendo_e2e.py --binary out/build/jamendo-msvc-debug/src/server/music-service.exe --report out/jamendo-live-report.json
```

构建及真实下载的结果以 [Windows 验收记录](../../../docs/research/jamendo-acceptance.md) 为准。其他系统、Release 包和 Windows 系统服务不在此次专项验收范围。
