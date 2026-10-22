# 网易云音乐适配器

原生平台 N07，source `netease`，实现 netease-native 0.1.0。所有请求都是网页客户端的 weapi：`POST https://music.163.com/weapi/<path>`，请求体按网页端算法加密（`protocol/weapi.cpp`，用 Mbed TLS 原生实现 AES-128-CBC、base64 与无填充 RSA），不调用任何外部工具或第三方解析服务。只声明 2026-10-22 实测过的能力：

| 能力 | weapi 路径 | 说明 |
| --- | --- | --- |
| `search_tracks` | `cloudsearch/pc`（type=1） | 游标是下一页的 offset；`songCount` 是会随页变化的估计值，空页即结束；最深翻到 offset 10000 |
| `track_detail` | `v3/song/detail` | 歌名、歌手（带 id）、专辑（带 id）、时长；空列表为 NOT_FOUND，应答须是同一 id |
| `lyrics` | `song/lyric`（lv=kv=tv=-1） | LRC 与翻译 LRC；`pureMusic`／`nolyric`／`uncollected` 为 unavailable，“纯音乐，请欣赏”占位不当作歌词 |
| `parse_links` | 无网络 | `music.163.com/song?id=N`、`/#/song?id=N`、`/song/N`、`/m/song?id=N`、`y.music.163.com/m/song?id=N`；短链 163cn.tv 需要网络，不支持 |
| `resolve_audio` | `song/enhance/player/url/v1`（level=standard） | 只实现 standard（128 kbit/s MP3）；凭据可选 |
| `verify_credentials` | `w/nuser/account/get` | `account` 非空为 valid（昵称作为 account_hint，会员时 scopes 含 `vip`），为 null 为 invalid |

- 匿名与账号：搜索、详情、歌词与链接始终匿名，不发送 Cookie。`resolve_audio` 有可用的默认 Cookie 凭据时带上它，否则匿名。授权方式 `web_cookie`：用户提交已登录 music.163.com 的整段 Cookie（须含 `MUSIC_U`），只作为秘密请求头发往 music.163.com，其中的 `__csrf` 同时作为 csrf_token。没有 `MUSIC_U` 的 Cookie 不发请求直接判 invalid。
- 音源规则：单曲 code 200 才可用；404（无版权或无此歌曲）为 NOT_FOUND；-110（需要会员或购买）为 CREDENTIALS_REQUIRED（带 `netease_song_code`）。应答带 `freeTrialInfo` 的是试听片段，completeness=preview，按公共规则在未允许试听时拒绝。level=standard、类型 mp3、码率 96～160 kbit/s 才声明 standard；无试听标记且有时长时 completeness=full。地址必须在 `*.music.126.net`，一律改用 HTTPS（CDN 以 md5 作强 ETag，支持 Range）；有效期取 `expi`。
- 顶层 code：301 为 CREDENTIALS_REQUIRED，405 为可重试的限流，-460／-462（风控）与其他为不可重试的 UPSTREAM_FAILED（带 `netease_code`）。
- 尚未实现：high／lossless 等更高品质（需要会员账号的在线验证）、歌单／专辑／歌手／榜单、歌单与专辑链接、二维码或密码登录。

目录：`protocol/` 构造请求与 weapi 加密，`parsing/` 解析响应（纯函数，契约测试使用 `tests/fixtures/providers/netease` 的脱敏样本），`capabilities.cpp` 连接两者与上游客户端，`bundle.cpp` 注册。
