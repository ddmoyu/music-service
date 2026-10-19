# music-service

独立音乐后端服务，为单独维护的 GUI 和前端提供音乐搜索、音源解析、下载与任务管理等 API。当前处于研究与设计阶段，文档入口见 [项目文档](docs/README.md)。

## 目录结构

```text
music-service/
  README.md
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

后续本项目的 C++ 工程按[后端目录设计](docs/design/cpp20-refactor/backend-service/modules-and-layout.md)放入 `cpp/`；当前尚未创建该工程。

## 文档入口

- [项目文档](docs/README.md)
- [Go Music DL 功能实现拆解](docs/research/go-music-dl/README.md)
- [C++20 重构设计](docs/design/cpp20-refactor/README.md)
