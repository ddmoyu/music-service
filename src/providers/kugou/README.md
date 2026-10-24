# 酷狗原生适配器

`source=kugou`，协议包 `0.1.18`。在 C++20 服务内直接实现协议，复用公共传输、上游额度、缓存、游标、任务、文件与媒体票据。生产运行没有 Python、外部下载程序或第三方解析服务依赖。Python 只用于独立验收。

## 已实现能力

| 功能 | 范围 |
| --- | --- |
| 公共推荐 | 实时公共推荐歌单，保留上游顺序；按移动端固定 30 项窗口拼接请求页，支持跨窗口分页 |
| 分类与榜单 | 官方分类／分类歌单；排行榜列表、详情和分页歌曲；保留平台更新时间 |
| 四类搜索 | 匿名单曲、歌单、专辑、歌手搜索，中文／英文、真实分页、空结果和明确错误 |
| 歌手目录 | 歌手详情、简介、头像与作品数量；按歌手 ID 分页列出歌曲、专辑；未知国籍／生日等结构化字段不从简介中猜测 |
| 详情与集合 | 单曲、歌单、专辑详情；歌单与专辑完整分页；保留重复位置、声明总数与已知不可用条目 |
| 链接 | `www.kugou.com/song/#hash=...`、`/yy/special/single/<id>.html`、`/album/<id>.html`、`/yy/album/single/<id>.html`、`m.kugou.com/plist/list/<id>`、`/singer/info/<id>`、`/yy/singer/home/<id>.html` |
| 元数据 | 按 hash 查找 LRC；官方歌曲／专辑封面；复用标签和歌词、封面独立导出；歌手头像不作为歌曲封面 |
| 音源 | standard 128 kbps MP3、high 320 kbps MP3、lossless FLAC 候选；strict 不降级，allow_lower 只选择同档或更低档；具体歌曲与档位受权限限制 |

共注册 21 项能力。目录保持匿名，`resolve_audio` 支持 optional `web_cookie`，播放解析和下载取流使用同一账号选择。没有启用账号时沿用匿名接口；有 Cookie 时调用官网签名的 `https://wwwapi.kugou.com/play/songinfo`，携带 Cookie 中的登录参数，先由 hash 获取歌曲身份，再取得账号授权的音源。账号取流失败不会改走匿名，也不会换成其他歌曲。

Cookie 取流目前支持 **standard MP3**；high／lossless／hires 的 strict 请求返回 `QUALITY_UNAVAILABLE`，allow_lower 可使用同一账号的 standard。匿名模式仍保留既有 standard／high／lossless 候选。歌曲的会员标记不代表账号已获授权；没有音源返回 `CREDENTIALS_REQUIRED`，只有试听且未指定 `allow_preview=true` 返回 `PREVIEW_REJECTED`。允许试听时仍标记 preview，下载继续执行完整的文件结构校验。

尚未实现登录／会员查询工厂，`supports_verify=false`，登录和会员状态保持 unknown，不能据此判断 Cookie 无效或账号没有 VIP。个人歌单、短链、`gcid_` 云歌单、Hi-Res 和 KGM／加密音源仍未实现。

## Cookie 使用与验证

通过 `POST /api/v1/credentials` 提交 `source=kugou`、`kind=cookie`、`auth_profile=web_cookie`、`secret={"cookie":"<浏览器 Cookie>"}`。新建启用账号会自动选中；再次启用已停用账号时，通过 `PUT /api/v1/credential-defaults/kugou` 提交 credential_id 与新的 secret。账号切换会停用原账号并清除其 Cookie；已受理下载仍绑定原账号，原账号不可用时明确失败，不改用新账号。详见[默认平台凭证](../../../docs/design/cpp20-refactor/backend-service/api-contract.md#111-默认平台凭证)。

Cookie 需包含 `KugooID` 与 `t`（支持官网 `KuGoo` 嵌套字段），以及 `kg_mid`／`kg_dfid`（或网页的 `mid`／`dfid` 字段）。缺少或冲突的登录字段明确失败。Cookie 仅进入 HTTPS songinfo 请求的秘密请求头；该官方 GET 协议还要求在签名参数中包含 token，因此两次请求均禁止重定向，不记录请求 URL、Cookie 或上游正文。目录与音频 CDN 不携带登录 Cookie。

2026-10-24 使用用户提供的 Cookie 实测：林俊杰《江南》《美人鱼》的 `pay` 三项均为 1，匿名解析／下载返回 `CREDENTIALS_REQUIRED`，Cookie 模式返回试听，默认整曲请求为 `PREVIEW_REJECTED`。允许试听后，代理 Range 读取各 65,536 字节成功；试听下载因 MP3 实际帧数少于头部声明返回 `AUDIO_INVALID`，未放宽校验。贝瓦儿歌《两只老虎》在匿名和 Cookie 模式下均完成整曲代理读取、下载和文件回读，产物均为 2,000,980 字节／125,022 ms，SHA-256 相同。此结果验证 Cookie 链路和权限限制，**未验证 VIP 整曲授权成功，也不能推断账号会员等级**。

Windows MSVC Debug 构建通过；平台单元测试 235 通过、4 个在线门控跳过，11,649 个断言通过；酷狗及账号集成测试 32 项、1,748 个断言通过，覆盖启用、切换、停用、秘密更新、排队任务重启与 CDN／日志脱敏。详细证据见[本次验收记录](../../../docs/research/kugou-acceptance.md#cookie-取流与-vip-样本协议包-0118)。未进行 Linux／macOS 或会员高品质整曲验收。

```powershell
python packaging/e2e/kugou_cookie_audio_e2e.py --binary <music-service.exe> --cookie-file <本地秘密文件> --report <结果.json>
```

脚本使用独立临时服务，只输出歌曲、错误码和音频校验证据，不输出 Cookie、账号标识或临时音源 URL。报告明确保留权限／试听及音频结构失败，不把脚本执行完成视为 VIP 整曲下载成功。

能力已实现不等于各档位均已成功下载；当前构建和样本验证结果单列在[酷狗验收记录](../../../docs/research/kugou-acceptance.md)。

## 接入服务

新构建的默认来源清单包含酷狗。已有 CMake 缓存需把 `kugou` 加入当前 `MUSIC_SERVICE_PROVIDERS` 列表；不要覆盖同工作区已增加的其他平台。项目标准构建与测试命令见根 README。

```text
GET /api/v1/search/tracks?sources=kugou&q=两只老虎&limit=20
GET /api/v1/search/playlists?sources=kugou&q=轻音乐&limit=20
GET /api/v1/search/albums?sources=kugou&q=小星星&limit=20
GET /api/v1/search/artists?sources=kugou&q=周杰伦&limit=20
GET /api/v1/artists/detail?source=kugou&id=3060
GET /api/v1/artists/tracks?source=kugou&id=3060&limit=50
GET /api/v1/artists/albums?source=kugou&id=3060&limit=20
GET /api/v1/recommendations/playlists?source=kugou&limit=20
GET /api/v1/categories?source=kugou
GET /api/v1/categories/playlists?source=kugou&category_id=1599:0&limit=20
GET /api/v1/charts?source=kugou
GET /api/v1/charts/tracks?source=kugou&id=8888&limit=20
POST /api/v1/links/parse  {"text":"https://www.kugou.com/song/#hash=89A84D23C85B70E0D7A23F5C427BA075"}
POST /api/v1/downloads  {"track":{"source":"kugou","id":"89a84d23c85b70e0d7a23f5c427ba075"}}
```

使用既有 Bearer、Idempotency-Key、destination 和品质参数契约。默认搜索来源仍由用户配置 `catalog.default_sources` 控制；仅注册酷狗不会改变默认搜索来源。

单曲 ID 是小写 32 位标准音频 hash；搜索和集合统一输出这个身份，链接中的大写 hash 会规范化。解析音源时重新请求该曲官方详情，从 `extra.128hash`／`320hash`／`sqhash` 读取实际映射，不信任客户端拼接的多品质 hash。高品质请求不改公开曲目身份；实际响应必须对应本次候选 hash、容器、码率与时长。

每页 1～50 项，最大起始偏移 10000；超过边界仍有数据时明确报错，不伪装为目录结束。上游按请求窗口推进，短页不会按可见数量推进；没有缺失 ID 的短页不推断不可用数量。专辑曲号只使用上游专辑条目的 `trans_param.sort`。集合游标校验实体、总数和可用的发布／榜单期次字段；推荐另校验第一页的有序身份集合，歌手目录绑定歌手、歌曲／专辑类型、排序与总数。这不是原子快照：上游同数量、同期次重排仍可能需要客户端重新遍历。

歌手搜索使用可返回总数的官方分页接口，未使用固定最多 10 项且忽略分页参数的旧搜索接口。歌手作品保留上游顺序和重复位置；目录可能包含合作或关联作品，只有歌曲／专辑的单一歌手名精确匹配时才附上父歌手引用。头像只用于歌手资料，歌曲备注不作为专辑名。加密候选在 strict 下明确拒绝；allow_lower 可继续尝试普通低档候选，网络和协议错误仍直接返回。

## 协议与资源边界

协议线索取自本地 `music-lib@6353508e84cd357b5bc3acf32d95a16e01bbe20c` 参考源码，生产代码重新实现并以官方实时接口验证。

| 入口 | 用途与传输 |
| --- | --- |
| `songsearch.kugou.com/song_search_v2` | HTTPS 单曲搜索 |
| `complexsearch.kugou.com/v1/search/author` | HTTPS 歌手搜索；按[官网公开 H5 算法](https://staticssl.kugou.com/common/js/min/infSign.min.js)在 C++ 内生成 MD5 签名，无账号秘密 |
| `m.kugou.com/plist/index&json=true` | HTTPS 推荐 |
| `m.kugou.com/app/i/getSongInfo.php?cmd=playInfo` | HTTPS 单曲详情与各品质音源；不尝试绕过付费限制的备用取流分支 |
| `lyrics.kugou.com/search`、`/download` | HTTPS hash 匹配、LRC Base64；不输出访问密钥 |
| `mobilecdn.kugou.com/api/v3/` | 匿名歌单／专辑搜索、详情／曲目、榜单及 `singer/info`、`singer/song`、`singer/album`；仅使用登记的精确 HTTP 路径与 operation |
| `mobilecdnbj.kugou.com/api/v3/tag/` | 匿名分类和分类歌单；仅使用登记的精确 HTTP 路径与 operation |

2026-10-23 实测后两种主机的 HTTPS 证书与主机名不匹配，HTTP 目录可用。沿用项目的明文端点登记机制，保持 TLS 校验，不发送 Cookie 或授权头，不放宽全局传输策略。音频与封面仍要求 HTTPS，音频域名限定在酷狗域下并继续经过公共地址安全检查。临时 CDN 地址只供后端传输，不作为客户端直链。

响应上限 4 MiB、JSON 深度上限 32，数组、标识、文本、时长和分页均有界。错误不带上游正文或签名 URL。整曲时长来自官方独立时长字段；短片段按 preview 处理，最终下载还会校验实际音频内容、品质和时长，不转码或提升品质。

## 验证入口

离线用例：[kugou_test.cpp](../../../tests/unit/providers/kugou_test.cpp)。独立在线客户端会启动临时服务、使用中文下载目录，并在完成后清理临时数据：

```powershell
python packaging/e2e/kugou_e2e.py --binary out/build/kugou-msvc-debug/src/server/music-service.exe --report out/kugou-live-report.json
python packaging/e2e/kugou_artists_e2e.py --binary out/build/kugou-msvc-debug/src/server/music-service.exe --report out/kugou-artists-live-report.json
```

样本在线可用性会变化；客户端对目录超时／限流最多重试两次并记录，权限失败不会按网络故障重试。报告只保存能力、数量、文件摘要和限制，不保存音源 URL 或歌词访问密钥。
