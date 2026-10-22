# 5sing 脱敏样本

2026-10-22 通过 HTTP 匿名请求录得（API 主机没有可用的 HTTPS），用于 `tests/unit/providers/fivesing_test.cpp` 的离线契约测试与 `tests/integration/fivesing_gate_test.cpp`。不含 Cookie、token 或账号信息；用户昵称与头像地址为平台公开信息。

| 文件 | 来源 | 脱敏处理 |
| --- | --- | --- |
| `search-qiannianjiu-p1.json` | `home/json`，关键词“千年酒”，page=1 | 无需处理 |
| `search-empty.json` | 同上，无结果的关键词（`list` 为 null） | 无需处理 |
| `newget-fc-16394286.json` | `song/newget`，翻唱 fc-16394286 | `dynamicWords` 换成两行占位 LRC，不保存歌词正文 |
| `newget-yc-1150287.json` | 同上，原创 yc-1150287，无歌词 | 无需处理 |
| `newget-missing.json` | 同上，不存在的作品（code 200062） | 无需处理 |
| `url-fc-16394286.json` | `song/getSongUrl`，只有 hq 与 lq | CDN 地址中的时间段置为 `202610220800`、签名段置零 |
| `url-bz-2493423.json` | 同上，sq、hq、lq 三档 | 同上 |
| `url-bz-3177109.json` | 同上，只有 lq | 同上 |
| `url-missing.json` | 同上，不存在的作品（code 28） | 无需处理 |

录制时用 HTTP Range 读取各档前 200 KB 的 MP3 帧头确认码率：sq 320、hq 192、lq 128 kbit/s；同一档的 primary 与 backup 返回相同长度、相同 Last-Modified，且没有 ETag。上游格式变化时重新录制并更新本表，不手工编造字段。
