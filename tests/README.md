# 测试目录

测试框架为 Catch2 v3，仅在 BUILD_TESTING=ON 时获取；Debug 预设默认开启。测试通过 `catch_discover_tests` 注册，`ctest --preset <name>` 在没有测试时报错，避免空测试误报成功。

- dependencies：固定依赖的版本与基本可用性（JSON、SQLite、本机回环 HTTP、Mbed TLS 客户端构造），不含业务行为。
- unit：领域规则（品质、身份、状态迁移、下载意图与指纹）与应用端口替身；后续加入请求签名向量和响应解析。
- integration：SQLite 存储（当前结构初始化与校验、单 writer、身份与凭证、目录绑定、任务与发布）、本机 HTTP(S) 上游、下载执行、SSE 与媒体入口；`kuwo_gate_test.cpp` 是 S1 关口（`[gate]`）：进程内运行服务入口与真实酷我协议包，上游由录制样本和合成 MP3 的 CDN 应答，覆盖搜索到文件取回、客户端保存失败、重启、取消与停机续传。`fivesing_gate_test.cpp`（`[gate]`）同时载入酷我与 5sing 协议包，覆盖两源合并搜索、能力差异、5sing 下载与主备地址；其中 `[t41]` 是第二平台关口（两个 principal 的注册、能力差异、缓存与事件隔离、同一下载执行与复用）。独立进程在线版本见 `packaging/e2e/kuwo_e2e.py`（`--source fivesing` 为 5sing）。
- fixtures/providers：按平台与上下文版本组织的去凭证样本。
- fixtures/audio：固定的 3 秒合成正弦样本（`sine-3s.flac`：8 kHz 单声道 FLAC；`sine-3s.m4a`：8 kHz 单声道 AAC 24 kbit/s、faststart），历史来源及摘要见该目录说明；测试不调用外部媒体工具。

## 账号启停与会员校验

| 入口 | 覆盖范围 |
| --- | --- |
| `integration/accounts_test.cpp` 的 `[activation]` | 同平台互斥启用、全部停用、跨用户隔离、Cookie 文件删除、清理失败重试／重启回收、事务回滚、并发切换与旧校验取消 |
| `integration/server_test.cpp` 的 `[activation]`、`[membership]` | HTTP 请求／错误／响应字段、查询与重启、到期日期和精确时刻、后台校验过载与控制接口响应 |
| `integration/catalog_test.cpp`、`kuwo_gate_test.cpp`、`kugou_cookie_audio_test.cpp` | 选择变化使缓存与游标失效；原账号停用后，已绑定的排队任务不会改用其他账号；重新提供 Cookie 后可创建新任务 |
| [credential_activation_e2e.py](../packaging/e2e/credential_activation_e2e.py) | 启动实际可执行程序，用两个合成 QQ Cookie 验证切换、启停、重启、磁盘 Cookie 文件及 SQLite 清理状态；无需真实平台账号 |
| [credential_membership_e2e.py](../packaging/e2e/credential_membership_e2e.py) | 使用本机真实 Cookie 在线验证登录、会员类型、到期精度与重启回读；与账号启停的合成测试分别验收 |

从仓库根目录执行，先完成 [MSVC Debug 构建](../cmake/README.md)：

```powershell
& .\out\build\windows-msvc-debug\tests\music_service_integration_tests.exe "[activation],[membership]"
python packaging/e2e/credential_activation_e2e.py --binary out/build/windows-msvc-debug/src/server/music-service.exe --report out/account-activation-e2e-report.json
```

独立进程脚本只使用自身创建的临时数据目录，完成后清理，不连接现有部署；Windows 重启步骤为强制结束测试进程后重新启动，用于验证磁盘恢复。其他系统使用对应预设的程序路径。该测试不证明真实 QQ 会员或歌曲权限。

在线会员验收将真实 Cookie 放在本机目录的 `netease-cookie.txt`、`qq-cookie.txt`、`kuwo-cookie.txt`，通过 `--secret-dir` 指定；未提供的平台记为未测，不当作通过。示例（需先准备目录和文件，文件内容不进入命令行）：

```powershell
python packaging/e2e/credential_membership_e2e.py --binary out/build/windows-msvc-debug/src/server/music-service.exe --secret-dir .local-secrets/membership --report out/credential-membership-report.json
```

脚本先验证会员快照重启回读，再创建同平台的无效 Cookie 账号，避免后者自动启用后清空前者证据影响断言。真实 Cookie 不进入仓库、报告或日志；报告保留脱敏查询结果。已执行的构建、完整回归数量、平台和进程验证边界见 [验收记录](../docs/research/credential-membership-acceptance.md#账号启用停用与-cookie-清理0129)，本页命令本身不代表已运行。
