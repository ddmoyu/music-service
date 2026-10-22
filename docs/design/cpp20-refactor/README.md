# C++20 重构设计

将 Go 参考实现 的音乐平台适配与业务编排迁移到 C++20。本项目是纯后端，不开发或依赖 GUI，以可独立常驻运行的跨平台服务交付，并支持 Docker。网页、Electron、Tauri、Qt、Win32 等均为外部客户端，通过同一套 HTTP API 接入；后端通过 HTTP 客户端访问音乐平台。Windows 与 Linux 从基础阶段共同验证，兼顾普通并发下的可用性与依赖体积；cpp-httplib 暂定为主方案，Drogon 作为备选。

更新日期：2026-10-20。状态：工程骨架已初始化，开发任务已拆分，业务未实现，配套依赖待验证。

## 当前方向

| 项目 | 当前状态 |
| --- | --- |
| 语言 | 以 C++20 作为重构方案的语言基线 |
| 构建与依赖 | CMake + CMakePresets；FetchContent 集中获取第三方依赖，Git 浅克隆并固定发布 tag |
| 服务形式 | HTTP Server 对外提供接口，HTTP Client 调用平台接口 |
| 产品边界 | 纯后端、无 GUI；构建与启动不依赖桌面会话、浏览器、音频设备或前端运行时 |
| 交付入口 | 先提供可独立运行的后端可执行程序，不依赖前端窗口或桌面运行时 |
| 多端接入 | 网页、Electron、Tauri、Qt、Win32、CLI 等通过统一的版本化 HTTP API 调用 |
| 部署形态 | 本机伴随服务、独立主机、NAS 或 Docker；按操作系统和 CPU 架构构建与验证 |
| 跨平台基线 | Windows、Linux 为必需目标；首批架构建议为 Windows x64、Linux x86_64／ARM64，macOS 为后续候选，详见配置与部署 |
| 任务生命周期 | 下载任务归后端持有；关闭前端后继续运行，进程重启后的恢复依赖持久化策略 |
| 网络模型 | 首轮采用 cpp-httplib 阻塞 I/O 配合有界线程池；搜索和下载分别限制并发，不将协程作为前置条件 |
| 服务框架 | cpp-httplib 暂定主方案，Drogon 为备选；其他库保留选型参考 |
| HTTP 客户端 | 先验证 cpp-httplib Client 与 Server 的组合；TLS 或下载需求必要时独立替换客户端 |
| TLS | 先验证覆盖 Windows／Linux 与容器的实现；Schannel 仅作为 Windows 可选适配，不成为其他系统的依赖 |
| 音频处理 | 以直接保存原始音频和写入元数据为主要需求，评估减少 FFmpeg 依赖 |
| 音乐平台 | 酷我优先；已研究平台由易到难，Spotify／海外来源持续加入，不设固定平台数量上限；见[平台开发计划](platform-roadmap.md) |
| 平台扩展 | 能力组合与开放注册；目录源、实际音源和实现工具分开；全部原生实现，不调用外部下载工具或第三方解析服务 |
| 实施状态 | T01～T03 已完成（固定版本依赖、领域规则与端口、配置／时钟／日志），CMake 工程位于[仓库根目录](../../../README.md)；尚无平台实现与服务程序 |

## 文档导航

| 文档 | 内容 |
| --- | --- |
| [后端服务详细设计](backend-service/README.md) | 当前 C++20 + cpp-httplib 主方案的设计基线、文档分工与功能分期 |
| [功能模块详细设计](backend-service/feature-modules.md) | 搜索与发现、平台适配、音源与换源、后端下载、媒体访问、SSE、身份与存储 |
| [内部接口与一致性](backend-service/internal-contracts.md) | 操作契约、执行上下文、对象所有权、事务、索引和崩溃恢复 |
| [关键流程与验收](backend-service/flows-and-acceptance.md) | 搜索／下载／媒体访问时序，断线与取消处理，行为验收矩阵 |
| [模块边界审查](backend-service/module-boundary-review.md) | 前后端职责、已确认边界及 R1～R4 的设计补齐、规范归属和验收映射 |
| [模块与目录结构](backend-service/modules-and-layout.md) | 组件职责、依赖方向、内部端口、拟建源代码目录与构建目标 |
| [公共模型与存储](backend-service/data-models.md) | 身份、品质、分页、任务快照、资产、幂等和 SQLite 表 |
| [对外 HTTP API](backend-service/api-contract.md) | 完整路由、权限、错误、批次、SSE、HTTP 进度查询与媒体契约 |
| [运行时与后台任务](backend-service/runtime-and-tasks.md) | 三套 httplib 监听、线程额度、下载状态机、事件分发、取消、续传、恢复与流桥接 |
| [配置与部署](backend-service/configuration-and-deployment.md) | 纯后端边界、系统与架构矩阵、跨平台约束、默认值、数据布局和 Docker／NAS 生命周期 |
| [构建与依赖](backend-service/build-and-dependencies.md) | C++20、CMake、FetchContent 浅克隆、版本固定、缓存与工具链隔离 |
| [架构评估与总览](architecture.md) | 合理性判断、实施风险、系统与部署边界、迁移关系 |
| [详细架构图](architecture-details.md) | 编译依赖、用例与平台、下载时序、资源调度、发布恢复、媒体与事件六个视图 |
| [技术栈选型](technology-stack.md) | 各候选库的适用位置、依赖关系、限制和待验证问题 |
| [网络库对比与排序](library-comparison.md) | 10 个库的 Stars、近 30 天与 90 天提交、Release、功能特点和项目契合度 |
| [实施阶段与验证](implementation-plan.md) | 网络验证、平台迁移、任务下载、元数据和发布验证 |
| [开发任务拆分](development-tasks.md) | S0～S5 任务、直接／可选前置、平台扩展与验收责任 |
| [音乐平台开发顺序](platform-roadmap.md) | 平台难度、确定的接入顺序、首批范围和完成条件 |
| [多平台适配与扩展](backend-service/provider-extensibility.md) | 平台包、注册、能力、凭证、原生实现原则、Spotify 路线与扩展验收 |
| [Go 参考实现 功能实现拆解](../../research/go-reference/README.md) | 原项目的源码依据、功能细节、平台差异和现有边界 |
| [Lanyue 多平台音乐功能实现拆解](../../research/lanyue/README.md) | Electron 业务分层、八平台搜索与取流、下载队列、播放和换源策略的补充参考 |

## 后续选型记录规则

当前围绕 cpp-httplib 推进验证，Drogon 保留为备选。服务端、客户端、下载传输和 TLS 分别验证。

同一套应用可以使用框架自带客户端，也可以通过适配层使用独立客户端。只有在 HTTPS、Cookie、重定向、流式下载、取消和并发限制等关键路径得到验证后，才决定是否移除 curl。

## 原项目基准

| 仓库 | 分析提交 |
| --- | --- |
| [Go 参考应用](https://github.com/guohuiyuan/go-music-dl) | `ddf2a4e550ca67086ff1f06edc5d15c7d4c8663d` |
| [music-lib](https://github.com/guohuiyuan/music-lib) | `7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec` |

迁移时以固定提交理解原始行为，再单独验证上游平台接口；平台接口随时可能发生变化。
