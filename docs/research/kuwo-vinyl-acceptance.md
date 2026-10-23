# 酷我黑胶原生接入验收

日期：2026-10-23；Windows x64／MSVC Debug。酷我实现版本 **kuwo-native 0.1.9**，处理计划 **audio.kuwo-flac v1**。黑胶功能首次构建与真实下载验收时应用版本为 0.1.9；工作区另有其他平台并行开发，应用总版本随后前进，未回退其他改动。

## 实现范围

| 项目 | 完成 | 行为 |
| --- | --- | --- |
| 黑胶取流 | ☑ | 现有 `hires` 对应 `br=23000kflac`；上游须为同一歌曲，不能以替代曲或不支持的专有格式冒充 |
| 品质与降级 | ☑ | 请求严格档位时拒绝低档；显式 `allow_lower` 才尝试普通无损／高品质／标准，返回实际品质及 `QUALITY_LOWERED` |
| 原生 FLAC 完整校验 | ☑ | 本项目 C++ 解码子帧与残差，检查头 CRC8、帧 CRC16、序号、样本数、声道、采样率、位深及 PCM MD5 |
| 尾部附加数据 | ☑ | 由验证后的末帧确定边界，识别有界尾部后写出清理副本；保留原下载，拒绝未知尾部或不完整音频 |
| 最终产物校验 | ☑ | 标签写入后再次完整校验、比较 PCM；按最终字节计算 SHA-256，实际规格不足时不得发布为 hires |
| 公共下载闭环 | ☑ | 完成任务、标签与嵌入／独立封面、结果复用、Range、重启后文件回读 |
| 外部工具依赖 | — | 生产取流、下载、清理和解码均为原生代码；只复用已有 Mbed TLS 哈希与 TagLib 标签库，无外部程序 |

品质规则沿用已有跨平台约定：FLAC 位深至少 24 bit、采样率至少 48 kHz 才维持 `hires`，不把平台参数 23000 当作实际码率。本轮指定样本是 24 bit／96 kHz。

实现没有注册 Cookie profile，也没有实现 MFLAC／MGG 解密。此前匿名与豪华 VIP 完整源文件对照相同，不把本次黑胶接入描述为会员 Cookie 的增益。

## 原生校验与拒绝边界

FLAC 实现依据 [RFC 9639](https://www.rfc-editor.org/rfc/rfc9639.html)。支持 constant、verbatim、fixed 和 LPC 子帧、Rice／Rice2／escape 残差、wasted bits 与立体声关联还原；读取窗口 64 KiB，PCM 仅保留一个块，循环内检查取消和截止时间。

总样本数与非零 STREAMINFO MD5 是确认完整音频的必要证据。任一帧的 CRC、序号、属性或最后 PCM MD5 不符都返回 `AUDIO_INVALID`，不搜索后续同步码跳过损坏音频。只在这些检查全部通过之后，才允许移除末帧后 8～4096 字节、以 `f000ff0f` 开始且以 `0e55fff0` 结束的已识别尾部。其他数据拒绝。样本的尾部恰为 30 字节，但代码没有固定删除 30 字节。

原下载文件保持不变，清理副本走现有标签及发布流程；成功清理在 `result.warnings` 报告 `KUWO_TRAILER_REMOVED`。复制或后续校验失败会移除本次生成的副本，不发布失败产物。

## 自动化回归

新夹具为合成波形、噪声及常量，没有真实音乐和账号内容。由独立编码器生成，编码前 PCM 的 MD5 和规格保存在 `tests/fixtures/audio/flac-verification.json`，测试不依赖生成器或外部音频程序。

| 验证 | 结果 |
| --- | --- |
| 针对性单元回归 | **21 项通过**，包含酷我既有解析与本轮黑胶；最终全量回归包含后续补充的降级分支断言 |
| 黑胶下载集成回归 | **1 项通过，91 条断言**；覆盖真实处理器和发布流程的 4 个场景 |
| 16／24／32 bit；44.1／48／96 kHz；单／双声道 | 解出的 PCM MD5 与编码前数据匹配 |
| 无尾部、30 字节尾部和不同长度的已识别尾部 | 按末帧边界处理；原文件不变，重复处理结果一致 |
| 坏 CRC、坏 MD5、错误样本数、截断、缺少 MD5 | 拒绝，未生成可发布结果 |
| 未知／过大／不完整尾部、拼接另一份 FLAC | 拒绝，不猜测裁切长度 |
| 传输大小不符、取消、截止时间、输入与工作路径冲突 | 拒绝或取消，保留输入 |
| 上游声称 hires，实际是 16 bit／44.1 kHz | strict 返回 `QUALITY_UNAVAILABLE`；allow_lower 完成为 lossless 并警告 |
| 上游无黑胶／返回 MFLAC，网络或协议错误 | 显式降级有界；严格请求不降级，网络／限流／损坏响应不掩盖 |

Windows MSVC x64 Debug 独立构建通过，构建目录为 `out/build/kuwo-vinyl-msvc`；本轮回归配置启用 `kuwo;fivesing;netease;qq`，不把工作区并行开发的其他平台算作本轮已验收范围。

全量 CTest **421 项**，串行执行耗时 **320.53 秒**：**413 项首次通过、6 项按配置跳过、2 项首次失败**。两项为既有 SSE 控制请求的 2 秒耗时门限（实测 2.060 秒）和 T41 多任务等待完成；改用系统盘上的独立临时目录后，不改代码或断言，两项均**连续复测 3 次通过**，复测耗时 18.05 秒。综合结果为 **415 项通过、6 项跳过，无未关闭失败项**，保留首次失败记录，不表述为首轮全绿。

此前共用构建输出／临时目录的并行运行还出现过文件占用；最终使用独立构建目录和临时目录复核。记录见 `out/kuwo-vinyl-final-validation.log`（最终构建及并行尝试）、`out/kuwo-vinyl-final-ctest.log`（完整串行回归）、`out/kuwo-vinyl-final-retest.log`（两项各三次复测）。六项跳过均为需显式启用的在线探针／本地音频样本测试；本轮黑胶在线闭环另行执行，见下节。

## 真实歌曲闭环

使用 `packaging/e2e/kuwo_vinyl_e2e.py` 启动独立临时服务，没有导入 Cookie。服务 PATH 排除 FFmpeg、ffprobe、flac、Python、Node，客户端只调用原生服务 API。搜索并核对梅艳芳《亲密爱人（黑胶版）》：歌曲 **492445332**、专辑 **46894897**、曲号 **3**。

| 项目 | 实测 |
| --- | --- |
| 请求 | `hires`、`strict`、`metadata_policy=required`；封面内嵌与独立导出 |
| 任务结果 | completed；actual_quality=hires；processor=audio.kuwo-flac v1 |
| 音频 | **FLAC，24 bit／96,000 Hz／双声道，28,476,521 样本，约 296.630 秒** |
| 原下载大小 | 112,571,394 字节 |
| 尾部处理 | 识别并移除 30 字节；报告 `KUWO_TRAILER_REMOVED` |
| 成品大小 | 112,647,165 字节，包含服务写入的标题、歌手、专辑、曲号和封面 |
| 成品 SHA-256 | `b754c1c58de9bcbec51c3c8681084c61bbce6d0dd5fd131400f348cff0117505` |
| PCM MD5 | `874082e3f1a569fe2f568d5a659c1423`，原生解码和独立解码一致 |
| 音频帧 SHA-256 | `b7bd80271b0835efa2def73bec061b02364292f0fe87966daaa0ead4a362b805`，与此前已验证参考副本完全一致 |
| 封面 | 79,788 字节，嵌入图片和独立图片字节一致 |
| 复用／Range／重启回读 | 均通过；重启后完整文件 SHA-256 不变 |

随后另用独立 FFmpeg 对最终成品完整解码：无错误诊断，PCM MD5 匹配。它仅用于验收交叉验证，不参与服务执行或文件修复。文件规格与完整性已核验，不据此认证原始母带的制作规格。

## 本地复现与证据

```powershell
python packaging/e2e/kuwo_vinyl_e2e.py --binary out/build/windows-msvc-debug/src/server/music-service.exe --output-dir out/kuwo-vinyl-acceptance
```

首轮在线结果：`out/kuwo-vinyl-acceptance/report.json`、`independent-verification.json`。成品为同目录的 `梅艳芳 - 亲密爱人（黑胶版）-原生黑胶.flac` 与 `梅艳芳 - 亲密爱人（黑胶版）-封面.jpg`。音频、报告原始数据和账号 Cookie 均位于 Git 忽略目录；仓库只记录脱敏验收文字、实现和合成夹具。

离线回归使用 `ctest --test-dir out/build/kuwo-vinyl-msvc --output-on-failure --parallel 1`；执行前将当前进程的 `TEMP`、`TMP` 指向本轮独占的已存在目录，避免和其他测试进程的固定夹具目录冲突。两项复测命令追加 `--rerun-failed --repeat until-fail:3`。

相关：[功能勾选表](../feature-status.md)、[酷我适配器与请求示例](../../src/providers/kuwo/README.md)、[此前会员与黑胶协议对照](kuwo-membership-quality-retest.md)。
