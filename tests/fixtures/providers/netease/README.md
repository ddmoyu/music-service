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
| `collection-playlist.json` | `v3/playlist/detail`，3778678 热歌榜，n=0、s=0；200 个 trackIds | 仅保留集合公共元数据、曲目 ID；creator 仅保留公开昵称 |
| `collection-album.json` | `v1/album/512175`，26 首 | 仅保留公开集合与歌曲元数据 |
| `collection-search-playlists.json` | `cloudsearch/pc`，type=1000，周杰伦，offset=0、limit=2 | 仅保留查询结果所需公共字段 |
| `collection-search-albums.json` | `cloudsearch/pc`，type=10，叶惠美，offset=0、limit=2 | 仅保留查询结果所需公共字段 |

B 阶段集合样本同于 2026-10-22 匿名录得。超过 500 项、空集合、缺失歌曲、顺序变更、错误与容量边界在测试中按相同结构构造；这些构造样本不作为真实平台容量的证明。

C 阶段 `discovery-categories.json`、`discovery-category-page.json`、`discovery-recommendations.json`、`discovery-charts.json` 同于 2026-10-22 匿名实测，分别来自 playlist/catalogue、playlist/list（华语，limit=2）、personalized/playlist（limit=30，fixture 仅留前两项）、toplist（fixture 仅留 3778678）。只保留解析所需公共字段，不保存 Cookie、私人账号或签名音源地址。高品质、普通／会员／购买限制、过期账号与错误组合在测试中按真实字段结构构造；真实 VIP1 下载与文件证据另见补齐计划 C 阶段记录。
