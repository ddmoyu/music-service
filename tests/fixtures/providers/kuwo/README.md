# 酷我脱敏样本

2026-10-20 通过 HTTPS 匿名请求录得，用于 `tests/unit/providers/kuwo_test.cpp` 的离线契约测试。不含 Cookie、token 或账号信息。

| 文件 | 来源 | 脱敏处理 |
| --- | --- | --- |
| `search-qingtian-pn0-rn3.json` | `searchMusicBykeyWord`，关键词“晴天”，pn=0、rn=3 | 请求标识 `UK` 置零 |
| `play-detail-78932517.html` | `play_detail/78932517` 页面 | 只保留标题、canonical 链接、歌曲信息区与页面状态脚本；删除状态中的请求方 IP 与地理位置（`userIpInfo` 置空） |
| `play-detail-missing.html` | 不存在的 rid（上游返回 HTTP 430） | 只保留标题 |
| `audio-78932517-standard.json` | `mobi.s`，br=128kmp3，免费歌曲 | 地址路径中的令牌与查询串、`sig`、`user` 置为占位值 |
| `audio-228908-substitute.json` | 同上，付费歌曲，使用固定 user 值时返回的 11 秒替代短片（另一 rid，type=1） | 同上 |
| `audio-missing.json` | 同上，不存在的 rid（code 4013） | 无需处理 |

上游格式变化时重新录制并更新本表，不手工编造字段。
