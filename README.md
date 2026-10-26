# music-service

[![CI](https://github.com/ddmoyu/music-service/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/ddmoyu/music-service/actions/workflows/ci.yml)
[![预览版本](https://img.shields.io/github/v/release/ddmoyu/music-service?include_prereleases&label=preview)](https://github.com/ddmoyu/music-service/releases)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)](cmake/README.md)
[![系统支持](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux%20%7C%20macOS-555)](packaging/package/README.md)

轻量、跨平台的音乐后端服务，通过 HTTP API 提供搜索、音源解析、下载与任务管理，可供网页、桌面和移动端独立接入。

**[下载预览版](https://github.com/ddmoyu/music-service/releases) · [前端快速接入](docs/frontend-integration.md) · [Docker 部署](packaging/docker/README.md) · [API 文档](docs/design/cpp20-refactor/backend-service/api-contract.md)**

## 能做什么

- **搜索与播放**：搜索、详情、音源解析、媒体流，以及返回实际播放来源的显式换源。
- **后台下载**：单曲与批量任务、进度订阅、取消、持久化与重启恢复。
- **自动换源**：下载失败时严格匹配歌曲与完整演唱者名单，优先选择历史下载成功率高的来源。
- **音频整理**：音频校验、标签写入、歌词与封面导出，以及成品文件访问。
- **账号管理**：Cookie 校验、会员信息查询、多账号保存与启停管理。
- **前端接入**：统一 HTTP/JSON 接口、SSE 进度事件、核心 OpenAPI 和无依赖 JS/TypeScript 示例。

## 获取与运行

在 [Releases](https://github.com/ddmoyu/music-service/releases) 下载匹配系统与架构的完整程序包，用同页的 `SHA256SUMS` 校验后解压。运行服务不需要 C++ 工具链。

| 部署环境 | 发行包后缀 / 镜像 | 运行说明 |
| --- | --- | --- |
| Windows x64 | `windows-x86_64.zip` | [前台运行或 Windows 服务](packaging/package/README.md) |
| Linux x86_64 / ARM64 | `linux-x86_64.tar.gz` / `linux-arm64.tar.gz` | [前台运行或 systemd](packaging/package/README.md) |
| macOS 15.0+ | `macos-x86_64.tar.gz` / `macos-arm64.tar.gz` | [Intel / Apple Silicon 与 launchd](packaging/macos/README.md) |
| Docker | `ghcr.io/ddmoyu/music-service:0.1.30` | [双架构镜像与持久化部署](packaging/docker/README.md) |

程序包内附配置、服务脚本、第三方许可与同版前端接入材料。Docker 标签包含 `linux/amd64` 和 `linux/arm64`；初始化、持久化目录和三个服务端口按教程配置。

当前为开发预览版。macOS 使用 ad-hoc 签名，尚未进行 Developer ID 签名与公证；Linux 原生包以 Ubuntu 24.04 为验证基线。旧系统、不同 NAS 型号及真实音源的可用性见[验证范围](docs/research/cross-platform-ci.md)。

## 接入自己的前端

1. 启动后端，取得服务地址与访问 Token；浏览器前端配置对应的 CORS origin。
2. 使用 [OpenAPI](docs/api/openapi.json) 或 [JS 客户端](examples/frontend/client.mjs)调用接口，先读取服务信息与实际能力。
3. 通过媒体 URL 播放音频，通过 SSE 或任务查询跟踪下载进度。

Release 中的 `music-service-<版本>-frontend-integration.zip` 包含接入指南、核心 OpenAPI、JS 客户端、TypeScript 类型和搜索 / 播放 / 下载示例。完整步骤见[前端快速接入](docs/frontend-integration.md)，账号和管理接口见[完整 API 契约](docs/design/cpp20-refactor/backend-service/api-contract.md)。

## 开发与文档

- [构建与测试](cmake/README.md) · [配置说明](config/README.md) · [账号接入](docs/account-management.md)
- [发布与版本切换](packaging/RELEASING.md) · [自动发布工作流](packaging/release-workflow.md)
- [文档索引](docs/README.md) · [第三方依赖](cmake/dependencies/README.md)

未发布提交可从成功的 [CI 运行](https://github.com/ddmoyu/music-service/actions/workflows/ci.yml)下载保存 14 天的程序包，或按构建文档自行编译。前端应固定配套的后端版本。
