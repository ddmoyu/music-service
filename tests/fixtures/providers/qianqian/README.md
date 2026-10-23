# 千千官方公开响应样本

2026-10-23 在本机匿名请求 `https://music.91q.com/v1/` 采集；仅用于离线解析回归。保留 JSON 数据字段和顺序，紧凑格式保存。没有 Cookie、账号数据或带签名的音源直链。

| 文件 | 请求 |
| --- | --- |
| search-track.json | search，word=许嵩，type=1，pageNo=1，pageSize=3 |
| search-album.json | search，word=许嵩，type=3，pageNo=1，pageSize=3 |
| empty.json | search，无匹配关键词，type=1，pageSize=3 |
| album.json | album/info，albumAssetCode=P10004353346 |
| playlist-p1.json | tracklist/info，id=309735，type=0，pageNo=1，pageSize=3 |
| playlist-p2.json | tracklist/info，id=309735，type=0，pageNo=2，pageSize=5；第 10 个位置缺少 artist 字段 |
| categories.json | tracklist/category |
| category.json | tracklist/list，subCateId=2376，pageNo=1，pageSize=3 |
| playlists.json | tracklist/list，pageNo=1，pageSize=3；官网首页热门歌单入口 |
| search-track.json.gz | 对公开 search-track.json 使用 Python gzip、mtime=0 生成，覆盖官方压缩响应 |
| oversized.json.gz | 合成压缩样本，解压后为 4 MiB + 1 字节，用于验证解压上限 |

签名向量使用固定时间、中文、空格、`&` 和 `+`，由独立 Python hashlib 计算期望值。权限、试听、身份错配、到期、分页变化、取消、网络及资源边界在单元测试中构造，明确属于合成响应，不当作真实账号或试听样本的验收证据。
