# 发布与部署

使用服务请先看 [Docker 使用教程](docker/README.md)或[发行包使用说明](package/README.md)。发布范围、产物与待完成事项见[发布说明](RELEASING.md)；本页记录构建和部署验证脚本。

Windows、Linux、macOS 和 Docker 共用同一程序 `music-service` 与同一 HTTP 契约。CI（`.github/workflows/ci.yml`）在 main 推送、PR 及手动触发时执行构建、测试与运行验证。

普通 CI 不创建 GitHub Release，也不推送容器镜像；五个平台程序包在运行验证通过后保存到 Actions 产物，附 SHA-256，保留 14 天。发布工作流复用完整 CI，在显式版本标签触发时发布预览 Release 和双架构镜像，详见[自动发布说明](release-workflow.md)。工作流配置不代表某版本已经发布。

2026-10-24 的 macOS ARM64／Intel 独立包与 launchd 已通过原生 CI，下载后哈希也已复核；具体提交、产物和签名状态见 [macOS 独立包验收](../docs/research/macos-package-acceptance.md)。

CI 使用标准托管运行器：Windows x64，以及 Linux、macOS、Docker 各自的 x86_64／ARM64 原生环境，不使用模拟器。编译并发默认限制为 3，单个 CTest 用例超时为 120 秒，单项 CI 任务上限为 45 分钟；矩阵中一个环境失败不会取消其他环境。手动触发可选 all／windows／linux／macos／macos-package／docker；`macos-package` 仅复验两个 macOS 架构的 Release 打包和包／launchd 运行，适用于没有 C++ 运行时变更的打包修正，不替代 Debug 全量测试。普通推送和 PR 仍运行全矩阵，同一分支同一目标的新运行会取消旧运行。具体通过情况以对应提交的 [CI 结果](https://github.com/ddmoyu/music-service/actions/workflows/ci.yml) 为准，配置了任务不代表已经验收通过。

| 目录 | 内容 | 验证内容 |
| --- | --- | --- |
| `windows/` | `smoke.ps1`；`install-service.ps1`、`uninstall-service.ps1`（随发行包）；`package_smoke.ps1`、`service_smoke.ps1` | smoke：version、check-config、init、serve 就绪、healthcheck、带 token 的 info、无 token 401、运行中管理命令被拒；强制结束后实例锁释放，重启后 token 仍有效。package_smoke：发行 zip 解压到中文深路径（完整程序路径小于 260 字符）、布局与许可文件齐全、无附带 DLL、只导入系统 DLL（自动发现当前 MSVC 的 dumpbin，CI 必须检查）、SCM 之外运行 service 给出提示并以 2 退出、对解压出的程序跑 smoke。service_smoke（需管理员）：以虚拟账户安装为服务、token 可用、secrets 属服务账户、Stop-Service 退出码 0 且日志有停机记录、停止后实例锁已释放、重启后 token 仍有效，最后卸载 |
| `linux/` | `smoke.sh`；`music-service.service`、`install.sh`（随发行包）；`package_smoke.sh` | smoke：同上，另以 SIGTERM 停止：退出码 0，停机顺序为监听 → 数据库 → 实例锁，之后管理命令可执行；输出 glibc 与动态库依赖。package_smoke：发行 tar.gz 解压到中文深路径、布局齐全、不动态链接 libstdc++／libgcc、输出所需 glibc 版本、跑 smoke；`--service`（root＋systemd）以 install.sh 安装、检查运行用户与 secrets 属主、token 请求、SIGTERM 停止退出状态 0 且 journal 有停机记录、重启，最后清理 |
| `package/` | 发行包的 README、配置示例与第三方许可说明 | 由 `cmake --build --preset <release 预设> --target package` 打入 `share/music-service/`（布局见 `cmake/Packaging.cmake`） |
| `macos/` | `bundle_runtime.py`、`package_smoke.sh`、`launch-agent.sh`、配置与使用说明 | CPack 收集已有 LLVM C++ 运行库并改写为包内相对路径，附许可材料，剥离后签名；解压到中文与空格目录检查结构、架构、依赖、签名，复用 `linux/smoke.sh` 验证前台运行，再验证 launchd 安装、停止、重启及卸载。CI 在 ARM64／x86_64 上临时移开 Homebrew LLVM 后执行包检查；默认 ad-hoc 签名，不包含 Developer ID 公证验收 |
| `docker/` | `Dockerfile`、`config.json`、`smoke.sh`、`e2e.sh` | 多阶段构建；运行镜像只含程序、Ubuntu 24.04 基础库与 CA 证书，非 root（uid 10001）；数据卷上 init、健康检查、发布端口访问、`docker stop` 正常收尾，删除容器后用同一卷重建仍可用；`e2e.sh IMAGE [--source fivesing]` 发布三个端口后运行在线闭环（默认酷我），以 `docker restart` 作为重启步骤 |
| `e2e/` | `kuwo_e2e.py` | 在线酷我闭环（仅标准库）：搜索 → 详情 → 解析 → 持久任务（创建连接关闭）→ SSE／HTTP → 文件元数据 → 票据 Range 与完整读取并与发布文件比对 → 客户端保存中途失败而任务仍 completed → 取消与传输竞争（canceled 或 completed 其一）→ 重启后新实例仍提供任务与文件、幂等重放与完成结果复用。`--binary` 自行 init／serve／重启，`--api`＋`--token`＋`--restart-command` 驱动已运行的服务；`--source fivesing` 对 5sing 运行同一闭环。需要能访问对应平台的网络；CI 中这些步骤允许失败，海外运行器可能受地区限制 |

## 账号功能验收

账号功能的独立进程验收另见 [测试说明](../tests/README.md#账号启停与会员校验)：`e2e/credential_activation_e2e.py` 使用合成 Cookie 验证切换、启停、磁盘清理与重启；`e2e/credential_membership_e2e.py` 使用本机真实 Cookie 查询会员与到期信息。两者使用独立临时数据目录，不依赖已有部署；合成账号验收不等于真实平台权益验证，也不替代发行包和系统服务验证。

## 证书

上游 HTTPS 一律校验证书链与主机名（Mbed TLS）。信任根：Windows 读取系统证书存储；Linux 读取发行版 CA 文件（如 `/etc/ssl/certs/ca-certificates.crt`）；macOS 经 cpp-httplib 读取系统 Keychain（链接 Security 与 CoreFoundation）；容器镜像安装 `ca-certificates`。Windows、Linux 和 macOS CI 都配置了 `[online]` 用例，用系统信任根访问公开 HTTPS 站点；本地开发默认不联网，设置 `MUSIC_SERVICE_ONLINE_TESTS=1` 才运行。

## 容器

完整命令见 [Docker 使用教程](docker/README.md)，包含控制、媒体、事件三个端口和 token 获取步骤。

- 基础镜像按多架构索引 digest 固定（`ubuntu:24.04`，见 Dockerfile）；构建阶段用 pip 安装固定版本的 CMake 3.31.6 与 Ninja，因为发行版 CMake 低于工程要求的 3.30。默认最多 3 个编译任务，可通过 `--build-arg BUILD_JOBS=2` 按构建机内存调整。
- 镜像使用 UID 10001，状态与下载分别位于 `/data`、`/downloads`。`init`、运行和管理命令必须使用相同的卷与配置；身份管理须先停止服务。
- ENTRYPOINT 为 exec 形式，服务是 PID 1，接收 `docker stop` 的 SIGTERM。`shutdown.soft_seconds` 默认 15 秒，不能用空闲冒烟的停止速度保证负载下在 Docker 默认 10 秒内退出；教程预留 60 秒停止宽限期，修改应用停机预算时也要调整容器设置。
- x86_64 与 ARM64 原生镜像已通过构建、非 root 运行、健康检查、SIGTERM 与同卷重建，在线结果另见[跨系统验收](../docs/research/cross-platform-ci.md)。Compose 示例与真实 NAS 尚未运行验收，也未发布多架构镜像清单。
