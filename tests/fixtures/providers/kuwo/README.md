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

集合样本（T23，2026-10-22，HTTPS，每页取 2 条）：

| 文件 | 来源 | 脱敏处理 |
| --- | --- | --- |
| `search-playlist-zhoujielun-pn0-rn2.txt`、`search-album-yehuimei-pn0-rn2.txt` | `r.s` ft=playlist／album，单引号伪 JSON | 请求标识 `UK` 置零 |
| `playlist-2234580349-pn0-rn2.json`、`playlist-missing.json` | `pl.svc` getlistinfo；不存在的 pid 返回另一个空歌单 | 无需处理 |
| `album-1293-pn0-rn2.txt`、`album-missing.txt` | `r.s` stype=albuminfo | 无需处理 |
| `tags.json`、`tag-playlists-1848-pn1-rn2.json`、`recommend-pn1-rn2.json`、`bang-list.json` | wapi 分类、分类歌单、推荐歌单、榜单树 | `reqId` 置零 |
| `chart-16-pn0-rn2.json`、`chart-missing.json` | `ksong.s`（encode=utf8），热歌榜与不存在的榜单 | 无需处理 |

上游格式变化时重新录制并更新本表，不手工编造字段。
