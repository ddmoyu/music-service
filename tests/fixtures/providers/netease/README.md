# 网易云音乐脱敏样本

2026-10-22 自中国大陆网络通过 HTTPS 匿名请求 `music.163.com/weapi/*` 录得（weapi：AES-128-CBC 两层加密 + 无填充 RSA 加密随机密钥），用于 `tests/unit/providers/netease_test.cpp` 的离线契约测试。匿名样本不含 Cookie、token 或账号信息；标为 synthetic 的样本按真实响应结构手工构造，用于没有真实账号时的登录态契约。

| 文件 | 来源 | 脱敏处理 |
| --- | --- | --- |
| `search-qingtian-o0-l3.json` | `cloudsearch/pc`，type=1，关键词“晴天”，offset=0、limit=3 | 无需处理 |
| `search-empty.json` | 同上，无结果的查询（只有 `songCount: 0`，没有 `songs`） | 无需处理 |
| `detail-5257138.json` | `v3/song/detail`，c=[{"id":5257138}]（《屋顶》，fee=8） | 无需处理 |
| `detail-missing.json` | 同上，不存在的 id：`songs` 为空，privilege 的 st=-200 | 无需处理 |
| `url-5257138-standard.json` | `song/enhance/player/url/v1`，level=standard，匿名可播（128 kbit/s mp3） | 地址路径中的签名置零，查询串 `vuutv` 置为占位值 |
| `url-210049-vip.json` | 同上，fee=1 的会员歌曲，匿名时 code=-110、无地址 | 无需处理 |
| `url-186016-nocopyright.json` | 同上，无版权歌曲（《晴天》原唱），code=404 | 无需处理 |
| `url-missing.json` | 同上，不存在的 id，code=404 | 无需处理 |
| `lyric-5257138.json` | `song/lyric`，lv=kv=tv=-1 | 删除贡献者信息 `lyricUser`／`transUser` |
| `lyric-400876427-translated.json` | 同上，带翻译歌词 | 同上 |
| `lyric-5276808-puremusic.json` | 同上，纯音乐：`pureMusic: true`，lrc 只有“纯音乐，请欣赏”占位 | 同上 |
| `account-anonymous.json` | `w/nuser/account/get`，不带 Cookie（无效 Cookie 的应答相同） | 无需处理 |
| `account-user-synthetic.json` | 构造：已登录账号的 `account`／`profile` 结构 | 编号与昵称均为虚构 |
| `needs-login-synthetic.json`、`rate-limited-synthetic.json` | 构造：weapi 顶层 code 301（需要登录）与 405（操作频繁） | 无 |
