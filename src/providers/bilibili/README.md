# Bilibili 原生适配器

`source=bilibili` 将公开视频的独立音轨接入公共搜索、解析和下载链路。运行时使用原生 C++ HTTP／JSON，不调用 yt-dlp、FFmpeg、平台客户端或远端解析服务。

## 首阶段能力

| 能力 | 行为 |
| --- | --- |
| `search_tracks` | 视频关键词搜索，每页 1～50 项，游标分页，中文／英文均可；不预先请求每个视频的详情 |
| `track_detail` | 读取指定视频分 P 的标题、实际 cid、时长和封面 |
| `parse_links` | 离线解析 BV 号、`bilibili:BV…` 和 bilibili.com／www.bilibili.com／m.bilibili.com 的 `/video/BV…?p=N` 链接 |
| `resolve_audio` | 取得单个完整 URL 的 DASH AAC 音轨，保存为 M4A，复用公共文件验证、标签、取消、恢复和复用逻辑 |
| `verify_credentials` | 用官方 nav 接口验证 SESSDATA，读取 vipType／vipStatus／vipDueDate 的月度或年度大会员证据；缺失字段为 unknown，不推断具体视频购买权限 |

匿名请求支持上述公开目录和音轨；搜索在缺少 buvid3 时通过官方 SPI 接口取得临时访客 Cookie，仅在当前操作的秘密请求头中使用，不写入凭证库或跨调用方缓存。上游风控、地区和视频权限仍可能限制访问。账号 Cookie 是可选凭证，经现有 credentials 接口保存后使用，不填写在服务配置或命令行里。

本阶段未注册歌词、专辑、歌手、收藏夹、合集、排行榜、推荐和扫码登录。b23.tv 短链、av 号、番剧和互动视频不在本次链接支持范围。多 P 的选择通过 `p=N` 完成，暂不提供浏览全部分 P 的集合接口。

## 身份与元数据

- 搜索项的 `BV…:p1` 是第一 P 的定位别名；`BV…` 也表示第一 P。网页的 `p=N` 映射为 `BV…:pN`。
- 详情与解析结果返回稳定的 `BV…:cid123`。保存实际曲目身份、刷新临时地址及重试时按 cid 精确定位；分 P 排序变化不会把旧 cid 替换为另一 P，已移除的 cid 返回 `NOT_FOUND`。
- 提交下载前必须先读详情的 `ref` 或音源解析的 `actual_track`，将其中的 cid 身份传给 `/downloads`。公共下载队列禁止执行期间改变曲目身份，因此不能直接把搜索／链接中的页码别名交给下载队列。
- UP 主不是演唱者，因此 `artists` 保持空；不把视频名填为专辑，不把页码填为专辑曲号。搜索端点的 duration 可能是多 P 总时长，搜索结果保留时长未知；详情才使用具体分 P 的时长。
- 封面仅接受 hdslb.com 的 HTTPS 图片。公共元数据不包含 Cookie、账号昵称或上游临时下载地址。

## 品质、权限和网络

本阶段只声明 `standard`：在 `dash.audio` 的 30216／30232／30280 中选择带宽最高的受支持 AAC（`mp4a.40.2`／`.5`／`.29`），真实容器仍由公共处理器验证。较高 AAC 带宽并不表示无损，暂不把平台音轨编号映射为公共 `high`／`lossless`／`hires`。严格请求其他档位返回 `QUALITY_UNAVAILABLE`；显式 `allow_lower` 可返回 `standard`。

取流比较视频分 P 时长与返回的音轨时长，拒绝未授权的试听；涉及付费的视频按试听保守处理。缺少视频时长保留 `unknown`。不会把 `durl` 视频、杜比、FLAC／Hi-Res 或分段清单当成已支持的 AAC 文件，也不执行转码。

所有 API 请求共用上游入场、截止时间、取消和 TLS 策略；最多读取 4 MiB JSON，深度不超过 32，列表、分 P、标识及文本有边界。负数平台错误、限流、登录失效、缺失分 P 和无音轨分别报告，不转成空成功。Cookie 只进入 API 的 `secret_headers`，不会送往 CDN。音轨 URL 仅接受 bilivideo.com／bilivideo.cn／bilivideo.net 子域的 HTTPS，并解析 `deadline` 过期时间；下载携带 Referer 与 User-Agent。

2026-10-23 在官方 HTTPS 端点核实的调用链：

| 操作 | 端点 |
| --- | --- |
| 匿名访客初始化 | `api.bilibili.com/x/frontend/finger/spi` |
| 视频搜索 | `api.bilibili.com/x/web-interface/search/type?search_type=video` |
| 视频与分 P | `api.bilibili.com/x/web-interface/view?bvid=…` |
| 独立音轨 | `api.bilibili.com/x/player/playurl?bvid=…&cid=…&fnval=16&fnver=0&fourk=0` |
| Cookie 校验 | `api.bilibili.com/x/web-interface/nav` |

当前实测链路无需 WBI 或固定匿名 Cookie，访客标识由官方接口即时取得。若端点开始要求签名，需单独核实、增加签名向量和回归，不通过隐藏的第三方解析回退掩盖错误。

## 使用与验收

```text
GET /api/v1/search/tracks?sources=bilibili&q=钢琴&limit=20
GET /api/v1/tracks/detail?source=bilibili&id=BV1Mq4y1U7Lw:p1
POST /api/v1/links/parse  {"text":"https://www.bilibili.com/video/BV1Mq4y1U7Lw/?p=1"}
POST /api/v1/assets/resolve  {"track":{"source":"bilibili","id":"BV1Mq4y1U7Lw:p1"}}
POST /api/v1/downloads  {"track":上一步返回的 actual_track}
```

遵守既有 Bearer、Idempotency-Key、destination 和下载参数契约。新平台不会自动加入 `catalog.default_sources`；需要默认参与搜索时显式配置。

已有构建目录需在 `MUSIC_SERVICE_PROVIDERS` 的缓存列表中加入 `bilibili` 后重新配置；只修改 CMake 默认列表不会覆盖用户已有的缓存选择。

离线回归在 [bilibili_test.cpp](../../../tests/unit/providers/bilibili_test.cpp)。在线验收客户端会创建独立临时服务，结束后清理：

```powershell
python packaging/e2e/bilibili_e2e.py --binary out/build/bilibili-msvc-debug/src/server/music-service.exe --multipart-video BV1wPq5BqEau --report out/bilibili-live-report.json
```

可用 `--video BV… --page N` 指定有权访问的样本，`--cookie-file .local-secrets/bilibili-cookie.txt` 增加真实账号校验。未提供 Cookie 时仍执行匿名验收，真实账号能力保留未验证。报告只保留状态、公开视频身份、字节数和 SHA-256，不保存临时 CDN 地址或账号信息。

最终构建、回归和在线证据见 [Bilibili 验收记录](../../../docs/research/bilibili-acceptance.md)。
