# 测试目录

测试框架为 Catch2 v3，仅在 BUILD_TESTING=ON 时获取；Debug 预设默认开启。测试通过 `catch_discover_tests` 注册，`ctest --preset <name>` 在没有测试时报错，避免空测试误报成功。

- dependencies：固定依赖的版本与基本可用性（JSON、SQLite、本机回环 HTTP、Mbed TLS 客户端构造），不含业务行为。
- unit：领域规则（品质、身份、状态迁移、下载意图与指纹）与应用端口替身；后续加入请求签名向量和响应解析。
- integration：SQLite 存储（迁移、单 writer、身份与凭证、目录绑定、任务与发布）、本机 HTTP(S) 上游、下载执行、SSE 与媒体入口；`kuwo_gate_test.cpp` 是 S1 关口（`[gate]`）：真实进程与真实酷我协议包，上游由录制样本和合成 MP3 的 CDN 应答，覆盖搜索到文件取回、客户端保存失败、重启、取消与停机续传。`fivesing_gate_test.cpp`（`[gate]`）同时载入酷我与 5sing 协议包，覆盖两源合并搜索、能力差异、5sing 下载与主备地址；其中 `[t41]` 是第二平台关口（两个 principal 的注册、能力差异、缓存与事件隔离、同一下载执行与复用）。在线版本见 `packaging/e2e/kuwo_e2e.py`（`--source fivesing` 为 5sing）。
- contract：公共 API、平台能力和扩展契约。
- fixtures/providers：按平台与上下文版本组织的去凭证样本。
- fixtures/audio：ffmpeg 生成的 3 秒正弦样本（`sine-3s.flac`：8 kHz 单声道 FLAC；`sine-3s.m4a`：8 kHz 单声道 AAC 24 kbit/s、faststart），生成参数见该目录说明，无版权内容。
