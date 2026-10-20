# 模块划分与目录结构

业务职责见[功能模块](feature-modules.md)，操作和所有权见[内部契约](internal-contracts.md)。

工程按本页结构建立在仓库根目录，当前进度见[根目录说明](../../../../README.md)：domain、application 端口与配置、infrastructure 的配置和可观测性已实现，其余模块暂用 INTERFACE 目标记录依赖，首次引入真实源码时转为普通库。下方的具体文件名表示后续实现位置；服务入口随 T10 加入，不创建空 main。

## 依赖方向

```text
src/server（组装与生命周期）
  ├── api/httplib（控制入口）
  ├── media_server/httplib（媒体入口）
  ├── event_server/httplib（SSE 入口）
  ├── application（业务用例与出站端口）
  ├── providers（平台协议实现）
  ├── downloads（任务执行）
  ├── audio_processing（文件处理）
  ├── runtime（执行器与统一入场）
  └── infrastructure（网络、数据库、文件、配置实现）

api、media_server、event_server → application → domain
providers → application/ports + domain
downloads → application/ports + domain
infrastructure → application/ports + domain
audio_processing、runtime → application/ports + domain
```

箭头表示编译依赖。应用层通过端口访问具体平台、数据库和传输实现，组装层负责注入；不得从 domain 反向包含 httplib、SQLite、JSON 或具体 Provider。`downloads` 通过端口调用解析与匹配用例，应用服务通过 `DownloadQueuePort` 发起任务，避免二者互相包含具体实现而形成环。

## 公共边界

- domain 保存值对象和规则；application 定义用例及端口。
- providers 实现平台协议；downloads 编排执行；infrastructure 适配网络、存储、文件和秘密。
- 每个平台以 ProviderBundle 组合能力；原生平台与可选外部 helper 共用端口，平台内部不持有任务队列或 SQL。扩展契约见[多平台适配](provider-extensibility.md)。
- api、media_server、event_server 只做协议转换与入场控制，共享应用服务。
- application/media 管理媒体授权，audio_processing 处理文件，media_server 传输字节。
- application/tasks 在事务外归一下载意图；domain 定义指纹纯规则，Repository 原子保存和去重，downloads 只执行持久意图。
- application/accounts 选择账号、生成完整身份键、合并刷新并协调提交；providers/<source>/auth 实现注册的授权协议，infrastructure 不放平台刷新分支。
- downloads/Scheduler 负责持久任务和执行槽；runtime/admission 实现进程内唯一的 UpstreamAdmissionPort，Transport／helper 按操作租约执行，不各自计数。
- 跨模块使用项目类型和 Result<T>；C++20 不依赖 C++23 的 std::expected。
- Windows／POSIX 原生接口限定在进程、文件、权限、网络、秘密等适配实现；业务公共头不暴露系统类型，入口不引入 GUI 或桌面运行时。

## 目录

本仓库是跨平台的纯后端：只产出 music-dl-server 服务程序，不发布 C++ 库，也不包含任何前端代码，GUI 与网页在独立仓库开发。因此 CMake 工程直接位于仓库根目录；头文件与实现放在同一模块目录，所有模块以 `src/` 为包含根（`#include "domain/result.hpp"`），不另设 `include/` 目录；`src/server` 是服务自身的进程入口。

```text
music-dl/
  CMakeLists.txt
  CMakePresets.json
  cmake/                           # 编译选项、依赖与第三方适配、工具链
    dependencies/                  # FetchContent 声明、固定 tag 与 commit 校验
    adapters/                      # cpp-httplib（Mbed TLS）、SQLite 等适配目标
    toolchains/
  src/
    domain/                        # 值对象与纯规则，.hpp 与 .cpp 同目录
      catalog/                     # 平台引用、Track、Album、Playlist、Artist、Page
      assets/                      # 品质、资源判定、匹配、可用性
      tasks/                       # 任务状态迁移、NormalizedDownloadIntent 与指纹
      identity/                    # AccountBinding、AuthIdentityKey
      detail/                      # 模块内部头文件
      error.hpp
      result.hpp
    application/
      config/                      # 生效配置 ServiceConfig（纯数据）
      ports/                       # 仅使用自有模型的出站接口
        providers/                 # descriptor、能力／授权端口、bundle 与注册契约
        runtime/                   # 上游入场与租约、时钟、操作上下文
        transport/                 # 上游 HTTP
        storage/                   # 任务等仓储
      catalog/
      assets/
      tasks/                       # 意图归一、命令／查询，不运行下载循环
      events/                      # 按 principal 分发，持久状态提交后发布
      media/
      accounts/                    # 账号选择、身份键、刷新合并和条件提交
    providers/
      registry/                    # 构建清单生成工厂绑定，运行时冻结
      common/                      # 平台共用编码／OAuth 协议工具，不存账号或业务状态
      external/                    # 可选 helper 协议适配，不存第二套下载队列
      kuwo/                        # 首个闭环平台
        CMakeLists.txt             # 独立平台 target 与注册项
        descriptor.cpp
        search.cpp
        collections.cpp
        resolve.cpp
        lyrics.cpp
        auth/                      # 按已启用 profile 实现验证；刷新仅按需增加
        parsing/
        protocol/
      fivesing/                    # 首批扩展起点，结构同 kuwo
      .../                         # 按平台开发顺序逐个增加
    downloads/
      scheduler.cpp                # 持久任务、公平性与执行槽，不计平台网络额度
      task_runner.cpp
      retry_policy.cpp
      resume_policy.cpp
      finalizer.cpp
      recovery.cpp
    audio_processing/
      format/
      decrypt/
      tags/
    infrastructure/
      config/                      # 配置加载与校验
      observability/               # 结构化日志、脱敏、关联 ID、指标
      clock.hpp
      transport/httplib/
      storage/sqlite/
        repositories/
        migrations/
      filesystem/
      secrets/
      auth/                        # 服务 token 校验等通用实现，不放平台 OAuth 协议
      external_tools/              # 可选进程、协议、取消与版本检查
    runtime/
      executors/
      admission/                   # 唯一上游额度账本、联合入场与冷却
      cancellation/
      shutdown/
    api/httplib/                   # 控制入口：server、middleware、dto、routes
    media_server/httplib/          # 媒体入口：server、range、文件响应、stream_bridge
    event_server/httplib/          # SSE 入口：server、sse_responder、subscription
    server/                        # music-dl-server：main、ProcessHost、CompositionRoot、子命令
  tests/
    dependencies/                  # 固定依赖的版本与可用性
    unit/                          # 领域规则、端口替身、配置与日志、协议向量与解析
    integration/                   # 本地模拟上游、HTTP、SQLite 与文件
    contract/                      # API 示例、错误和权限边界
    fixtures/providers/            # 按 source 和上下文版本保存的去凭证样本
  config/                          # 配置键名说明与无秘密的示例配置
  packaging/                       # windows、linux、docker
  docs/
  third_party/                     # 研究用外部源码，不参与构建
```

只在一个模块内部使用的头文件放在该模块的 `detail/` 或具体子目录，其他模块不得包含；跨模块使用的类型才放在模块的公共位置。不要求每个表或路由单独建一个库，也不为未来平台先创建空目录。

auth_profiles 工厂与平台能力一起注册；UpstreamAdmissionPort 实例、账号服务及传输适配由 CompositionRoot 连接。新增这些契约不增加独立进程或每平台线程池。

运行数据不在源码树或程序安装目录：数据库、凭证、缓存和工作文件放到配置的 data_dir；用户音频放到 destination 对应目录。具体运行布局见[配置与部署](configuration-and-deployment.md)。

## CMake 目标

| 目标 | 依赖方向 |
| --- | --- |
| `music_dl_domain` | C++20 标准库 |
| `music_dl_application` | domain，业务端口 |
| `music_dl_provider_<source>` | application 端口、domain、该平台实际需要的 JSON／协议加密适配 |
| `music_dl_providers` | 聚合选中的平台 target、注册表与生成工厂绑定，不强制链接所有平台依赖 |
| `music_dl_downloads` | application 端口、domain |
| `music_dl_audio_processing` | application 端口、domain；具体格式、解密和标签依赖后续接入 |
| `music_dl_infrastructure` | application 端口、httplib、SQLite、JSON、媒体适配等实际依赖 |
| `music_dl_runtime` | application 端口、domain；执行器、额度、取消与停机 |
| `music_dl_http_api` | application、控制 HTTP 适配 |
| `music_dl_media_server` | application、媒体 HTTP 适配 |
| `music_dl_event_server` | application、SSE HTTP 适配 |
| `music-dl-server`（待实现） | 组装上述目标与运行时，当前没有可执行目标 |

目标可按编译收益合并，第三方头不经公共类型传播。music_dl_domain 与 music_dl_infrastructure 已是静态库，music_dl_application 目前是只含端口头文件的 INTERFACE 目标；尚未创建具体平台 target。Drogon 替换 HTTP／传输适配，业务用例共用。

依赖获取与版本规则见[构建与依赖](build-and-dependencies.md)。

上面的细分文件表示目标结构，实际已有文件以仓库为准；实现后再补打包规则。Windows／Linux 原生包、Dockerfile 与 Compose 将共用服务可执行目标，macOS 等候选目标启用后再增加相应打包目录。系统和架构支持以[部署矩阵](configuration-and-deployment.md#部署方式)为准。

## 新增平台



增加平台包及构建注册项 → 实现所需能力端口与 descriptor → 离线验证解析和签名 → 验证账号及在线行为 → 在发行 preset 启用。首版静态注册，不加载动态 C++ 插件；按需加入的外部工具使用版本化 helper 协议。新增已存在能力的平台不改通用路由、任务状态机或新增平台专表，具体模板和验收见[扩展设计](provider-extensibility.md#9-新平台交付模板与验收)。

能力缺失标记 unsupported；目录来源、品质降级、试听和第三方 relay 依赖保留在描述与结果中。
