# 发布说明

本项目仍处于开发阶段。建议先交付预览版：Windows／Linux／macOS 可下载程序包，以及服务器部署使用的 Docker 双架构镜像；两种交付形式共用版本和源码提交。使用教程与产物一起维护。

## 当前状态

截至 2026-10-24，[CI 运行 40082916400](https://github.com/ddmoyu/music-service/actions/runs/40082916400)在提交 `8810e16819a61e736d2e54c7bb4a9fbf12d50835` 上的 7 个作业全部通过。该运行没有上传 Actions 产物，仓库尚无 GitHub Release；现有工作流没有镜像推送步骤。

这证明该提交在矩阵环境中完成了对应检查，不表示当前工作区、后续提交或新的发布包已经验收。在线音乐步骤允许失败，真实平台结果见[跨系统验收](../docs/research/cross-platform-ci.md)，不能从绿色作业推断所有音源可用。

macOS 后续在提交 `9ee88117b973d9f7fd5441496e21fb3372bbc20e` 的[运行 40094310300](https://github.com/ddmoyu/music-service/actions/runs/40094310300)中完成 ARM64／Intel 独立包及 launchd 检查，已保存包和 SHA-256，并下载复核。包内业务版本为 0.1.26，不包含工作区并行开发的 0.1.27 账号改动；[详细验收](../docs/research/macos-package-acceptance.md)包含下载入口、实际大小与测试基线。

| 交付内容 | 已有能力 | 发布前仍需完成 |
| --- | --- | --- |
| Windows x64 ZIP | Release 打包、中文深路径运行、系统 DLL 检查、Windows 服务验证 | 保存产物、校验和、预览 Release 与下载后复验 |
| Linux x86_64／ARM64 tar.gz | 两种原生架构打包、运行与 systemd 验证 | 保存产物、校验和、声明实际测试的系统基线、下载后复验 |
| Docker linux/amd64＋linux/arm64 | 两种原生镜像构建、非 root、健康检查、SIGTERM、同卷重建 | 镜像中的许可材料、推送仓库、合并多架构清单、实际拉取复验 |
| macOS x86_64／ARM64 | 独立运行库、tar.gz、launchd、脱离 Homebrew 运行、校验和上传及下载复核已通过 | Actions 产物保留 14 天，长期分发仍需预览 Release；Developer ID 证书、公证和 Gatekeeper 验收另需完成 |

macOS 包携带现有 LLVM 20 运行库，必须整体保留 bin/lib/share 布局；最低系统版本、包验证与签名状态见 [macOS 说明](macos/README.md)。Linux 当前实测 Ubuntu 24.04；不按编译成功或单个 GLIBC 符号版本推定所有旧发行版、NAS 都能运行。

## 建议的首批产物

版本取根目录 `CMakeLists.txt` 的 `project(... VERSION ...)`，与程序 `version` 输出、Git tag、发行说明和镜像版本保持对应。以 `0.1.26` 为命名示例，以下不是已发布下载链接：

```text
music-service-0.1.26-windows-x86_64.zip
music-service-0.1.26-linux-x86_64.tar.gz
music-service-0.1.26-linux-arm64.tar.gz
music-service-0.1.26-macos-arm64.tar.gz
music-service-0.1.26-macos-x86_64.tar.gz
SHA256SUMS
```

包内保留程序、配置示例、使用说明、平台服务脚本和第三方许可文件，沿用 [CPack 布局](../cmake/Packaging.cmake)。不要把 Debug 程序、测试程序、开发工具或构建缓存作为用户发行包。

Docker 建议使用 GHCR，拟定名称为 `ghcr.io/ddmoyu/music-service:<版本>`，清单同时包含 `linux/amd64` 和 `linux/arm64`。该地址是发布规划，目前教程使用自行构建的 `music-service:local`。GitHub Actions 可通过 `GITHUB_TOKEN` 和 `packages: write` 向 GHCR 发布，具体权限与登录方式见 [GitHub 官方说明](https://docs.github.com/en/actions/tutorials/publish-packages/publish-docker-images#publishing-images-to-github-packages)。

首批 GitHub Release 标记为 **Pre-release**。镜像使用明确版本标签，同时记录 digest；开发预览期间不以 `latest` 隐含稳定性或兼容性保证。

## 自动发布流程的准备顺序

以下是后续实现要求，当前只有 `ci.yml`，尚无自动发布工作流。

1. **固定发布提交。** 整理本次要交付的变更，保持版本、源码与 tag 一致；对该提交完成所需 CI 检查。已有绿色提交不能替代对新运行时变更的验证。
2. **保留经检查的产物。** Windows／Linux 在包冒烟通过后保存压缩包；Docker 保存本次构建的镜像身份。已有运行没有上传产物，需要重新构建，不能从旧的成功状态恢复下载包。
3. **准备分发材料。** 编写本版本变更与已知限制、系统要求、教程入口、文件大小和 SHA-256。明确仓库本身的许可证声明；原生包已有第三方许可安装规则，Dockerfile 当前只复制程序与配置，镜像分发前也需补齐相应材料。
4. **交付预览包。** 由显式版本 tag 或手动发布触发，完成检查后把对应包与校验和附到 GitHub 预览 Release。普通 PR 验证不执行公开发布。
5. **交付容器镜像。** 沿用两个原生架构构建与冒烟，推送各自的镜像 digest，合并同版本的多架构清单。确认镜像访问权限满足教程中的拉取方式。
6. **从分发地址复验。** 下载 Release 包核对 SHA-256 并解压启动；在 amd64／arm64 环境从镜像仓库拉取版本或 digest，检查架构、健康状态、认证、三个入口、停止及同卷重建，再把实际链接补入 README。

首次发布不新增业务依赖，继续使用已有构建、打包和冒烟脚本。涉及新增依赖的方案仍按 [AGENTS.md](../AGENTS.md) 评审。

## 数据与版本切换

- 本项目不维护历史配置、数据库或任务快照兼容性。预览版说明应列出格式变化，不能承诺旧数据自动升级。
- 当前格式支持正常重启和任务恢复；格式不匹配时明确报错，保留原数据，使用新的数据目录／数据卷初始化。
- 切换版本前停止服务，配套保存配置、数据库、`secrets/` 和需要保留的下载文件。单独执行 `backup` 只生成数据库快照，不包含秘密或音频文件。
- 回到旧版本时使用对应版本和对应备份；不要把旧程序直接指向新格式数据。

容器操作与常见故障见 [Docker 使用教程](docker/README.md)。已验证的打包和服务脚本见[发布与部署](README.md)。
