# 酷狗原生适配器

`source=kugou`，协议包 `0.1.17`。在 C++20 服务内直接实现协议，复用公共传输、上游额度、缓存、游标、任务、文件与媒体票据。生产运行没有 Python、外部下载程序或第三方解析服务依赖。Python 只用于独立验收。

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

共注册 21 项能力。当前均为匿名接口，不登记 Cookie profile。付费资源明确返回 `CREDENTIALS_REQUIRED`，当前适配器尚不能通过导入 Cookie 解锁；登录、个人歌单、短链、`gcid_` 云歌单、Hi-Res 和 KGM／加密音源尚未实现。没有用其他音源替代受限歌曲。

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
