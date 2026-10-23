# 千千音乐首阶段 Windows 验收

日期：2026-10-23。`source=qianqian`，首阶段协议包 `qianqian-native 0.1.10`，对应服务二进制版本 `0.1.14`。以下保留首次验收证据；随后 `0.1.11` 的代码审查修复见文末。已完成 13 项匿名能力及 Windows 下载闭环。所有实际请求均未使用 Cookie，生产逻辑为原生 C++20；Python 仅作为独立 HTTP 验收客户端。

## 已接入范围

| 能力 | 结果与边界 |
| --- | --- |
| 单曲搜索、专辑搜索 | 中英文关键词；两类搜索各验证 3 页，每页 3 项，无重复 ID；无结果为成功空页 |
| 单曲详情、歌词、链接解析 | 稳定 TSID、演唱者、专辑、时长、封面；取得 1148 字符 LRC；歌曲、专辑、歌单三种官网链接验证通过 |
| 歌单详情、分页曲目 | 歌单 `309735`，5 页完整读取 24 首，24 个唯一 ID；显式 pageNo／pageSize，避免默认前 20 首截断 |
| 专辑详情、分页曲目 | 专辑 `P10004353346`，2 页完整读取 9 首，9 个唯一 ID；保留顺序与专辑曲目总数 |
| 分类、分类歌单、公共推荐 | 官方实时返回 70 个分类；分类 `2376` 及官网首页热门歌单均验证下一页推进；不宣称个性化推荐 |
| 音源解析 | standard／high／lossless；严格品质不降档，allow_lower 从 Hi-Res 请求返回实际 lossless |

以上对应 13 个已注册能力：`search_tracks`、`search_albums`、`track_detail`、`lyrics`、`parse_links`、`playlist_detail`、`playlist_tracks`、`album_detail`、`album_tracks`、`categories`、`category_playlists`、`recommend_playlists`、`resolve_audio`。

歌单第二页第 10 个位置 `T10064316227` 在官方响应中缺少 `artist` 字段。适配器保留曲目及位置，歌手列表留空；缺失和 null 都有回归覆盖，错误字段类型仍明确失败。演唱者仅取 `artistType=38`，作词／作曲人员不写入歌手。

## 三档真实下载

匿名样本为许嵩《心安之地》`T10065435650`。三档均完成解析、持久任务、真实字节校验、标签处理、票据读取、幂等重放、完成结果复用和服务重启后的 SHA-256 回读。下表大小与摘要对应最终带标签文件，包含嵌入封面，不能当作上游裸音频大小。

| 请求与实际品质 | 格式 | 最终字节数 | SHA-256 |
| --- | --- | ---: | --- |
| standard | MP3 128 kbps | 8,272,822 | `cf632b35e883ada214bd9451bcd93459688bf4a53caa7512d7921b58383db102` |
| high | MP3 320 kbps | 14,563,894 | `49280403e088e3b9bb2201d6d4038bfc0ce66be85a7b1f77683c9cc60b326da6` |
| lossless | FLAC | 62,676,304 | `79b8800067169c138d940576165ccf0d69e0bd5fb121c7e0855616c645c33144` |

三档任务均报告 title、artist、album、track_number、lyrics、cover、lyrics_file、cover_file 写入成功。在线客户端直接解析 MP3 标题及歌词帧，比较三档内嵌封面与导出图片的完整字节；FLAC 通过服务的公共音频校验。所有音频、封面和歌词侧文件在重启后逐项回读并核对摘要；SSE 完成事件通过。

封面 SHA-256 为 `f350c17e66a328afe2829df4e3f44ff8382d1171ceacc01c6f36657f775ec1c5`，LRC 为 `10c9971c6e72db8d99e6a4e11e52f55f14a0cce4b793a3e3b286b5506b68ec72`。三档均一致。

原始在线报告位于本机忽略目录 `out/qianqian-live-report.json`，开始时间为 `2026-10-23T06:44:03Z`，最终 `status=passed`。报告不保存 Cookie、票据或带签名的音源直链。独立临时服务与下载目录在验收结束后清理。

## 权限和未接入项

- VIP 样本 `T10064935558` 返回权限限制，不能匿名完成整曲下载；不会静默换来源或换歌曲。
- 不存在的歌曲、专辑、歌单 ID 均返回 `NOT_FOUND`。严格 Hi-Res 返回 `QUALITY_UNAVAILABLE`，允许降档才返回实际可用品质。
- 歌单搜索 `search type=6` 的官方匿名响应为 `23001`，提示不支持该搜索类型，因此未注册。公共 API 对未接入能力明确返回 `CAPABILITY_UNSUPPORTED`。
- 榜单、歌手实体目录、个人歌单、Cookie／扫码登录、付费权限获取、Hi-Res 均未接入。
- 试听识别、默认拒绝及显式允许预览的分支有合成协议测试；本次没有取得可用于正向试听下载的真实样本，不把合成测试作为线上试听成功证据。
- 分页每页 1～50 项、最大起始偏移 10000，专辑最多 2000 个位置。专辑游标核对完整曲目摘要；歌单、推荐／分类分页不能检测总数不变的所有重排，不提供原子快照。
- 本次仅验证 Windows x64 Debug。Release、Linux、macOS、Docker／NAS 和真实账号授权未专项验收。公开目录和播放权限可能随平台变化。

## 官方协议核对与修复

依据[千千官网](https://music.91q.com/)当前网页脚本和匿名官方接口逐项验证，仓库参考实现仅作协议线索。端点为 `https://music.91q.com/v1/` 下的 `search`、`song/info`、`song/tracklink`、`album/info`、`tracklist/info`、`tracklist/category`、`tracklist/list`。

签名按键排序原始 UTF-8 参数，加上官网公开常量后计算 MD5，再编码查询参数。中文、空格、`&`、`+` 和固定时间戳向量使用独立 Python hashlib 计算并验证。错误签名 `22001` 与不存在 `23010` 分别处理，`23001` 不一律误报登录失效。每次请求生成新时间戳；官方仍接受正确签名的旧时间戳，不能声称上游强制了某个签名时效窗口。

官方接口忽略 `Accept-Encoding: identity` 仍返回 gzip，初次真实服务验证因公共 HTTP 库未启用自动解压而失败。已增加通用的可选 `HttpRequest::preserve_content_encoding`，千千适配器取回原始压缩字节后使用固定依赖 zlib 解码。压缩体和解压结果分别受限，校验 CRC、截断和尾部数据；每个输出块检查取消与期限。默认传输行为保留，音频下载不启用此选项。

JSON 最大 4 MiB、32 层，歌词最大 256 KiB。资源仅接受经过验证的官方 HTTPS 地址；不转发账号信息，不在错误中包含响应正文或临时音源地址。上游格式与码率仅用于候选选择，下载仍由公共 MP3／FLAC 验证器核对实际字节。

## 构建与测试证据

通过 `vswhere -prerelease` 发现 Visual Studio Insiders，初始化 `vcvars64.bat`，使用 MSVC x64 Debug。为避免同时进行的平台任务互相占用产物，使用独立构建目录 `out/build/qianqian-msvc-debug`，选择 `kuwo;fivesing;netease;qq;qianqian` 五个平台；本结论不覆盖其他聊天正在修改的平台。依赖使用本机已有固定版本源码，构建产物独立。

- MSVC 构建完成；本轮千千及传输改动编译无警告。
- 千千离线测试：19 个用例、961 个断言通过，覆盖签名、官方目录样本、分页／身份、权限／试听、品质降档、有效期、错误语义、边界、取消与 gzip 完整性。
- 公共传输 gzip 测试：1 个用例、31 个断言通过，覆盖显式保留字节、响应头、压缩体上限及重定向。
- 选定五个平台的最终 CTest：441 项中 435 项通过、6 项跳过、0 项失败；耗时 159.24 秒。跳过的是依赖额外环境开关或外部样本的 5sing／酷我／网易云在线探针、真实 MP3 样本及公共 HTTPS 探针；千千在线验证由独立客户端完成。
- 独立在线客户端完整通过，见上述目录与下载结果。

最初并行 CTest 遇到已有 secret-store 用例固定临时目录的共享占用；改用本次独立 TEMP／TMP 并串行执行，没有为该冲突修改生产凭证逻辑。

复现入口（先初始化 MSVC；平台列表可追加到自己现有的构建选择）：

```powershell
cmake --preset windows-msvc-debug -B out/build/qianqian-msvc-debug "-DMUSIC_SERVICE_PROVIDERS=kuwo;fivesing;netease;qq;qianqian"
cmake --build out/build/qianqian-msvc-debug -j 4
New-Item -ItemType Directory -Force out/qianqian-test-temp | Out-Null
$env:TEMP = (Resolve-Path out/qianqian-test-temp).Path
$env:TMP = $env:TEMP
ctest --test-dir out/build/qianqian-msvc-debug --output-on-failure -j 1
python packaging/e2e/qianqian_e2e.py --binary out/build/qianqian-msvc-debug/src/server/music-service.exe --report out/qianqian-live-report.json
```

代码及用法见[适配器说明](../../src/providers/qianqian/README.md)、[离线样本](../../tests/fixtures/providers/qianqian/README.md)和[在线客户端](../../packaging/e2e/qianqian_e2e.py)。新构建默认注册千千；已有 CMake 缓存需追加 `qianqian` 并重新配置，聚合搜索默认来源另由 `catalog.default_sources` 控制。

## 0.1.11 代码审查修复

2026-10-23 按“审查代码并直接修复”进行第二轮检查，覆盖签名、目录解析、分页、音源品质／权限／试听、歌词、压缩边界以及公共传输衔接。确认并修复以下问题：

| 问题 | 证据与影响 | 修复 |
| --- | --- | --- |
| 歌单位置误作专辑曲号 | 官方歌单中 `T10064900866`、`T10064949966`、`T10064316227` 的 sort 分别为 2、3、10；再次请求各自 song/info，专辑曲号均为 1。旧解析使列表与单曲详情互相矛盾 | 歌单读取不再使用 sort 填充 track_number；保留原数组顺序。单曲详情、搜索及专辑中的专辑曲号继续保留 |
| 纯文本歌词被拒绝，部分错误页被当成歌词 | `T10064949966` 的官方 .txt 返回 UTF-8 纯文本，无 LRC 时间戳，旧代码因缺少 `[` 失败；同时大写 HTML、XML／JSON 错误体只要含 `[` 就可能被接受 | 接受无时间戳文本，不伪造时间；空白正文视为无歌词；识别大小写不同的 HTML、XML 和 JSON 错误文档，保留大小与 UTF-8 限制 |
| 试听资源读取了外层有效期 | 代码已选用 trail_audio_info 的地址、格式与时长，却仍使用外层 expireTime。合成回归复现了过期试听被接受、有效试听被误拒绝两种相反错误 | 优先使用实际选中资源的 expireTime；试听未单列期限时才沿用外层期限 |

新增 3 个审查回归用例，修复前 3 项均失败、共 10 个失败断言。歌单序号及纯文本歌词另以官方匿名响应核对；试听有效期是合成协议边界验证，本轮仍未取得真实正向试听下载样本。协议包升至 `0.1.11`，隔离旧版本目录缓存与游标；不新增平台能力或账号功能。

修复后的 MSVC x64 Debug 构建通过，无编译警告。千千专项 **22 个用例／1136 个断言通过**；传输专项 **14 项通过、301 个断言通过**，公共 HTTPS 探针因未开启 `MUSIC_SERVICE_ONLINE_TESTS` 跳过 1 项。本轮未重复全仓库 CTest，也未覆盖其他并行开发平台；上方 441 项记录是首阶段历史基线。

最终匿名在线复测使用服务 `0.1.17`，报告为 `out/qianqian-review-live-report.json`，开始时间 `2026-10-23T07:09:38Z`，结果 `passed`。新增检查确认歌单 5 页均没有把位置填作专辑曲号，官方纯文本歌词返回 314 字符；原有搜索、70 个分类、24 首歌单／9 首专辑、三档音频下载、标签与侧文件、幂等复用、SSE 和重启回读全部通过。独立临时服务已清理；代码未提交。
