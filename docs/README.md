# 项目文档

服务定位与主要功能见[项目简介](../README.md)。外部项目源码统一存放在 [third_party](../third_party/README.md)，研究与设计文档保留在本目录。

- [Docker 使用教程](../packaging/docker/README.md)：从源码构建、初始化与认证、持久化、端口、Compose 示例及日常管理。
- [macOS 使用说明](../packaging/macos/README.md)：独立运行库、配置、launchd 启停与签名／公证边界。
- [发布说明](../packaging/RELEASING.md)：当前交付状态、预览发行范围、二进制和镜像发布前的准备与验证。
- [发布与部署验证](../packaging/README.md)：各平台打包、系统服务与容器检查脚本的范围。
- [功能完成情况](feature-status.md)：平台功能勾选表、Windows 闭环、音质实测、公共下载能力及待开发／待验收项目。
- [平台账号与 Cookie 接入指南](account-management.md)：两个 QQ 账号的创建、切换、单个／全部停用、重新启用、状态与到期精度显示、错误及重试。
- [Cookie 自动校验与会员识别验收](research/credential-membership-acceptance.md)：后台登录校验、VIP／SVIP 与到期时间、真实账号验证；账号启停、秘密清理失败恢复、并发与进程重启验收。
- [跨系统验收](research/cross-platform-ci.md)：Linux、macOS、Docker 双架构与 Windows 的 CI、运行验证、修复及交付边界。
- [macOS 独立包验收](research/macos-package-acceptance.md)：运行库、双架构原生打包、launchd、下载校验与签名边界。
- [酷狗接入验收](research/kugou-acceptance.md)：21 项匿名能力、歌手资料与完整作品分页、标准整曲、歌词／封面输出、复用及重启回读；包含审查修复记录，高品质和账号能力单列边界。
- [酷我歌手与封面验收](research/kuwo-artists-acceptance.md)：0.1.8 直接 JSON 接口、歌手资料与完整分页、MP3／FLAC 封面嵌入和导出、复用与重启回读。
- [酷我黑胶原生接入验收](research/kuwo-vinyl-acceptance.md)：黑胶映射 hires、C++ 完整 FLAC 校验、尾部附加数据处理、实际音质判定及 Windows 下载闭环。

## C++20 重构设计

- [千千音乐接入验收](research/qianqian-acceptance.md)：匿名目录、签名、真实集合分页、三档音源、元数据／侧文件与 Windows 验收边界。

- [参考项目功能补齐计划](design/cpp20-refactor/reference-gap-plan.md)：A～E、现有三平台补齐、原生 NCM 导入与侧文件的实现、验收和限制。
- [集合有限批次验收客户端](../packaging/e2e/collections.md)：三平台集合链接、网易云分页、选中或全部分批提交、停止与幂等重放。
- [项目简介](../README.md)：服务定位与主要功能。
- [构建与测试](../cmake/README.md)：原生构建预设、工具链要求及验证命令。
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
- [开发任务拆分](design/cpp20-refactor/development-tasks.md)：开发任务、模块顺序、依赖、当前实施状态和历史验收证据。
- [音乐平台开发顺序](design/cpp20-refactor/platform-roadmap.md)：Windows 优先，QQ → 酷狗 → 咪咕的范围、顺序与验收记录。
- [多平台适配与扩展](design/cpp20-refactor/backend-service/provider-extensibility.md)：开放平台清单、能力组合、目录与实际音频来源、多种凭证、原生适配和规模验证。

- [Bilibili 首阶段验收](research/bilibili-acceptance.md)：视频搜索、分 P 与 cid 身份、独立 AAC 音轨、Cookie 校验及 Windows 下载验证。

## 源码研究

- [咪咕首阶段 Windows 验收](research/migu-acceptance.md)：匿名四类搜索、实时推荐／分类／榜单、数字专辑身份、试听判定、WebP 封面和原生下载闭环。
- [咪咕 Z3D／MG3D 原生解密](research/migu-z3d.md)：开源来源与许可证、逐曲密钥、PCM WAV 校验、下载链路和验证边界。

- [Jamendo Windows 验收](research/jamendo-acceptance.md)：20 项匿名能力、精选歌单搜索、原生歌曲分类与榜单、音质核查及下载闭环。

- [酷我会员账号音质复测](research/kuwo-membership-quality-retest.md)：新账号会员验证、标准／高品质／无损 A/B、完整 FLAC 哈希与 MFLAC 等扩展参数边界。

- [酷我非 VIP Cookie 对照测试](research/kuwo-cookie-evaluation.md)：会话与会员状态、匿名接口覆盖、3 首歌曲三档音质对照，以及后续接入的歌手目录／封面来源。
- [Go 参考实现 功能实现拆解](research/go-reference/README.md)：整体架构、单曲搜索、歌单与专辑、分类、排行榜、下载、换源，以及平台差异和实现边界。
- [Lanyue 多平台音乐功能实现拆解](research/lanyue/README.md)：八个平台的搜索与音源解析、插件能力、歌单与榜单、下载队列、流式播放、换源开关，以及对 C++20 重构的参考。
- [Lanyue 搜索使用的 NPM SDK](research/lanyue/npm-sdks.md)：网易云与 QQ 的锁定依赖、SDK 搜索实现、加密与 Cookie 传递，以及原生迁移要点。
