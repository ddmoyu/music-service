# music-service

一个基于 C++20 的音乐后端服务，通过 HTTP API 提供音乐搜索、音源解析、下载与任务管理，供独立前端接入。

## 主要功能

- 音乐搜索、详情查询与链接解析
- 后台下载、批量任务、进度订阅与取消
- 下载失败后严格换源重试，按歌曲名和完整演唱者名单匹配，优先使用历史下载成功率高的平台
- 显式试听换源接口，返回实际播放平台和已尝试的平台，便于前端提示和继续换源
- 下载任务持久化与重启恢复
- 音频校验、标签写入，以及歌词和封面导出
- 文件访问与媒体流服务
- Cookie 保存后自动校验登录，查询平台会员类型与到期时间；无法确认时显示未知
- 同平台多账号记录、单账号启用和全部停用；停用清除 Cookie，再次启用需重新提交

项目仍处于开发阶段，尚未发布正式版。

## 获取与运行

先查看 [Releases](https://github.com/ddmoyu/music-service/releases) 中的预览发行包；尚未发布的提交可从源码构建，或下载成功 CI 中保存 14 天的程序包和校验和。Windows、Linux、macOS 使用同一 HTTP API，前端接入材料随包交付。

| 使用方式 | 当前支持范围 | 入口 |
| --- | --- | --- |
| Docker | Linux amd64／arm64 原生镜像，本地构建后运行 | [Docker 使用教程](packaging/docker/README.md) |
| Windows | x64，可生成 ZIP 发行包，支持前台运行和 Windows 服务 | [构建与测试](cmake/README.md)、[发行包使用说明](packaging/package/README.md) |
| Linux | x86_64／ARM64，可生成 tar.gz 发行包，支持前台运行和 systemd | [构建与测试](cmake/README.md)、[发行包使用说明](packaging/package/README.md) |
| macOS | 15.0 起，Intel／Apple Silicon 独立 tar.gz，内含 LLVM C++ 运行库，支持前台和 launchd | [macOS 使用与签名说明](packaging/macos/README.md) |

Docker 教程包含构建、首次初始化、访问 token、持久化目录、三个服务端口、日常管理及 Compose 示例。默认只允许宿主机访问；局域网或反向代理部署需要同时调整端口映射与公开地址。

版本标签可触发五个平台程序包、前端接入包和 Docker 双架构镜像的自动预览发布，流程与权限见[自动发布说明](packaging/release-workflow.md)。实际下载以发布结果为准。macOS 默认 ad-hoc 签名，尚非 Developer ID 签名、公证版本。平台与真实上游的验证范围见[跨系统验收](docs/research/cross-platform-ci.md)。

## 文档

- [第三方前端快速接入](docs/frontend-integration.md)：服务发现、Token、CORS、搜索、播放、下载及 SSE
- [核心 OpenAPI](docs/api/openapi.json)与[无依赖 JS/TS 客户端](examples/frontend/client.mjs)
- [自动构建与预览发布](packaging/release-workflow.md)

- [Docker 使用教程](packaging/docker/README.md)
- [macOS 使用说明](packaging/macos/README.md)
- [发布说明](packaging/RELEASING.md)
- [API 说明](docs/design/cpp20-refactor/backend-service/api-contract.md)
- [换源与试听接入](docs/design/cpp20-refactor/backend-service/api-contract.md#81-严格匹配与试听换源)
- [平台账号与 Cookie 接入指南](docs/account-management.md)：新增、切换、停用、会员显示及错误处理
- [配置说明](config/README.md)
- [项目文档](docs/README.md)
