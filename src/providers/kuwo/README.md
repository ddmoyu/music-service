# 酷我适配器

首个开发平台，source `kuwo`，实现 kuwo-native 0.1.0。当前只声明 T12 已验证的能力，均为匿名（不登记 Cookie profile，不发送 Cookie）：

| 能力 | 端点（全部 HTTPS） | 说明 |
| --- | --- | --- |
| `search_tracks` | `www.kuwo.cn/search/searchMusicBykeyWord` | pn／rn 分页，游标为页码字符串；每页 1～50；TOTAL 给出总数 |
| `track_detail` | `www.kuwo.cn/play_detail/{rid}` 页面 | 读取隐藏字段的歌名、歌手、专辑与页面状态里的时长；页面须指向同一 rid；430／404 或无歌曲信息为 NOT_FOUND |
| `parse_links` | 无网络 | `www.kuwo.cn`／`kuwo.cn` 的 `/play_detail/{rid}` 与 `m.kuwo.cn/newh5app/play_detail/{rid}` |
| `resolve_audio` | `mobi.kuwo.cn/mobi.s`（br=128kmp3） | 仅标准品质；返回的 CDN 地址限 `*.kuwo.cn` 默认端口，统一改用 HTTPS |

- 所有请求经 `ProviderSetup::upstream`（统一入场、TLS、重定向规则）；无法入场返回可重试的 SERVICE_BUSY，details 给出受限维度。
- 音源证据：返回 rid 与请求不同或 `type` 非 0 时是替代短片，报告为 preview 且不声明品质；同 rid、`bitrate`=128、`format`=mp3 时报告标准品质；`type` 为 0 且时长为正时报告上游声明的整曲，`type` 缺失或无法读取时完整性保持未知（附 full_length_unverified 警告），字节层面的核验在 T15。
- 搜索结果不按标题猜测版本（Live 等），未知字段保持为空。
- 参考实现使用的 `m.kuwo.cn/newh5/singles/songinfoandlrc` 在 2026-10-20 对所测歌曲均返回查询失败，`www.kuwo.cn/api/www/music/musicInfo` 拒绝请求，因此详情改读歌曲页面。高品质、歌词、集合与凭证能力尚未实现，不声明。

目录：`protocol/` 构造请求，`parsing/` 解析响应（纯函数，契约测试使用 `tests/fixtures/providers/kuwo` 的脱敏样本），`capabilities.cpp` 连接两者与上游客户端，`bundle.cpp` 注册。
