# Bilibili 首阶段验收

日期：2026-10-23。范围为 N06 的原生视频搜索、详情与分 P、链接解析、独立 AAC 音轨、可选 Cookie 校验和公共下载链路。能力说明见 [适配器](../../src/providers/bilibili/README.md)，在线客户端见 [bilibili_e2e.py](../../packaging/e2e/bilibili_e2e.py)。

## 已核实的协议与语义

- 官方 `search/type` 接口匿名请求成功，`page_size=3` 的两页返回不同结果。服务最初不带访客 Cookie 时返回来源级 `UPSTREAM_RATE_LIMITED`；通过官方 `finger/spi` 获取 buvid3／buvid4 后，同一服务搜索成功。实现按操作取得临时访客会话，不复制固定 Cookie，也不为未用的 WBI 增加签名代码。
- 公开样本 `BV1Mq4y1U7Lw` 的视频时长 53 秒，`playurl` 声明 52,309 毫秒，提供 30216／30232／30280 的独立 AAC 音轨；CDN Range 请求返回 206 和 MP4 `ftyp` 文件头。
- 多 P 样本 `BV1wPq5BqEau` 有 35 个分 P：P1 对应 cid `34800733309`，P2 对应 cid `34800862212`。搜索总时长不作为第一 P 的时长证据。
- 详情／解析把页码定位别名转换为固定的 `BV…:cidN`。下载必须提交转换后的身份，继续使用公共队列禁止执行期间换曲的规则。
- 音频选择仅限已支持的 AAC 编码和 HTTPS bilivideo CDN。UP 主不写作演唱者，不把视频当专辑；试听、未知时长、无独立音轨、权限、限流和过期地址均有明确结果。

上述是本次从官方 HTTPS 端点取得的样本证据，只能说明当前样本与网络环境。参考仓库的实现没有复制运行时依赖或默认 Cookie。

## 验证状态

MSVC x64 Debug 构建通过，Bilibili 编译无警告；在线验收二进制版本为 `0.1.14`，协议包为 `bilibili-native 0.1.0`。构建目录为 `out/build/bilibili-msvc-debug`，避免与同工作区的其他平台构建竞争输出文件。依赖源码复用已锁定、校验过版本及文件状态的本地 FetchContent 源码，构建产物独立。本次编入酷我、5sing、网易云、QQ 和 Bilibili，不代表其他同时开发的平台已通过本轮回归。

| 验收项 | 结果 |
| --- | --- |
| Bilibili 离线回归 | 16 组、1094 条断言全部通过；包含临时访客会话合并／隔离、分页、分 P 重排、失效 cid、伪造链接、试听、错误码、Cookie 校验、资源过期和取消 |
| Provider／音频／品质回归 | 138 组中 133 通过、5 组按原配置显式跳过在线／外部样本，0 失败，共 5692 条断言；首次旧构建曾遇到酷我黑胶用例失败，重新构建当前工作区后已通过，本轮没有修改酷我代码 |
| 公共目录／下载集成 | 40 组、1893 条断言全部通过；不是全仓库 CTest 或其他系统验收 |
| 搜索与身份 | 匿名中文搜索 3 页，每页 3 项；英文搜索成功。P1／P2 返回不同 cid，上传者未进入演唱者字段 |
| Cookie | 伪造 SESSDATA 校验为 invalid；未提供真实账号，正向登录验收保留未验证 |
| 品质 | 严格 lossless 返回 QUALITY_UNAVAILABLE；allow_lower 返回 standard。样本选择 AAC，声明带宽 319335 bps，公共档位保守标记 standard |
| 实际下载 | `BV1Mq4y1U7Lw:cid399771709` 完成下载；发布文件 2,236,402 字节，SHA-256 `8b6ce7156d1c108fd137c40040d5fa09c802b35a78fe5e8b14bb89ae96b1dd1b`，任务无警告 |
| 独立成品检查 | ffprobe 检出单条 AAC 音频、0 条视频流、52.309 秒，标题标签与详情一致，artist 未伪造；FFmpeg 完整解码通过 |
| 生命周期 | 重复请求复用完成文件；强制新任务取消为 canceled；服务重启后完成任务和文件 SHA-256 回读一致 |

原始本机证据保存在 Git 忽略的 `out/bilibili-unit.log`、`out/bilibili-regression-unit.log`、`out/bilibili-regression-integration.log`、`out/bilibili-build.log`、`out/bilibili-live-report.json`。在线报告不包含 Cookie、UID 或带签名的 CDN 地址。

在线客户端验证匿名中英文搜索、三页推进、无效 Cookie、BV／分 P → cid、标准 AAC、严格无损拒绝、显式降级、真实下载、复用、取消和重启 SHA-256 回读。上表的独立工具检查仅保留为历史证据；当前脚本已删除 ffprobe／FFmpeg 参数及执行入口，后续验收沿用服务自身校验与文件回读，不引入外部工具。

## 未覆盖范围

未提供真实 Bilibili 账号 Cookie，有效账号和不同权限样本待验收。高品质／无损／Hi-Res、歌词、歌手、专辑、推荐、排行榜、收藏夹／合集、全部分 P 浏览、扫码、b23.tv 短链和 av／番剧链接未接入。Linux、macOS、Windows Release 和系统服务专项验收不包含在本次首阶段结论中。
