# 项目文档

工作区目录见[项目入口](../README.md)。外部项目源码统一存放在 [third_party](../third_party/README.md)，研究与设计文档保留在本目录。

## C++20 重构设计

- [C++ 工程入口](../cpp/README.md)：已初始化的目录、模块依赖、CMake 预设与当前实现边界。
- [重构设计入口](design/cpp20-refactor/README.md)：当前方向、选型状态和文档导航。
- [后端服务详细设计](design/cpp20-refactor/backend-service/README.md)：C++20 + cpp-httplib 主方案的模块与目录、公共模型、API 契约、任务运行时、配置与部署。
- [功能模块详细设计](design/cpp20-refactor/backend-service/feature-modules.md)：各模块的职责、业务规则、输入输出和协作方式。
- [模块边界审查](design/cpp20-refactor/backend-service/module-boundary-review.md)：前后端分工、下载意图、账号隔离、授权扩展和调度职责；R1～R4 已补齐设计并映射验收。
- [内部接口与一致性](design/cpp20-refactor/backend-service/internal-contracts.md)：组件操作、对象所有权、数据库事务、文件发布与恢复。
- [关键流程与验收](design/cpp20-refactor/backend-service/flows-and-acceptance.md)：搜索、下载、SSE 重连、播放和故障验收矩阵。
- [构建与依赖设计](design/cpp20-refactor/backend-service/build-and-dependencies.md)：C++20、CMake、FetchContent 浅克隆、固定版本与缓存策略。
- [配置与跨平台部署](design/cpp20-refactor/backend-service/configuration-and-deployment.md)：纯后端、无 GUI 边界，系统／CPU／运行库矩阵、NAS、Docker／Compose 与服务生命周期。
- [架构评估与总览](design/cpp20-refactor/architecture.md)：设计合理性、实施风险、系统与部署边界，以及原实现迁移关系。
- [详细架构图](design/cpp20-refactor/architecture-details.md)：编译依赖、平台调用、下载时序、执行资源、发布恢复和媒体／事件通路。
- [技术栈选型](design/cpp20-refactor/technology-stack.md)：cpp-httplib 暂定主方案、Drogon 备选，以及 HTTP 客户端、TLS、加密和音频元数据等配套选型。
- [网络库对比与排序](design/cpp20-refactor/library-comparison.md)：10 个候选库的 Stars、更新频率、功能特点和当前项目的契合度。
- [实施阶段与验证](design/cpp20-refactor/implementation-plan.md)：最小验证、迁移顺序和各阶段完成条件。
- [开发任务拆分](design/cpp20-refactor/development-tasks.md)：开发任务、模块顺序、直接与可选前置、可并行分支及验收映射；当前未开始业务实现。
- [音乐平台开发顺序](design/cpp20-refactor/platform-roadmap.md)：由易到难的平台接入顺序、首批范围、排序依据与完成条件。
- [多平台适配与扩展](design/cpp20-refactor/backend-service/provider-extensibility.md)：开放平台清单、能力组合、Spotify 与实际音频来源、多种凭证、可选外部 adapter 和规模验证。

## 源码研究

- [Go Music DL 功能实现拆解](research/go-music-dl/README.md)：整体架构、单曲搜索、歌单与专辑、分类、排行榜、下载、换源，以及平台差异和实现边界。
- [Lanyue 多平台音乐功能实现拆解](research/lanyue/README.md)：八个平台的搜索与音源解析、插件能力、歌单与榜单、下载队列、流式播放、换源开关，以及对 C++20 重构的参考。
- [Lanyue 搜索使用的 NPM SDK](research/lanyue/npm-sdks.md)：网易云与 QQ 的锁定依赖、SDK 搜索实现、加密与 Cookie 传递，以及原生迁移要点。
