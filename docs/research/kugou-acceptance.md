# 酷狗接入与 Windows 验收

日期：2026-10-23。范围为酷狗原生适配器的匿名发现、四类搜索、歌手与集合目录、歌词、封面和下载链路。与同工作区其他平台的修改分别记录。

## Cookie 取流与 VIP 样本（协议包 0.1.18）

日期：2026-10-24。新增 optional `web_cookie`，播放和下载共用账号取流实现；目录继续匿名。下面是本次实现与验收，后文 0.1.17／0.1.10 是当时的历史范围。

协议核对来自[官网播放器](https://www.kugou.com/yy/static/js/play/playbyAudio.js)、[网页账号参数](https://staticssl.kugou.com/common/js/min/npm/getBaseInfo.min.js)和[H5 签名实现](https://staticssl.kugou.com/common/js/min/infSign.min.js)。直接给旧 getSongInfo 接口添加 Cookie，对 VIP 样本仍返回 status=0／空 URL；新实现按官网使用 HTTPS songinfo，先提交 hash，再提交返回的 encode_album_audio_id，保持两次歌曲身份一致。Cookie 的用户 ID、token 和设备参数参与签名，未构造或购买任何权益。

Cookie 仅发送到 songinfo 的秘密请求头；官方 GET 协议中的登录 token 保留在私有请求参数中，两次请求均禁止重定向。URL、Cookie 和上游正文不进入 API 响应、错误或日志，CDN 请求不携带账号 Cookie。全链路使用既有身份、凭证租约、超时与额度机制，没有新增依赖。绑定账号缺少登录字段时直接失败；已选账号的上游失败不重试匿名。

目前只确认官网账号 standard MP3，strict 高档请求明确失败；allow_lower 仍使用原账号请求 standard。匿名接口保留已有三档候选。没有接入登录／会员验证工厂，两个状态为 unknown，不能由此判断用户是否拥有会员。

### 本次在线样本

使用用户提供的 Cookie，在独立临时服务中执行真实 API、代理 Range 读取、后台下载与文件回读。Cookie 只存于已忽略的本地秘密文件，记录不含账号信息或音源 URL。

| 样本 | pay_play／pay_down／pay_month | 匿名 | 使用所提供 Cookie |
| --- | --- | --- | --- |
| 林俊杰《江南》，b83dbb8e3dd3fcf86bf32690b91547f5 | 1／1／1 | 整曲解析和下载均 CREDENTIALS_REQUIRED | 官网 is_free_part=1；默认整曲解析／下载均 PREVIEW_REJECTED |
| 林俊杰《美人鱼》，cdae8c7e2faf7f08dc18e78a1d65117b | 1／1／1 | 整曲解析和下载均 CREDENTIALS_REQUIRED | 官网 is_free_part=1；默认整曲解析／下载均 PREVIEW_REJECTED |
| 贝瓦儿歌《两只老虎》，89a84d23c85b70e0d7a23f5c427ba075 | 0／0／0 | 整曲代理读取、下载及文件回读成功 | 整曲代理读取、下载及文件回读成功，未跨账号复用 |

两首 VIP 歌显式 allow_preview=true 后，代理均读取到 65,536 字节，解析结果仍为 preview。试听下载均返回 AUDIO_INVALID，原因是“实际音频帧数少于文件头声明”，因此没有发布试听文件；没有放宽 MP3 完整性校验。

免费歌两种模式产物均为 2,000,980 字节，处理器测得 125,022 ms，SHA-256 为 `903b2b16004cd12dd73b254e8bd73f7137013e1207afa4fc0c75e16e4fcf6daf`。全部停用账号后，VIP 样本恢复匿名的 CREDENTIALS_REQUIRED。API 与运行日志的 Cookie／token 检查通过。

**本次确认了账号参数接入和权限／试听处理，没有验证 VIP 整曲授权成功，也不能凭该结果断言 Cookie 过期、账号没有 VIP 或平台全部会员歌不可用。** 高品质账号取流、会员等级与到期时间、Linux／macOS 本轮未验收。

### 构建和自动化验证

- Windows MSVC x64 Debug 服务、单元及集成目标构建通过。共享集成测试 EXE 曾被另一项测试占用，期间使用相同对象／库链接独立测试 EXE；其结束后标准 CMake 目标与测试已复核，不停止其他任务。
- 酷狗单元测试 44 项、1,927 个断言通过，包含签名复用、Cookie 字段、账号交替、身份错配、试听、音质策略、失败／取消／限流和秘密隔离。
- 平台单元测试共 239 项：235 通过、4 个在线门控跳过，11,649 个断言通过。
- 酷狗及账号集成测试 32 项、1,748 个断言通过；其中新增酷狗场景验证创建／选择账号、切换后清除旧秘密、旧排队任务重启后明确失败、重新启用和替换 Cookie、显式账号下载、全部停用后匿名、不同账号不复用结果及 CDN／日志无秘密。
- 在线脚本 [`kugou_cookie_audio_e2e.py`](../../packaging/e2e/kugou_cookie_audio_e2e.py) 完成上述三首歌的两种模式对照。执行完成表示证据收集与限制断言通过，不能等同于 VIP 整曲下载成功。

本地证据：`out/kugou-cookie-final-build.log`、`out/kugou-cookie-unit.log`、`out/kugou-cookie-providers-unit.log`、`out/kugou-cookie-final-integration.log`、`out/kugou-cookie-audio-live.json`、`out/kugou-cookie-audio-live.log`。正式 API 使用说明见[酷狗适配器](../../src/providers/kugou/README.md#cookie-使用与验证)。

## 歌手目录与审查修复（协议包 0.1.17）

按用户要求新增歌手搜索、详情、歌曲与专辑分页，能力从 17 项增至 21 项；增加歌手链接解析，复用公共路由、身份、游标和下载链路。本次使用独立构建目录 `out/build/kugou-msvc-debug`，配置为 `kuwo;fivesing;netease;qq;kugou`。两组在线验收使用应用 0.1.17；随后工作区同期代码更新，增量构建应用 0.1.18 并复测失败项与酷狗用例。酷狗协议包版本均为 0.1.17。

| 项目 | 实现与实际结果 |
| --- | --- |
| 签名与搜索 | 根据[官网公开 H5 签名实现](https://staticssl.kugou.com/common/js/min/infSign.min.js)，在原生 C++ 中排序参数、计算 MD5 并编码 URL；固定签名向量通过。使用 `https://complexsearch.kugou.com/v1/search/author`，无需账号，未采用忽略页码且最多 10 项的旧接口 |
| 歌手搜索在线验证 | “周杰伦”连续 3 页，每页 2 项，返回总数 29，页间身份不重复；英文搜索与不匹配关键词的空结果通过 |
| 歌手资料 | 薛之谦 `3060` 与周杰伦 `3520` 的身份、简介、头像和作品数量通过；不存在的 `999999999` 返回 NOT_FOUND。出生日期、国籍等没有独立字段，保持未知 |
| 完整歌曲目录 | 薛之谦共 20 页／963 个条目，961 个不同标准 hash，保留两个重复位置；总数与上游一致，已知缺失项 0 |
| 完整专辑目录 | 薛之谦共 5 页／47 张专辑，47 个不同 ID；总数与上游一致 |
| 游标与缺失语义 | 游标绑定歌手、目录类型、排序和总数；跨歌手误用被拒绝。接口 `timestamp` 是请求时刻，不作为版本；同数量重排仍不是原子快照。短页不能推断未知的不可用数量 |
| 歌手目录到下载 | 从贝瓦儿歌 `283640` 目录取得《两只老虎》的标准 hash，完成原生整曲解析与下载；标题、歌词和专辑封面内嵌，独立 LRC／封面、结果复用与重启哈希回读通过 |
| 构建与离线回归 | Windows MSVC x64 Debug 构建通过；酷狗 37 项测试／1,071 个断言通过，包含新增 14 项签名、歌手目录与审查问题回归 |
| 原有功能在线复测 | 三类原有搜索各 3 页、55 个榜单、18 个分类／分组、公共推荐总量 600；76 首歌单／45 首专辑完整遍历；标准整曲、标签／侧文件、复用及重启回读通过，无网络重试 |
| 五平台全量回归 | 首轮共 466 项：453 通过、6 按配置跳过、7 失败。失败为 6 项应用令牌测试及 1 项公共负载故障测试，酷狗用例全部通过 |
| 增量构建后复测 | 应用 0.1.18 增量构建通过；上述 7 项全部通过，用时 15.83 秒；酷狗 37 项／1,071 断言再次通过。未将这次定向复测描述为重新运行完整 466 项 |

审查覆盖现有及新增酷狗适配器的协议签名、身份绑定、分页、元数据来源、音质与权限、资源地址和解析边界，发现并修复以下三项问题：

| 问题 | 修复与验证 |
| --- | --- |
| 歌曲 `remark` 被当成缺失专辑名 | 备注可能是“电视剧插曲”等来源说明，现只读取真实专辑名字段；未知专辑保持空。回归验证备注不进入专辑标签 |
| 触及 10000 偏移上限时误报目录结束 | 若上游仍有数据，现返回明确的分页边界错误，不返回假的 `has_more=false`；边界回归通过 |
| 不支持的加密候选阻断 `allow_lower` | 明确允许降级时继续尝试同曲低档候选；strict 仍拒绝。后续网络错误直接返回，不被降级吞掉；两类回归通过 |

下载音频与两个侧文件的 SHA-256 和下方首阶段样本一致。该验收不扩大高品质／无损成功样本、Cookie、个人歌单、Hi-Res 或加密音源的支持范围。歌手资料中的头像与歌曲专辑封面分别处理，不把合作或关联作品全部改写为父歌手。

歌手验收脚本为 [`kugou_artists_e2e.py`](../../packaging/e2e/kugou_artists_e2e.py)。本地证据：`out/kugou-artists-build.log`、`out/kugou-artists-refresh.log`、`out/kugou-artists-unit-final.log`、`out/kugou-artists-live-report.json`、`out/kugou-review-live-report.json`、`out/kugou-artists-ctest.log`、`out/kugou-artists-ctest-retry.log`。两组在线验收无网络重试；报告不包含账号或临时音源地址。

首轮全量测试执行时，应用令牌测试与实现文件的修改时间晚于对应对象文件；增量构建后这 6 项通过。公共 T32 负载故障测试首轮触发 90 秒期限，串行复测用时 12.40 秒通过，保留这次超时记录。本轮未改动这些公共模块或放宽测试阈值。6 项跳过为显式启用的在线探测及真实文件样本，酷狗真实协议由两组独立在线客户端覆盖。

## 已核实协议

官方 HTTPS 单曲搜索、移动端推荐、单曲详情、歌词查询／下载均可匿名访问。官方移动 CDN 的歌单／专辑搜索、详情、歌曲分页、分类及榜单可通过 HTTP 访问；HTTPS 返回主机名不匹配的证书。生产适配器登记匿名目录的精确 HTTP 路径，保持证书校验。

单曲详情 `extra` 提供标准、高品质和无损的 hash、整曲时长等映射，因此公共曲目 ID 只使用小写标准 hash，不把多品质 hash 拼接进身份。付费样本返回 `status=0`、无音源；可访问的样本返回 HTTPS 文件地址。未采用 Tracker 等备用路径绕过权限结果。

## 首阶段验收（协议包 0.1.10）

原生模块、离线协议回归和在线闭环已通过 Windows MSVC x64 Debug 验收。为避开同工作区其他构建的文件占用，使用独立目录 `out/build/kugou-msvc-debug`；依赖源码复用已校验的固定版本，构建产物独立。本次配置选择 `kuwo;fivesing;netease;qq;kugou`，不代表同时开发中的其他新增平台已经验收。

| 项目 | 结果 |
| --- | --- |
| 构建 | MSVC x64 Debug 构建通过，编译器 19.51，酷狗协议包 0.1.10；在线样本使用应用版本 0.1.11，随后按工作区当前源码完成的 0.1.14 增量构建也通过 |
| 酷狗离线协议回归 | 23 项／652 个断言全部通过；覆盖三类搜索、空／短页、跨 30 项推荐窗口、重复位置、缺失 hash、身份错配、游标过期、权限、品质、试听、加密标记、非法域名、歌词及有界解析 |
| 五平台全量回归 | 上述五平台配置、应用版本 0.1.14：CTest 共 445 项，439 项通过、6 项按配置跳过、0 失败，耗时 170.22 秒；使用独立 TEMP／TMP 和 `-j 1` |
| 搜索 | 中文单曲／歌单／专辑各验证 3 页，每页 2 项；英文歌曲搜索有结果；不匹配关键词返回正常空结果 |
| 发现与榜单 | 55 个榜单、18 个分类／分组；公共推荐声明总量 600，验证 20 项×2 页跨上游窗口；分类及两个榜单分别验证两页 |
| 完整集合 | 推荐歌单 `9839115` 共 76 首／2 页；专辑 `37376237` 共 45 首／9 页；可见数量与上游声明一致，无已知缺失项 |
| 实际下载 | 贝瓦儿歌《两只老虎》，标准 128 kbps MP3；小写 hash `89a84d23c85b70e0d7a23f5c427ba075`；发布文件 2,064,462 字节 |
| 元数据 | 标题、歌手、专辑、歌词与封面写入；直接检查 MP3 ID3 标题、APIC、USLT；独立 LRC 与封面可通过文件票据读取 |
| 复用与重启 | 相同完成请求复用原任务；重启后重新签发文件票据，音频及两份侧文件的 SHA-256 均保持一致 |
| 品质与权限 | 样本没有 high／lossless 候选，严格请求明确 QUALITY_UNAVAILABLE，allow_lower 返回 standard；付费样本返回 CREDENTIALS_REQUIRED；严格 Hi-Res 明确不支持 |

发布音频 SHA-256：`6f5b71cdf092dd179cfd1b689b8b515c35a816c020179e12fdb1a7855fed7ad7`。

独立 LRC SHA-256：`c429e18c304093f6e968d93ffdf89c360c9f6ce0269a2bf627ea7dc655751178`；独立封面 SHA-256：`ed1b82af5fac76af329e832d80d314e8aa7b13b0dfb4363fc1f50ad0f7606bb5`。

高品质与无损的候选、hash 匹配和 strict／allow_lower 逻辑已实现并通过离线用例，**成功高品质／无损下载仍待可访问样本**。Cookie 登录、个人歌单、Hi-Res、云歌单和加密音源仍未实现，不据标准品质闭环扩大支持结论；歌手目录已在上述 0.1.17 追加实现。

本地原始证据位于 Git 忽略的 `out/kugou-isolated-configure.log`、`out/kugou-isolated-build.log`、`out/kugou-rebuild.log`、`out/kugou-unit.log`、`out/kugou-ctest-final.log`、`out/kugou-live-report.json`。最终在线验收无网络重试，不含账号、Cookie 或签名音源地址。

首次共享目录构建曾遇到其他构建占用对象文件；一次在线启动早于链接器释放文件而失败，随后独立构建退出后重新验收通过。首次并行 CTest 有 8 项失败，其中 7 项在独立临时目录串行复测后通过；另一项酷我测试对应的适配器对象早于构建期间更新的源码，增量重建后通过。最终完整回归使用独立临时目录串行执行，未修改这些其他模块的源码或测试。跳过项为显式启用的在线协议探测、真实 MP3 样本及系统信任链在线测试，酷狗在线闭环由上述独立客户端验证。

最终回归命令（先创建独立临时目录，再让本次进程的 TEMP／TMP 指向它）：

```powershell
$kugouTestTemp = Join-Path (Get-Location) 'out/kugou-test-temp'
New-Item -ItemType Directory -Path $kugouTestTemp -Force | Out-Null
$env:TEMP = $kugouTestTemp
$env:TMP = $kugouTestTemp
ctest --test-dir out/build/kugou-msvc-debug --output-on-failure -j 1
```

源码、使用方式与边界见[酷狗适配器](../../src/providers/kugou/README.md)。该记录不代表其他系统、发行包或 Windows 系统服务已验收。
