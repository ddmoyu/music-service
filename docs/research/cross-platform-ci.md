# 跨系统构建与部署验收

日期：2026-10-24。应用版本：0.1.26。

- 程序基线：`8810e16819a61e736d2e54c7bb4a9fbf12d50835`，[完整矩阵](https://github.com/ddmoyu/music-service/actions/runs/40082916400)的 7 个作业全部通过。该基线包含本轮跨系统修正，以及远端新增的父进程生命周期选项。
- 后续诊断提交 `0366d6a` 仅增加测试失败输出。本地 Debug／Release 重建、连续重启用例与发行包冒烟结果见下文。
- 排障记录：[前一矩阵](https://github.com/ddmoyu/music-service/actions/runs/40080069256)的 Windows 包验证因测试启动路径过长失败；修正脚本后的 [Windows 专项复验](https://github.com/ddmoyu/music-service/actions/runs/40081602375)第二次运行通过，首次的重启用例超时保留在下文。

全部使用原生架构运行器，未使用模拟器。复现命令见[构建与测试](../../cmake/README.md)，部署检查见[发布与部署](../../packaging/README.md)。

## 验证结果

| 环境 | 原生架构 | 验证范围 | 结果 |
| --- | --- | --- | --- |
| Windows Server 2025／MSVC | x86_64 | Debug 全量测试、系统证书 HTTPS、运行冒烟、Release zip、中文深路径、导入库、系统服务 | 通过 |
| Ubuntu 24.04／GCC | x86_64 | Debug 全量测试、系统 CA HTTPS、运行冒烟、Release tar.gz、systemd | 通过 |
| Ubuntu 24.04／GCC | ARM64 | 同上 | 通过 |
| macOS 15／LLVM 20 | x86_64 | Debug 全量测试、Keychain HTTPS、Debug 与 Release 运行冒烟 | 通过 |
| macOS 15／LLVM 20 | ARM64 | 同上 | 通过 |
| Docker／Ubuntu 24.04 | x86_64 | Release 镜像、非 root、健康检查、认证、SIGTERM、同卷重建 | 通过 |
| Docker／Ubuntu 24.04 | ARM64 | 同上 | 通过 |

程序基线的 Windows、Linux、macOS 共 5 个测试作业各发现 610 项 CTest：604 项通过、6 项在线／样本门控跳过、0 失败。系统证书 HTTPS 在单独的必需步骤执行，每个环境 1 项／7 条断言通过。在线音乐检查属于可选步骤，结果单独列在下文，不能由作业绿色推断所有上游均可用。

Linux 两种架构使用 GCC 13.3.0、CMake 3.31.6、glibc 2.39。Release 包的 GLIBC 符号最高为 `GLIBC_2.38`，本轮仅实测 Ubuntu 24.04，未据此承诺最低系统版本。systemd 安装、服务账户权限、认证、正常停止及重启均通过。macOS 两种架构均为 15.7.9、LLVM 20.1.8；CMake 分别为 ARM64 的 4.4.3 和 x86_64 的 4.4.2。

该程序基线的镜像解压后大小：x86_64 为 93,899,517 字节，ARM64 为 115,032,178 字节。这是 `docker image inspect` 的镜像大小，包含基础系统与 CA，不是压缩下载大小或程序文件大小。

## 本轮修正

- macOS SDK：CMake 4 默认允许 SDK 为空，CI 的 FindIconv 因而把 SDK 系统头文件目录插到 Clang 内建头文件之前，两个架构均编译失败。工具链在编译器检测前选择 `macosx`，保留显式 `CMAKE_OSX_SYSROOT`／`SDKROOT`；参见 [CMake SDK 说明](https://cmake.org/cmake/help/latest/variable/CMAKE_OSX_SYSROOT.html)。
- Linux 低内存停机：libstdc++ 的空 `deque` 也可能分配，构造失败后的清理再次分配会终止进程。停机现在阻止工作线程继续取队列，逐项在锁外释放任务，不再创建临时容器；补充无分配清理、析构回调和并发停机回归。
- Windows／macOS 路径夹具：维护测试使用未规范化的临时目录，遇到大小写、短名或 macOS `/var` 符号链接时违反文件接口的前置条件。改为与生产启动流程相同的规范路径，保留原有路径安全检查，并直接报告清理错误。
- 下载异常回归：同一毫秒创建的任务按 ID 排序，测试改为按待执行队列定位首个故障任务；仍检查失败落库、后续完成、执行槽释放和观察器异常隔离。
- Shell 管道：版本探测完整消费 `ldd` 输出，避免 `head` 提前关闭管道导致严格模式下失败。
- 依赖下载：一轮 ARM64 镜像构建下载 zlib 时等待约 17 分钟后网络中断。现有 SQLite／zlib 归档增加单次 120 秒、无进展 30 秒超时，由 CMake 有限重试；依赖、版本、来源、固定哈希均未变化。
- Windows 包验证：原夹具的完整程序路径超过 PowerShell 启动边界，程序尚未运行就失败。保留中文、多层深目录并显式检查完整可执行路径小于 260 字符；不把文件 I/O 的长路径能力等同于进程启动能力，参见 [CreateProcessW](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessw)。自动发现当前 MSVC 的 `dumpbin`，CI 必须完成导入库检查；后台冒烟窗口隐藏。
- CI：Linux、macOS、Docker 各覆盖 x86_64／ARM64，macOS 增加 Release 运行。编译并发为 3、单项 CTest 上限 120 秒、单个作业上限 45 分钟。手动触发可选环境；普通推送与 PR 仍运行全矩阵，同分支同目标取消旧运行。

## 本地验证

Windows MSVC x64 Debug 全量重建与 Release 打包通过。故障注入 2 项／9 条断言、运行时专项 19 项／215 条断言、维护专项 2 项／94 条断言通过；下载异常回归连续执行 20 次通过，重建后再次通过。

Windows 专项 CI 首次复验出现一次 T32 负载重启用例的 30 秒轮询超时；此前通用基线在 5 个原生环境均通过同一用例，同提交第二次运行通过，用时 17.79 秒，新程序基线再次通过，用时 17.18 秒。本地增加任务快照、CDN 请求／字节数与等待传输数诊断后，保持原有超时预算和断言，连续 10 次通过，每次约 17 秒。该次 CI 超时的根因仍未确认，不能据重跑通过认定问题已修复。

把临时目录改为大小写不同的同一路径，修复前可复现维护测试的 10 条失败断言，修复后相同条件通过。另在 296 字符的程序路径复现 PowerShell 启动失败；修正后的包在中文深目录下完成启动、认证、锁检查、停止和重启。接入 `8810e16` 后再次重建并通过包验证，Release 程序为 7,784,448 字节，`dumpbin` 仅列出系统 DLL，没有动态 C/C++ 运行库。

## 在线结果与边界

7 个 CI 环境均通过 5sing 搜索、解析、下载、文件读取、取消竞争及重启回读，包括 Windows x86_64，以及 Linux、macOS、Docker 的两个架构。容器样本为 5,328,631 字节。[前一轮 x86_64 容器](https://github.com/ddmoyu/music-service/actions/runs/40078717274/job/120827571481)曾在搜索时超过 5 秒预算而返回 `UPSTREAM_TIMEOUT`，本轮通过，真实上游仍有波动。

新基线的 Linux ARM64 在酷我搜索阶段发生上游 TLS 握手超时，服务返回 `UPSTREAM_TIMEOUT`；其他 6 个环境在解析阶段返回上游 `407`、无音源地址，服务明确返回 `UPSTREAM_FAILED`。这些结果不计作在线闭环通过。

- 上述基线的 macOS 产物仍依赖 Homebrew LLVM 20。后续已完成双架构独立包、ad-hoc 签名及 launchd 验证，下载入口和独立测试基线见 [macOS 独立包验收](macos-package-acceptance.md)；Developer ID 签名与公证尚未验收。
- Windows 包测试验证的是 PowerShell 可启动的中文深路径，未宣称可从超过 MAX_PATH 的可执行路径启动。
- 未覆盖 Windows ARM64、旧版系统最低要求、Compose、真实 NAS／网络文件系统，也不等同于断电、长期稳定性或吞吐验收。
- Docker 验证的是本地构建的原生镜像，未发布镜像仓库中的多架构清单。
