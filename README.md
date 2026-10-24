# music-service

一个基于 C++20 的音乐后端服务，通过 HTTP API 提供音乐搜索、音源解析、下载与任务管理，供独立前端接入。

## 主要功能

- 音乐搜索、详情查询与链接解析
- 后台下载、批量任务、进度订阅与取消
- 下载任务持久化与重启恢复
- 音频校验、标签写入，以及歌词和封面导出
- 文件访问与媒体流服务
- Cookie 保存后自动校验登录，查询平台会员类型与到期时间；无法确认时显示未知
- 同平台多账号记录、单账号启用和全部停用；停用清除 Cookie，再次启用需重新提交

项目仍处于开发阶段，尚未发布正式版。

## 获取与运行

尚无公开 Release 或已发布容器镜像，可从源码构建。macOS CI 在独立包检查通过后保存压缩包与校验和，下载入口和对应提交见 [macOS 独立包验收](docs/research/macos-package-acceptance.md)；Actions 预览产物保留 14 天。

| 使用方式 | 当前支持范围 | 入口 |
| --- | --- | --- |
| Docker | Linux amd64／arm64 原生镜像，本地构建后运行 | [Docker 使用教程](packaging/docker/README.md) |
| Windows | x64，可生成 ZIP 发行包，支持前台运行和 Windows 服务 | [构建与测试](cmake/README.md)、[发行包使用说明](packaging/package/README.md) |
| Linux | x86_64／ARM64，可生成 tar.gz 发行包，支持前台运行和 systemd | [构建与测试](cmake/README.md)、[发行包使用说明](packaging/package/README.md) |
| macOS | 15.0 起，Intel／Apple Silicon 独立 tar.gz，内含 LLVM C++ 运行库，支持前台和 launchd | [macOS 使用与签名说明](packaging/macos/README.md) |

Docker 教程包含构建、首次初始化、访问 token、持久化目录、三个服务端口、日常管理及 Compose 示例。默认只允许宿主机访问；局域网或反向代理部署需要同时调整端口映射与公开地址。

计划先提供 Windows／Linux／macOS 预览发行包和 Docker 双架构镜像，具体准备项见[发布说明](packaging/RELEASING.md)。macOS 默认 ad-hoc 签名，尚非 Developer ID 签名、公证版本。平台与真实上游的验证范围见[跨系统验收](docs/research/cross-platform-ci.md)。

## 文档

- [Docker 使用教程](packaging/docker/README.md)
- [macOS 使用说明](packaging/macos/README.md)
- [发布说明](packaging/RELEASING.md)
- [API 说明](docs/design/cpp20-refactor/backend-service/api-contract.md)
- [平台账号与 Cookie 接入指南](docs/account-management.md)：新增、切换、停用、会员显示及错误处理
- [配置说明](config/README.md)
- [项目文档](docs/README.md)
