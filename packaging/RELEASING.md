# 发布说明

music-service 以开发预览版交付原生程序包、前端接入包和 Docker 双架构镜像。各产物使用同一版本与源码提交，实际可下载版本以 [Releases](https://github.com/ddmoyu/music-service/releases) 为准。

## 获取发行版

在版本页选择系统与架构；文件名前缀为 `music-service-<版本>-`，以 0.1.30 为例：

| 文件 | 用途 |
| --- | --- |
| `music-service-0.1.30-windows-x86_64.zip` | Windows x64，支持前台与系统服务 |
| `music-service-0.1.30-linux-x86_64.tar.gz` | Linux x86_64，支持前台与 systemd |
| `music-service-0.1.30-linux-arm64.tar.gz` | Linux ARM64，支持前台与 systemd |
| `music-service-0.1.30-macos-x86_64.tar.gz` | macOS Intel，支持前台与 launchd |
| `music-service-0.1.30-macos-arm64.tar.gz` | macOS Apple Silicon，支持前台与 launchd |
| `music-service-0.1.30-frontend-integration.zip` | 接入指南、核心 OpenAPI、JS 客户端与 TypeScript 类型 |
| `release-manifest.json` / `SHA256SUMS` | 版本、源码提交、文件大小与 SHA-256 |

程序包包含配置、服务脚本、第三方许可与同版接入材料。解压时保留完整目录，使用方法见[发行包指南](package/README.md)。

Docker 使用 `ghcr.io/ddmoyu/music-service:0.1.30`，版本标签包含 `linux/amd64` 和 `linux/arm64`。不使用 `latest` 标签；持久化、鉴权、端口和 Compose 示例见[Docker 教程](docker/README.md)。

## 验证与系统范围

发布工作流会重新构建发布提交，在五个原生环境运行测试、HTTP/CORS/SSE 接入检查、发行包与系统服务检查；两个 Linux 架构另执行容器检查。全部通过后才汇总产物。

镜像推送后按 digest 拉取验证，再合并版本清单，并在两个原生环境匿名拉取运行。Release 先作为草稿上传，下载资产复核校验和后才公开。具体权限、失败重跑和首次 GHCR 可见性设置见[自动发布说明](release-workflow.md)。

- Linux 原生包以 Ubuntu 24.04 为验证基线，不推定旧发行版或任意 NAS 均可运行。
- macOS 最低 15.0，包内携带 LLVM C++ 运行库，使用 ad-hoc 签名；Developer ID、公证和目标机器 Gatekeeper 验收尚未完成。
- 在线音源诊断允许失败；绿色 CI 不代表所有真实音源均可用。Compose 与真实 NAS 未逐一验收。
- 各版本以对应工作流结果为证据；历史结果见[跨系统验收](../docs/research/cross-platform-ci.md)，不能替代后续提交验收。

## 发布操作

1. 将已检查的变更合入 `main`，确认根 `CMakeLists.txt` 的版本与待发布版本一致。
2. 创建并推送精确对应的版本 tag，例如 `v0.1.30`。工作流拒绝版本不符或覆盖已公开 Release。
3. 等待构建、运行、镜像和下载校验全部完成，确认 Release 已转为 Pre-release。
4. 从公开入口下载资产核对 SHA-256，并检查镜像匿名拉取；记录最终提交、运行与下载链接。

仅准备产物时，手动运行 release，保留 `publish=false`。普通 CI 保存五个平台程序包 14 天，不等于已公开发行；不要为了验证随意创建发布 tag。

## 数据与版本切换

- 本项目不维护历史配置、数据库或任务快照兼容性。预览版说明应列出格式变化，不能承诺旧数据自动升级。
- 当前格式支持正常重启和任务恢复；格式不匹配时明确报错，保留原数据，使用新的数据目录／数据卷初始化。
- 切换版本前停止服务，配套保存配置、数据库、`secrets/` 和需要保留的下载文件。单独执行 `backup` 只生成数据库快照，不包含秘密或音频文件。
- 回到旧版本时使用对应版本和对应备份；不要把旧程序直接指向新格式数据。

容器操作与常见故障见 [Docker 使用教程](docker/README.md)。已验证的打包和服务脚本见[发布与部署](README.md)。
