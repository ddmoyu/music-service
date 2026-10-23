# 咪咕 Z3D／MG3D：公开实现与原生 C++ 接入

核对日期：2026-10-23。当前实现：`migu-native 0.1.12`，处理器 `audio.migu-z3d` v1。

## 公开来源与许可证

| 项目 | 已核对源码 | 许可证及用途 |
| --- | --- | --- |
| guohuiyuan/music-lib | [download.go](https://github.com/guohuiyuan/music-lib/blob/6353508e84cd357b5bc3acf32d95a16e01bbe20c/migu/download.go)、[download_test.go](https://github.com/guohuiyuan/music-lib/blob/6353508e84cd357b5bc3acf32d95a16e01bbe20c/migu/download_test.go) | [AGPL-3.0](https://github.com/guohuiyuan/music-lib/blob/6353508e84cd357b5bc3acf32d95a16e01bbe20c/LICENSE)。用于核对协议参数、字段和公开输入输出向量；未引入其库、下载调度器或源码文件 |
| Unlock Music 镜像 | [mg3d.ts](https://github.com/DHJComical/unlock-music-web-mirror/blob/f27bc0478555d8de4b068205f6d15d17edd4e0ca/src/decrypt/mg3d.ts) | [MIT](https://github.com/DHJComical/unlock-music-web-mirror/blob/f27bc0478555d8de4b068205f6d15d17edd4e0ca/LICENSE)。旧 MG3D 候选序列恢复方法的参考；版权与 MIT 原文已加入发行包 |

新实现使用 C++20 和项目已有的 Mbed TLS、TagLib，不启动 Go、JavaScript 或外部解密工具。AGPL 与 MIT 来源分别记录，不将 AGPL 项目误称为 MIT，也不因改写语言而重新声明上游许可证。

## 协议与实现

主要实现：[Z3D 解密及处理器](../../src/audio_processing/migu_z3d.cpp)、[PCM WAV 校验](../../src/audio_processing/format/wav.cpp)、[咪咕取流适配](../../src/providers/migu/audio.cpp)。

1. 对固定的 32 字节协议常量作 AES-128-CBC 解码，保留完整 32 字节结果。
2. 对上述结果与当前文件密钥的 ASCII 内容拼接后求 MD5，转为 32 字节大写十六进制 ASCII 序列。
3. 可选的三字节 `Z3D` 前缀不参与音频偏移。每字节减去对应循环序列字节，按 256 取模；跨 64 KiB 窗口保持偏移。
4. 旧 MG3D 没有外部密钥时，在前 64 KiB 的 32 字节对齐块中寻找大写十六进制候选。每个候选都从原始头部重新试解，经 RIFF 和 PCM fmt 验证后才解整个文件；没有候选则明确失败。
5. 解密输出必须完整通过 PCM WAV 校验，再进入通常的时长、试听、标签和最终 SHA-256 校验。标签写入前后比较 PCM 数据哈希。

公开测试向量：文件密钥 `9A14CB2B6AA5DA30CE036BBD6664373E`，循环序列 `397BE2C84E0F983C65ED04FC407C8F99`。测试夹具只包含合成 PCM，由固定公开序列独立构造，不调用待测密钥派生函数生成预期结果。

WAV 结构规则依据微软的 [RIFF 定义](https://learn.microsoft.com/en-us/windows/win32/xaudio2/resource-interchange-file-format--riff-)和 [WAVEFORMATEX](https://learn.microsoft.com/en-us/windows/win32/api/mmreg/ns-mmreg-waveformatex)。支持 PCM fmt 16／18 字节以及 extensible PCM；检查 RIFF 总长度、奇数 chunk 填充、采样率、声道、有效位深、块对齐、字节率、唯一 data 块和整数采样帧。输入上限 2 GiB，工作窗口 64 KiB。

## 接入与边界

- `ResolvedAudio.decryption_key` → `ProcessingRequest.decryption_key` → `MiguZ3dProcessor`，使用 `SecretBytes` 管理生命周期；不进入任务、资源、缓存持久化或 API JSON。任务恢复重新解析音源取得密钥。
- 只有权限接口实际返回 `formatType=Z3D`、有效的 Android WAV 密钥和可接受的 HTTPS 音源地址，才附加 Z3D 处理计划。三种曲目身份仍须一致，权限拒绝仍优先处理。
- 普通音源附带 `z3dCode` 仅是可用格式元数据，不替换正在请求的 PQ／HQ／SQ 音源。当前没有独立 3D 选择参数，也不将 Z3D 当作 Hi-Res 或统一音质枚举。
- Z3D 输出的采样率、位深、码率来自实际 WAV；上游没有普通音质档位证据时 `actual_quality` 仍未知。PCM 容器本身不能证明其制作源无损。
- `auditionsType`／`auditionsLength` 等权限证据继续生效；解密不把试听变成整曲，不增加会员或购买权限。
- 加密资产不能直接作为普通音频代理播放；完成下载和处理后，可以通过已有本地文件播放接口使用 WAV。
- I3D／M4A、RF64、浮点或压缩 WAV 不在本轮范围。旧 MG3D 候选恢复依赖零填充，不能保证适用于所有文件。
- 保留输入；失败、超时和取消删除本次明文工作副本；拒绝覆盖已存在的解密目标和工作文件链接。实际 PCM 样本没有额外认证标签，结构及哈希校验不能发现所有原始密文位翻转。

## 验证记录

MSVC x64 Debug 已完成编译与链接，实测服务版本 `0.1.22`；独立配置的平台清单为 `kuwo;fivesing;netease;qq;migu`。TagLib 必须启用 `WITH_RIFF`，项目配置和头文件适配均已包含 WAV 支持。

- `[z3d],[migu]`：35 个单元测试，1114 个断言通过。覆盖公开密钥向量、可选前缀、跨窗口、旧 MG3D 候选恢复、fmt 16／18／40、RIFF 损坏、错误密钥、标签及封面读回、试听保留、流式取消和工作路径别名。
- `[z3d]` 集成测试：使用真实咪咕适配器、HTTP 控制接口、下载线程池、数据库及发布链路，只有上游响应和音频为合成样本。有效样本发布为 `audio/wav`；损坏样本拒绝发布；实际音质保持未知；API、日志和数据库扫描均未发现逐曲密钥。
- 最后补充的失败用例先复现了标签阶段取消残留 `tagged.wav`、标签路径硬链接到输入时改写输入的问题。修复后同时清理两个明文副本，并拒绝输入别名；上述回归通过。
- 普通咪咕在线 E2E 通过：匿名目录、标准整曲 MP3（3,644,579 字节）、WebP 封面／LRC、试听 Range、下载复用和重启回读正常。此项验证普通未加密音源，不代替真实 Z3D 验证。

最终全量 CTest：472 项中 466 通过、6 项按条件跳过、0 失败，耗时 204.58 秒。跳过的是已有的外网／真实 MP3 条件用例。真实 Z3D 本地样本测试是另行显式启用的隐藏用例，本轮未运行。

2026-10-23 匿名在线请求 `toneFlag=Z3D`：公开参考样例 `600919000009811300` 返回 `cannotCode=440003`，`600902000006889366` 返回 `440006`；均没有音源或密钥。本轮未因此取得真实 Z3D 文件，不把合成样本验证表述为真实会员歌曲下载成功。

本地真实样本可显式验证：设置 `MUSIC_SERVICE_Z3D_SAMPLE` 为文件路径、`MUSIC_SERVICE_Z3D_KEY_FILE` 为只含文件密钥的本地文件路径，运行单元测试过滤器 `[z3d-real]`。该测试不打印密钥，只报告成功后的字节数、时长、采样率、位深和文件哈希。
