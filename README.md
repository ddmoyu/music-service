# music-service

独立音乐后端服务，为单独维护的 GUI 和前端提供音乐搜索、音源解析、下载与任务管理等 API。本项目是纯后端，不包含 GUI；要求跨平台运行，并支持原生进程、系统服务及 Docker 部署。系统、架构和验证边界见[配置与部署](docs/design/cpp20-refactor/backend-service/configuration-and-deployment.md)。当前已初始化 C++20 工程骨架，尚无业务实现，文档入口见[项目文档](docs/README.md)。

命名约定：music-service 是产品名；仓库目录为 `music-dl`，CMake 项目与库目标前缀为 `music_dl`，服务可执行程序为 `music-dl-server`，`/api/v1/info` 的 service 字段为 `music-dl`。

## 目录结构

```text
music-dl/
  README.md
  cpp/                         # C++20 库模块、服务入口与构建骨架
    include/music_dl/          # 公共领域类型、应用契约与端口
    src/                      # 分层库模块
    apps/server/              # 服务入口与依赖组装
    cmake/                    # 构建支持与依赖接入位置
    resources/                # 配置资源
    tests/                    # 测试与去凭证样本位置
    packaging/                # 原生与容器交付位置
  docs/                        # 本项目的研究记录与设计文档
    research/
    design/
  third_party/                 # 外部项目源码，保留各自的 Git 仓库
    cinatra-source/
    cofetch-source/
    cpr-source/
    go-music-dl/
    music-lib-source/
```

外部项目源码统一放在 `third_party/`，不再直接放到项目根目录。目录用途及当前源码版本见 [第三方源码说明](third_party/README.md)。

C++ 工程位于 [cpp](cpp/README.md)，按[后端目录设计](docs/design/cpp20-refactor/backend-service/modules-and-layout.md)建立公共头、库模块和独立服务入口。当前只含构建配置、目录与说明，没有 C++ 源文件、第三方依赖或可运行服务；配置方法见[工程说明](cpp/README.md#配置与检查)。

## 文档入口

- [项目文档](docs/README.md)
- [Go Music DL 功能实现拆解](docs/research/go-music-dl/README.md)
- [C++20 重构设计](docs/design/cpp20-refactor/README.md)
