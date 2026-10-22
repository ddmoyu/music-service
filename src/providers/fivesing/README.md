# 5sing 适配器

第二个原生平台（T22），source `fivesing`，实现 fivesing-native 0.1.5。只声明 2026-10-22 实测过的能力，均为匿名（不登记 Cookie profile，不发送 Cookie）。B 阶段补充 `5sing.kugou.com/{userId}/dj/{songListId}.html` 歌单链接，引用取 songListId，不把 userId 当集合 ID；沿用现有集合详情与分页。

| 能力 | 端点 | 说明 |
| --- | --- | --- |
| `search_tracks` | `http://search.5sing.kugou.com/home/json` | 上游每页固定 10 条、page 从 1 开始；每次调用只请求一个上游页，游标为 `页码` 或 `页码:偏移`，limit 小于 10 时在同一页内续读；totalCount 给出总数 |
| `track_detail` | `http://mobileapi.5sing.kugou.com/song/newget` | 歌名、上传演唱者、专辑名（多为空）；应答须是同一作品编号与类型 |
| `lyrics` | 同上，`dynamicWords` | 保留定时 LRC 与纯文本歌词；仅空白内容为 unavailable |
| `parse_links` | 无网络 | `5sing.kugou.com/{yc,fc,bz}/{编号}.html`，http 或 https |
| `resolve_audio` | `http://mobileapi.5sing.kugou.com/song/getSongUrl` 与 CDN 的 HEAD | standard＝lq（128 kbit/s，缺 lq 时用 hq 192 kbit/s），high＝sq（320 kbit/s），均为 MP3；无 lossless |

- 作品身份：同一编号在 yc（原创）、fc（翻唱）、bz（伴奏）下是不同作品，platform id 写作 `<类型>-<编号>`（如 `fc-16394286`）。类型同时写入 version：fc 为 cover，bz 为 instrumental，yc 只标“原创”标签、kind 保持 unknown。翻唱不被当作原唱录音，原唱者（originSinger）不写入演唱者。
- 明文端点：两个 API 主机的 HTTPS 端口出示的是其他域名的证书，只能用 HTTP。bundle 按主机、80 端口、路径前缀与 operation 精确登记 7 个明文端点（`/home/json` 用于 search_tracks 与 search_playlists，`/song/newget` 用于 track_detail 与 lyrics，`/song/getSongUrl` 只用于 resolve_audio，`/song/getsonglist` 用于 playlist_detail，`/song/getsonglistsong` 用于 playlist_tracks）；请求不带任何秘密，传输层拒绝其他路径、operation 或附带 Cookie 的明文请求。音频地址只接受 HTTPS、`*.kugou.com`、默认端口。
- 主备地址：每档有 primary 与 backup。先对 primary 发 HEAD；只有当两者的文件名都等于该档的 `<md5>.<ext>`（同一文件）时，primary 无应答、非 200 或长度不符才改用 backup；backup 指向其他文件时从不尝试。入场受限（SERVICE_BUSY）、已停止或超出期限时不再试 backup。HEAD 长度等于该档声明大小时报告 completeness=full（5sing 提供上传的整个文件，没有试听片段）；长度未知时保持 unknown。续传沿用公共规则：CDN 只给 Last-Modified，主备返回相同的 Last-Modified 与相同字节。
- 时长：搜索、详情与取流都不提供时长，始终为未知；不用 songSize 估算。品质与码率来自档位；码率依据录制样本的 MP3 帧头（sq 320、hq 192、lq 128 kbit/s），扩展名不是 mp3 时不声明品质与码率。
- 歌单（T23）：`search_playlists` 用 `home/json` type=1（每页固定 15 条，游标 `页码[:偏移]`）；`playlist_detail` 用 `song/getsonglist`（“暂无简介”视为无简介，code 24 为 NOT_FOUND）；`playlist_tracks` 用 `song/getsonglistsong`，上游忽略分页参数、一次返回整张歌单，游标是偏移量，total 为当前列出的曲目数（歌单声明的数目可能含已删除作品）。网页版歌单会截断长标题，不使用。code 13（操作太频繁）为可重试的限流。
- 参考项目的榜单、分类、个人歌单明确返回 unsupported；未提供专辑、歌手与登录协议，本包同样不声明这些能力。歌单链接已在 B 阶段完成。5sing 的最高已验证档位为 MP3 320 kbps，不虚构无损音源。
- 0.1.5 修复原先丢弃纯文本歌词的问题，并在发布文件前按实际 MP3 帧码率复核品质；真实 bz-2493423 下载为 320000 bit/s，重启后回读哈希一致。

目录：`protocol/` 构造请求与明文端点清单，`parsing/` 解析响应（纯函数，契约测试使用 `tests/fixtures/providers/fivesing` 的脱敏样本），`capabilities.cpp` 连接两者与上游客户端，`bundle.cpp` 注册。
