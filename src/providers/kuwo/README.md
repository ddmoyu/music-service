# 酷我适配器

首个开发平台，source `kuwo`，实现 kuwo-native 0.1.9。能力均为匿名（不登记 Cookie profile，不发送 Cookie）。B 阶段补充 `www.kuwo.cn`／`kuwo.cn` 的 `/playlist_detail/{id}`、`/album_detail/{id}` 链接，返回 playlist／album，沿用现有集合详情与分页；仅接受默认端口，不展开短链。

| 能力 | 端点（全部 HTTPS） | 说明 |
| --- | --- | --- |
| `search_tracks` | `www.kuwo.cn/search/searchMusicBykeyWord` | pn／rn 分页，游标为页码字符串；每页 1～50；TOTAL 给出总数 |
| `track_detail` | `wapi.kuwo.cn/api/www/music/musicInfo?mid={rid}` | 直接读取 JSON；校验 rid，保留专辑身份、时长、曲号与可信专辑封面；不存在为 NOT_FOUND |
| `parse_links` | 无网络 | 官方主机的单曲、歌单、专辑，以及 `/singer_detail/{id}` 的主页面、`/info`、`/album`、`/mv`；只接受默认端口 |
| `resolve_audio` | `mobi.kuwo.cn/mobi.s`（br=128kmp3／320kmp3／2000kflac／23000kflac） | standard／high／lossless／hires（黑胶）；返回的 CDN 地址限 `*.kuwo.cn` 默认端口，统一改用 HTTPS |

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

每次调用只请求一个上游页，游标为页码字符串；集合 id 为十进制数字，非法 id、其他来源的 id 和非法游标不发请求。个人歌单尚未接入；歌手能力见下表。

- 0.1.8 将单曲详情切换到已验证可匿名访问的 `wapi.kuwo.cn/api/www/music/musicInfo`，移除旧的 HTML 页面解析实现，不进行网页回退。歌词继续使用 newlyric HTTPS 端点；不根据登录状态推断音源权限。

目录：`protocol/` 构造请求，`parsing/` 解析响应（纯函数，`collections.*` 为集合、`artists.*` 为歌手与 JSON 单曲详情；契约测试使用 `tests/fixtures/providers/kuwo` 的脱敏样本），`client.*` 是共用的上游请求与状态码映射，`capabilities.cpp`、`collections.cpp`、`artists.cpp` 分别是单曲、集合与歌手的能力对象，`bundle.cpp` 注册。

## 0.1.5 补齐

- `lyrics`：`https://newlyric.kuwo.cn/newlyric.lrc`，请求 XOR／base64，响应 zlib 解压、base64／XOR 后严格 GB18030→UTF-8。Windows 使用系统转换，POSIX 使用 iconv；不调用外部程序。
- 压缩输入最多 512 KiB，解压最多 2 MiB、最多 10000 行；截断、尾随数据、非法编码和字标记均拒绝；`tp=none` 明确返回 unavailable。
- 返回行级 LRC 并接通下载内嵌歌词。保留同时间轴的附加文本，不猜测翻译语言；酷我字标记含非标准负值，未声明字级时间能力。
- high 必须为同一 rid 的 MP3／320 kbps；lossless 必须为 FLAC，不能仅看请求参数。`flac` 参数实测会降级，采用有效的 `2000kflac`。平台降级由 strict 拒绝，allow_lower 明确标记；替代 rid／非零 type 仍是试听，不能续接成原曲。
- 下载完成后再次检查文件：MP3 帧码率、FLAC 容器、时长；只能维持或降低平台声明。实测 228908 的 high 为 320000 bit/s、269819 ms，FLAC 为 24 bit／44100 Hz、269747 ms；后者仍称 lossless。

## 0.1.8 歌手与专辑封面

全部通过 `wapi.kuwo.cn/api/www/` 的官方 JSON 路径，携带 `httpsStatus=1&plat=web_www` 和官网 Referer；不需要 Cookie 或签名工具，不解析 HTML。

| 能力 | 路径 | 对外接口与范围 |
| --- | --- | --- |
| `search_artists` | `search/searchArtistBykeyWord?key=...&pn=...&rn=...` | `GET /api/v1/search/artists?sources=kuwo&q=周杰伦&limit=20`；每页 1～50 |
| `artist_detail` | `artist/artist?artistid=...` | `GET /api/v1/artists/detail?source=kuwo&id=336`；姓名、头像、简介、别名、地区、生日、出生地、性别、语言、身高、体重、星座及歌曲／专辑／MV 数量 |
| `artist_tracks` | `artist/artistMusic?artistid=...&pn=...&rn=...` | `GET /api/v1/artists/tracks?source=kuwo&id=336&limit=100`；每页 1～100，不限制为热门 50 首 |
| `artist_albums` | `artist/artistAlbum?artistid=...&pn=...&rn=...` | `GET /api/v1/artists/albums?source=kuwo&id=336&limit=20`；每页 1～50，含发行日期和专辑展示封面 |

请求 `pn` 从 1 开始，搜索响应 `pn` 从 0 开始；上游私有游标按页窗口推进，公共 API 返回绑定查询、歌手和页大小的不透明游标。每次只请求一页，短页且尚未到 total 时继续；不以异常空页、错误结构或重复 ID 伪装成功。目录内容随上游变化，不是固定历史快照。资料缺失为 null，计数来自上游，不用当前页数量代替。

单曲详情与歌手歌曲中的 `albumpic` 只在同时具有有效专辑 ID 和标题时作为歌曲封面；只接受 `*.kuwo.cn/star/albumcover/` 默认端口公开图片，统一 HTTPS，不用歌手头像或其他 `pic` 字段替代。下载通过现有原生媒体处理流程嵌入 MP3／FLAC；开启 `downloads.output.export_cover` 可导出同一张封面，`sidecars_required` 控制是否必须成功。没有封面的歌曲维持缺失语义，遵守既有元数据策略。

Windows 在线验收客户端（Python 只驱动 HTTP 与核对文件，生产服务不依赖 Python）：

```powershell
python packaging/e2e/kuwo_artists_e2e.py --binary out/build/windows-msvc-debug/src/server/music-service.exe --report out/kuwo-artists-live-report.json
```

客户端使用独立临时数据目录、不导入账号 Cookie，验证两位歌手资料、全目录分页、错误 ID／游标、MP3／FLAC 嵌入与独立封面、结果复用及重启回读。验收记录见[酷我歌手与封面验收](../../../docs/research/kuwo-artists-acceptance.md)。

## 0.1.9 黑胶与原生 FLAC 完整校验

统一 API 的 `quality: "hires"` 映射酷我 `vinyl`／`br=23000kflac`，不新增另一套品质枚举。该参数来自搜索响应 `N_MINFO` 的黑胶档位；并非所有歌曲都提供。上游返回普通档位时，`strict` 拒绝；`allow_lower` 可依次尝试普通无损、320 kbps、标准品质并报告实际档位。协议损坏、网络失败和限流不以换档掩盖。FLAC 的 `bitrate=2000/23000` 是档位代码，不作为实际码率输出。

```json
{
  "track": {"source": "kuwo", "id": "492445332"},
  "quality": "hires",
  "quality_policy": "strict",
  "metadata_policy": "best_effort"
}
```

向 `POST /api/v1/downloads` 提交上述 JSON。黑胶 FLAC 使用内置 `audio.kuwo-flac` v1：

- 本项目 C++ 按帧解码 constant／verbatim／fixed／LPC 子帧、Rice 残差和声道关联，检查头 CRC8、帧 CRC16、序号、样本总数及 STREAMINFO PCM MD5；不调用外部下载、解码或修复工具。
- 仅在完整音频通过后识别附加尾部。边界来自最后一帧；尾部须不超过 4096 字节并匹配已验证的首尾标记。未知尾部、损坏、截断、缺少总样本数或 PCM MD5 均拒绝，不固定删去 30 字节。
- 清理到工作副本，原始下载保留供重试；标题、专辑和封面仍走原有标签流程。最终副本再次完整校验并比较 PCM，保持音频帧不变，不转码。清理成功报告 `KUWO_TRAILER_REMOVED`。
- 发布前使用实际位深、采样率和容器执行已有品质规则：FLAC 至少 24 bit／48 kHz 才维持 `hires`；16 bit 或较低采样率不能冒充高规格。严格模式拒绝，允许降级时如实报告 `lossless`。
- MFLAC／MGG 仍未支持，不能仅因扩展名或文件大小标记为黑胶成功。

Windows 在线验收：

```powershell
python packaging/e2e/kuwo_vinyl_e2e.py --binary out/build/windows-msvc-debug/src/server/music-service.exe --output-dir out/kuwo-vinyl-acceptance
```

验收客户端从服务 PATH 中排除外部音频和脚本工具，验证真实歌曲、严格品质、尾部清理、标签与封面、复用、Range 和重启回读。结果见[黑胶原生接入验收](../../../docs/research/kuwo-vinyl-acceptance.md)。
