# C++20 重构设计

将 Go Music DL 的音乐平台适配与业务编排迁移到 C++20，先交付无界面、可独立常驻运行的音乐后端服务。网页、Electron、Tauri、Qt、Win32 等前端通过同一套 HTTP API 接入；后端通过 HTTP 客户端访问音乐平台。优先考虑 Windows、普通并发下的可用性与依赖体积；cpp-httplib 暂定为主方案，Drogon 作为备选。

更新日期：2026-10-19。状态：主框架已暂定，后端模块、目录、模型、API 与运行时详细设计已形成；尚未编写服务代码，配套技术栈待验证。

## 当前方向

| 项目 | 当前状态 |
| --- | --- |
| 语言 | 以 C++20 作为重构方案的语言基线 |
| 构建与依赖 | CMake + CMakePresets；FetchContent 集中获取第三方依赖，Git 浅克隆并固定发布 tag |
| 服务形式 | HTTP Server 对外提供接口，HTTP Client 调用平台接口 |
| 交付入口 | 先提供可独立运行的后端可执行程序，不依赖前端窗口或桌面运行时 |
| 多端接入 | 网页、Electron、Tauri、Qt、Win32、CLI 等通过统一的版本化 HTTP API 调用 |
| 部署形态 | 本机伴随服务、独立主机、NAS 或 Docker；按操作系统和 CPU 架构构建与验证 |
| 任务生命周期 | 下载任务归后端持有；关闭前端后继续运行，进程重启后的恢复依赖持久化策略 |
| 网络模型 | 首轮采用 cpp-httplib 阻塞 I/O 配合有界线程池；搜索和下载分别限制并发，不将协程作为前置条件 |
| 服务框架 | cpp-httplib 暂定主方案，Drogon 为备选；其他库保留选型参考 |
| HTTP 客户端 | 先验证 cpp-httplib Client 与 Server 的组合；TLS 或下载需求必要时独立替换客户端 |
| TLS | 优先评估 Windows Schannel；同时保留 OpenSSL、Botan 等后端的取舍 |
| 音频处理 | 以直接保存原始音频和写入元数据为主要需求，评估减少 FFmpeg 依赖 |
| 实施状态 | 当前只整理设计和选型记录，尚未建立 C++ 工程或引入这些依赖 |

这里的功能规划与技术建议用于后续实现，不表示已完成迁移。原 Go 项目的实际行为与新方案的改进项分开记录，避免把设计目标当作现有能力。

## 文档导航

| 文档 | 内容 |
| --- | --- |
| [后端服务详细设计](backend-service/README.md) | 当前 C++20 + cpp-httplib 主方案的设计基线、文档分工与功能分期 |
| [功能模块详细设计](backend-service/feature-modules.md) | 搜索与发现、平台适配、音源与换源、下载、播放、SSE、身份与存储的具体职责 |
| [内部接口与一致性](backend-service/internal-contracts.md) | 操作契约、执行上下文、对象所有权、事务、索引和崩溃恢复 |
| [关键流程与验收](backend-service/flows-and-acceptance.md) | 搜索／下载／播放时序，断线与取消处理，28 项故障验收条件 |
| [模块与目录结构](backend-service/modules-and-layout.md) | 组件职责、依赖方向、内部端口、拟建源代码目录与构建目标 |
| [公共模型与存储](backend-service/data-models.md) | 身份、品质、分页、任务快照、资产、幂等和 SQLite 表 |
| [对外 HTTP API](backend-service/api-contract.md) | 完整路由、权限、错误、批次、SSE、HTTP 进度查询与媒体契约 |
| [运行时与后台任务](backend-service/runtime-and-tasks.md) | 三套 httplib 监听、线程额度、下载状态机、事件分发、取消、续传、恢复与流桥接 |
| [配置与部署](backend-service/configuration-and-deployment.md) | 初始默认值、配置来源、数据布局、Windows／NAS／Docker 与生命周期 |
| [构建与依赖](backend-service/build-and-dependencies.md) | C++20、CMake、FetchContent 浅克隆、版本固定、缓存与工具链隔离 |
| [重构架构](architecture.md) | 迁移功能、层次划分、模型、API 草案、下载与换源流程 |
| [技术栈选型](technology-stack.md) | 各候选库的适用位置、依赖关系、限制和待验证问题 |
| [网络库对比与排序](library-comparison.md) | 10 个库的 Stars、近 30 天与 90 天提交、Release、功能特点和项目契合度 |
| [实施阶段与验证](implementation-plan.md) | 网络验证、平台迁移、任务下载、元数据和发布验证 |
| [Go Music DL 功能实现拆解](../../research/go-music-dl/README.md) | 原项目的源码依据、功能细节、平台差异和现有边界 |
| [Lanyue 多平台音乐功能实现拆解](../../research/lanyue/README.md) | Electron 业务分层、八平台搜索与取流、下载队列、播放和换源策略的补充参考 |

## 后续选型记录规则

当前围绕 cpp-httplib 推进验证，Drogon 保留为备选。新增网络库先作为研究记录，不自动重新开启主框架选型。服务端、普通平台请求、文件下载和 TLS 后端分别记录，避免因为暂定框架就默认锁定所有网络依赖。

同一套应用可以使用框架自带客户端，也可以通过适配层使用独立客户端。只有在 HTTPS、Cookie、重定向、流式下载、取消和并发限制等关键路径得到验证后，才决定是否移除 curl。

## 原项目基准

| 仓库 | 分析提交 |
| --- | --- |
| [go-music-dl](https://github.com/guohuiyuan/go-music-dl) | `ddf2a4e550ca67086ff1f06edc5d15c7d4c8663d` |
| [music-lib](https://github.com/guohuiyuan/music-lib) | `7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec` |

迁移时以固定提交理解原始行为，再单独验证上游平台接口；平台接口随时可能发生变化。
