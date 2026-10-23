# 咪咕原生适配器

`source=migu`，实现标识 `migu-native 0.1.12`。通过既有公共 HTTP API、凭证、游标、缓存、上游入场与下载处理链路接入，不启动外部解析器或下载工具。

## 能力与边界

注册 18 项能力：单曲／歌单／专辑／歌手搜索；单曲详情、链接、歌词和音源；歌单与专辑详情及曲目；公共推荐、分类及分类歌单；榜单列表、详情及曲目。

- 公共目录与标准音源可以匿名请求。公共推荐来自官网歌单广场的编辑、专题、功能与上新目录；排除每次请求随机变化的场景模块，以便有序翻页。分类来自官网标签接口，不把内置关键词搜索伪装为推荐或分类。
- 搜索沿用上游结果。当前实测无匹配关键词会被上游回退为热门歌曲，并未提供已验证的可靠区别字段；不凭标题不含关键词过滤掉可能的歌手／歌词匹配，也不宣称该场景返回空结果。上游实际返回空数组时保留为空。
- `standard`、`high`、`lossless` 分别请求官方 `PQ`、`HQ`、`SQ`。`strict` 不静默降级；只有 `allow_lower` 才尝试较低档位，包括高档位返回加密音源的情况。只使用取流接口返回的 URL，不重写路径猜测更高音质。
- `auditionsLength`、`auditionsType` 和 `chargeAuditions` 联合判定试听。默认公共策略拒绝试听；显式 `allow_preview=true` 才接受已知试听。试听时长优先使用响应级正值，其次使用歌曲条目中的正值；两者均缺失时保持未知，不借用整曲时长。URL 可访问或文件较大不能证明有整曲权限。不确定的完整性保留 `unknown`。
- 地址刷新后，完整性或处理计划变化会使旧资源和票据失效，要求重新解析，不继续保留旧的整曲描述。官方取流结果明确为 `formatType=Z3D` 且提供 Android WAV 密钥时，使用原生 C++ 解密和校验；密钥只存内存。普通 PQ／HQ／SQ 旁附带的 `z3dCode` 不会改变实际选择的音源。Z3D 不是统一音质档位，`actual_quality` 保持未知；详见 [Z3D 实现与验证](../../../docs/research/migu-z3d.md)。
- 可选 `web_cookie` profile 仅供取流请求使用；Cookie 不发送到歌词或 CDN。尚未接入凭证有效性校验，不声明 `verify_credentials`，不推断账号会员或购买权限。
- 未接入歌手详情／作品目录、个人歌单、Hi-Res、I3D 等其他加密格式、歌词翻译／逐字／罗马音字段。公共音质选择仍为 PQ／HQ／SQ，尚无独立的“3D 音效”选择参数；不会把 Z3D 冒充 lossless／hires 请求。高品质／无损能否成功取流取决于账号和歌曲权限；实际在线结果见[验收记录](../../../docs/research/migu-acceptance.md)。

## 身份与分页

| 实体 | 公共 ID | 处理规则 |
| --- | --- | --- |
| 单曲 | `contentId` 数字字符串 | `copyrightId`、`songId` 由资源详情取得，仅作为取流参数；校验取流返回的三种身份均一致 |
| 歌单／榜单／歌手 | 原始数字字符串 | 不转换为浮点数，不借用歌曲 ID |
| 普通专辑 | `albumId` 数字字符串 | `resourceType=2003` |
| 数字专辑 | `d:<contentId>` | `resourceType=5`；展开曲目前读取其 `materialId`，不把数字专辑 ID 当普通专辑 ID |

上游搜索固定每页 20 项，分类歌单每页 30 项，歌单／专辑曲目按 50 项请求；榜单接口一次返回完整快照（最多接受 1,000 项），按公共游标切片。公共 `limit` 为 1～50；在固定窗口内切片，读完当前窗口后再进入下一窗口。单次公共响应可以少于 `limit`，客户端应以 `next_cursor` 为准。已知总数与上游窗口长度不符时返回错误，不跳过缺项或假称遍历完成。保留曲目重复位置；榜单空槽单独计入 `unavailable_count`。

集合游标绑定实体、总数和可用的更新时间；推荐与榜单快照额外绑定有序 ID 列表，分类歌单在同一窗口内绑定有序 ID 列表。检测到变化返回 `CURSOR_EXPIRED`，不把过期游标默默移到其他位置。歌单／专辑没有完整版本号时，无法检测总数／时间均未变的所有重排。单集合最多遍历偏移 10,000。

链接支持 `music.migu.cn` 的明确内容 ID 形式：`/v3/music/song/<18位contentId>`、对应的 `/v5/music/` 路径、歌单及普通专辑路径，以及 `/v5/playlist?playlistId=...` 和 `/v5/albumDetail?albumId=...&playlistType=5|2003`。旧版 11 位版权 ID 歌曲链接、短链和含歧义参数的链接不支持；不把版权 ID 当作内容 ID。

## 实际端点

| 用途 | HTTPS 主机与路径 |
| --- | --- |
| 四类搜索 | `pd.musicapp.migu.cn/MIGUM2.0/v1.0/content/search_all.do` |
| 资源详情 | `app.c.nf.migu.cn/MIGUM2.0/v1.0/content/resourceinfo.do` |
| 歌单曲目 | `app.c.nf.migu.cn/MIGUM3.0/resource/playlist/song/v2.0` |
| 专辑曲目 | `app.c.nf.migu.cn/MIGUM2.0/v1.0/content/queryAlbumSong` |
| 公共推荐 | `app.c.nf.migu.cn/pc/bmw/page-data/playlist-square-recommend/v1.0` |
| 分类目录／歌单 | `app.c.nf.migu.cn/pc/v1.0/template/musiclistplaza-taglist/release`、`musiclistplaza-listbytag/release` |
| 榜单目录 | `app.u.nf.migu.cn/bmw/rank/rank-index/v2.0` |
| 榜单内容 | `app.c.nf.migu.cn/bmw/rank/rank-info/v1.0` |
| 音源 | `c.musicapp.migu.cn/MIGUM2.0/v2.1/content/listen-url` |

目录端点核对自[咪咕官网](https://music.migu.cn/v5/)及其前端请求。上游的明文分类 `nextPageUrl` 只用于校验分页含义，由客户端重建 HTTPS 请求。所有接口复用统一超时、取消、限流、重定向和错误处理；不会把原始上游响应、Cookie 或签名音源地址放入错误信息。

官方封面可能是 WebP。公共元数据链路现支持有界的静态 WebP 容器检查、原图嵌入和 `.webp` 导出，拒绝动画、截断和异常块长度；不转码，也不宣称完成像素解码验证。规则依据 [WebP RIFF 规范](https://developers.google.com/speed/webp/docs/riff_container)。播放器是否显示 WebP 内嵌图片取决于播放器本身。

## 构建与验证

新配置默认清单已包含 `migu`；已有 CMake 缓存不会自动增加平台，需要在原有 `MUSIC_SERVICE_PROVIDERS` 清单中追加 `migu` 后重新配置。独立验证示例：

```powershell
cmake --preset windows-msvc-debug -B out/build/windows-msvc-migu-debug "-DMUSIC_SERVICE_PROVIDERS=kuwo;fivesing;netease;qq;migu"
cmake --build out/build/windows-msvc-migu-debug --target music_service_server music_service_unit_tests music_service_integration_tests
out/build/windows-msvc-migu-debug/tests/music_service_unit_tests.exe "[migu]"
python packaging/e2e/migu_e2e.py --binary out/build/windows-msvc-migu-debug/src/server/music-service.exe --report out/migu-live-report.json
```

Windows 先初始化 MSVC 环境，CMake/Ninja 使用该环境内的工具。E2E 自建临时配置与数据目录，通过公共 HTTP/SSE 接口验证目录分页、权限、下载、元数据、Range、复用及重启回读，不读取用户凭证，不输出带签名的音源 URL。具体已通过项目和未覆盖环境见验收记录。
