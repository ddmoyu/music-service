# 网易云音乐适配器

原生平台 N07，source `netease`，实现 netease-native 0.1.5。所有请求都是网页客户端的 weapi：`POST https://music.163.com/weapi/<path>`，请求体按网页端算法加密（`protocol/weapi.cpp`，用 Mbed TLS 原生实现 AES-128-CBC、base64 与无填充 RSA），不调用任何外部工具或第三方解析服务。能力及边界如下：

| 能力 | weapi 路径 | 说明 |
| --- | --- | --- |
| `search_tracks` | `cloudsearch/pc`（type=1） | 游标是下一页的 offset；`songCount` 是会随页变化的估计值，空页即结束；最深翻到 offset 10000 |
| `track_detail` | `v3/song/detail` | 歌名、歌手（带 id）、专辑（带 id）、时长；空列表为 NOT_FOUND，应答须是同一 id |
| `lyrics` | `song/lyric`（lv=kv=tv=rv=yv=-1） | 独立 LRC、翻译、罗马音及 YRC 行／词毫秒时间；损坏 YRC 保留有效其他变体并警告；纯音乐不返回占位文本 |
| `parse_links` | 无网络 | `music.163.com/song?id=N`、`/#/song?id=N`、`/song/N`、`/m/song?id=N`、`y.music.163.com/m/song?id=N`；短链 163cn.tv 需要网络，不支持 |
| `resolve_audio` | `song/enhance/player/url/v1` | standard→standard（128 kbit/s MP3）、high→exhigh（320 kbit/s MP3）、lossless→lossless（FLAC）；凭据可选，实际权限由上游决定 |
| `verify_credentials` | `w/nuser/account/get` | `account` 非空为 valid（昵称作为 account_hint，会员时 scopes 含 `vip`），为 null 为 invalid |
| `search_playlists`／`search_albums` | `cloudsearch/pc`（type=1000／10） | 每页最多 50 项，offset 最深 10000；空页结束 |
| `playlist_detail`／`playlist_tracks` | `v3/playlist/detail`，n=0、s=0 | 声明元数据与完整可见 ID 清单；当前页再以 `v3/song/detail` 每批最多 50 ID 补详情 |
| `album_detail`／`album_tracks` | `v1/album/{id}` | 上游返回专辑和歌曲，服务有界切页；不伪造上游分页 |
| `user_playlists` | `w/nuser/account/get`＋`user/playlist` | 必须绑定 Cookie，账号 ID 仅由当前会话取得；每页最多 50，created／subscribed 分类 |
| `artist_tracks` | `artist/{id}` | 热门最多 50 首，total=null，listed_total 为当前热门数量；有序 ID 变化使旧分页失效 |
| `podcast_detail` | `djradio/v2/get` | 电台标题、封面、主播、描述、声明节目数 |
| `podcast_episodes` | `dj/program/byradio` | 正序分页，每页最多 100，total 保留上游 count |
| `episode_detail` | `dj/program/detail` | 节目详情与经过验证的稳定音频引用；购买标记保留 |
| `categories`／`category_playlists` | `playlist/catalogue`／`playlist/list` | 分类目录最多 256 子类；验证目录成员后按 hot 排序请求分类歌单，每页最多 50 项、offset 最深 10000；total 保留上游声明 |
| `recommend_playlists` | `personalized/playlist` | 匿名公共推荐，最多 30 项候选，按短时快照分页；不是账号个性化推荐，total 未知，listed_total 为候选数 |
| `charts`／`chart_detail`／`chart_tracks` | `toplist`，曲目复用歌单与歌曲详情 | 目录最多 256 项，只接受当前榜单目录 ID；曲目按原榜单位置分页，总数不等于单页条数 |

- 匿名与账号：搜索、歌曲详情、歌手热门、歌词、公共发现与链接不发送 Cookie。歌单详情／曲目、节目／电台详情／列表、节目音频的 track_detail 及 resolve_audio 可携带已绑定凭据；个人目录必须有账号。授权方式 `web_cookie` 保持手动 Cookie，秘密头只发往 music.163.com，CDN 不携带 Cookie。所有私有缓存与游标沿用完整身份及选择版本隔离。
- 音源规则：单曲 code 200 才可用；404（无版权或无此歌曲）为 NOT_FOUND；-110（需要会员或购买）为 CREDENTIALS_REQUIRED（带 `netease_song_code`）。应答带 `freeTrialInfo` 的是试听片段，completeness=preview，按公共规则在未允许试听时拒绝。level=standard、类型 mp3、码率 96～160 kbit/s 才声明 standard；无试听标记且有时长时 completeness=full。地址必须在 `*.music.126.net`，一律改用 HTTPS（CDN 以 md5 作强 ETag，支持 Range）；有效期取 `expi`。
- 高品质证据：只有 level=exhigh、MP3、码率 300～340 kbit/s 才声明 high；level=lossless、FLAC、正码率才声明 lossless。采用应答档位，不按请求值填实际档位。strict 拒绝已知降级，allow_lower 保留实际档位并产生 QUALITY_LOWERED；higher／未知或相互矛盾的事实不满足高品质请求。Hi-Res 请求发送 level=hires；仅上游明确返回 hires／FLAC／正码率才声明该档位。落盘后还须满足至少 24 bit／48 kHz，否则降级为 lossless 并重新执行 strict／allow_lower。仅凭 24 bit／44.1 kHz 不改称 Hi-Res。quality_hints 表示支持请求，不保证账号拥有权限。
- 账号验证的 301／403 响应与 account=null 均判 invalid；普通有效账号 scopes 为空，会员有 vip scope，但会员不保证每首歌可用。-110 附上游 `netease_fee`（如有），保留会员／单独购买限制的原始证据，不从 Cookie 存在或 VIP 标记猜测购买权。Cookie 只发往平台 API，下载 CDN 请求不携带 Cookie。
- 顶层 code：301 为 CREDENTIALS_REQUIRED，405 为可重试的限流，-460／-462（风控）与其他为不可重试的 UPSTREAM_FAILED（带 `netease_code`）。
- 集合边界：歌单响应最多 4 MiB／10000 个 ID，专辑响应最多 8 MiB／2000 首；歌曲详情批最多 2 MiB／50 首。列表每页 1～100，保持原顺序与重复位置。`page.total` 为上游声明数，`listed_total` 为可见清单长度，`unavailable_count` 为本页缺详情位置数；空详情页仍向前推进。列表可见不保证歌曲可下载。
- 集合游标含 ID 顺序、声明数与上游曲目更新时间的变化标记，由应用层绑定身份、集合与页大小；可检测的变化返回 CURSOR_EXPIRED，要求重新读取。上游响应和服务页缓存都不是原子快照。
- 集合链接：上述 song 形式同时支持 playlist／album，返回准确 kind。只接受已知主机与默认端口，拒绝歧义 id 参数、伪造域名与 163cn.tv 短链；不请求任意重定向。
- 公共发现始终不携带 Cookie。分类名称作为平台内 ID，“全部歌单”的 ID 为 `全部`；不存在的分类明确 NOT_FOUND，不能让上游默认为全部。榜单 detail／tracks 同样先验证目录成员及 source。
- 推荐上游每次调用可能改变内容。适配器为首次读取保存最多 30 项的快照，按完整身份和页大小分区，最多 64 份、估算 8 MiB、10 分钟，先到期或先进入者淘汰；重启／淘汰后返回 CURSOR_EXPIRED。应用层仍按能力、查询、身份缓存页和发放不透明游标。copywriter 是描述，不冒充创建者。热门曲目可读取热歌榜 `3778678` 的有界页，来源明确为榜单，没有新增“热门搜索”接口。
- 节目音频引用为 `program:<program_id>:<mainSong_id>`。详情及解析重新核对映射，改变或移除返回 NOT_FOUND；音源请求只用 mainSong_id。节目标题写 title，所属电台写 album，主播仅在 Episode.presenter 展示，不写成歌曲 artist。未购买付费节目拒绝；试听沿用 preview 策略。节目当前只声明 standard；缺 level 时须有 MP3／96～160 kbps 证据，时长取节目详情并由下载音频字节复核。
- 链接新增 artist／djradio／program，分别返回 artist／podcast／episode；节目引用不能直接作为歌曲提交，须使用详情的 audio_ref。节目分页是实时目录，不承诺并发删改期间的原子快照；客户端仍须限制页数和提交数。
- YRC 响应上限 2 MiB、10000 行／100000 词、时间范围 0～24 小时，拒绝越界／负时间与行内倒序；`YRC_MALFORMED` 表示跳过损坏部分。`downloads.output.lyrics_variant` 可选 lrc（默认）、verbatim、romanized；缺少所选变体时回退并返回 `LYRICS_VARIANT_FALLBACK`。
- 未包含：扫码／密码登录、订阅同步／定时追更、分段拼接／转码／无限导入。G07.c 请求、降级与字节校验已实现；当前 VIP1 账号所测三首仅返回 lossless，真实 Hi-Res 成功下载仍需有权限的歌曲样本。

目录：`protocol/` 构造请求与 weapi 加密，`parsing/` 解析响应（纯函数，契约测试使用 `tests/fixtures/providers/netease` 的脱敏样本），`capabilities.cpp`、`collections.cpp`、`discovery.cpp` 连接上游客户端，`bundle.cpp` 注册。C 阶段在线复现命令与证据见[计划记录](../../../docs/design/cpp20-refactor/reference-gap-plan.md)。

## 0.1.5 歌手目录

- `search_artists`：WeAPI cloudsearch/pc type=100；`artist_albums`：artist/albums/{id}。每页 1～50，偏移最多 10000；身份不符、缺少 more、空页却声称有后续均为协议错误。
- 复用 `/api/v1/search/artists?sources=netease&q=...`、`/api/v1/artists/albums?source=netease&id=...`；搜索分页位于 `meta.source_results[].page`，列表分页位于 `data.page`，游标继续绑定来源、查询、身份与实现版本。
- 专辑数不取歌手头部的 albumSize（与实际公开列表可能不同）；沿用列表的 more。歌手曲目仍是热门最多 50 首，不冒称全部作品。
