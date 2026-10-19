# 模块划分与目录结构

设计基线与导航见[入口](README.md)。下面的目录和构建目标均为拟建结构，本轮没有创建任何 C++ 文件。

各功能具体行为见[功能模块详细设计](feature-modules.md)，内部操作与对象所有权见[内部契约](internal-contracts.md)，跨模块流程和验收见[关键流程](flows-and-acceptance.md)。本页维护依赖与目录边界，不重复完整业务规则。

## 依赖方向

```text
apps/server（组装与生命周期）
  ├── api/httplib（控制入口）
  ├── media_server/httplib（媒体入口）
  ├── event_server/httplib（SSE 入口）
  ├── application（业务用例与出站端口）
  ├── providers（平台协议实现）
  ├── downloads（任务执行）
  └── infrastructure（网络、数据库、文件、配置实现）

api、media_server、event_server → application → domain
providers → application/ports + domain
downloads → application/ports + domain
infrastructure → application/ports + domain
```

箭头表示编译依赖。应用层通过端口访问具体平台、数据库和传输实现，组装层负责注入；不得从 domain 反向包含 httplib、SQLite、JSON 或具体 Provider。`downloads` 通过端口调用解析与匹配用例，应用服务通过 `DownloadQueuePort` 发起任务，避免二者互相包含具体实现而形成环。

## 模块职责

| 模块 | 核心组件 | 输入／输出 | 明确边界 |
| --- | --- | --- | --- |
| `domain` | Track、集合、分页、质量、错误、任务状态 | 值对象、枚举、规则结果 | 不联网、不操作路径、不返回 JSON |
| `application/catalog` | SearchService、CollectionService、DiscoveryService | 查询与 SourceResult | 聚合、分页与部分成功；不持有服务端 Request |
| `application/assets` | ResolveService、ProbeService、MatchService | TrackRef → 资源描述／候选 | 权限、品质、版本、来源追踪；不直接写文件 |
| `application/tasks` | DownloadCommandService、TaskQueryService | 创建／取消／重试命令 | 事务提交、幂等、授权；不在 HTTP 请求里下载整首歌 |
| `application/events` | TaskEventService、TaskEventPort | 已提交版本 → 按身份分发进度投影 | 有界订阅，失败触发重新同步，不作为持久事实来源 |
| `application/media` | PlaybackService、MediaTicketService、FileAccessService | 会话、票据、授权文件句柄 | 资源授权和稳定性；不解释平台私有 JSON |
| `application/accounts` | CredentialService、PrincipalService | 凭证写入／校验与身份上下文 | 凭证仅在必要阶段交给对应平台 |
| `application/ports` | ProviderRegistry、HttpTransport、TaskRepository 等 | 自有领域类型 | 定义可替换的出站能力，不包含第三方类型 |
| `providers` | 各平台能力实现、解析、签名参数、ID 还原 | 平台请求 → 统一实体 | 不保存下载任务，不读取全局当前用户 Cookie |
| `downloads` | Scheduler、TaskRunner、RetryPolicy、Finalizer | 已提交任务 → 状态／资产 | 平台解析、传输、媒体阶段分离；不创建独立 HTTP 服务 |
| `transport` | HttplibTransport、ClientLease、RedirectPolicy | 有界响应／字节块 | TLS、连接、取消、大小与重定向策略统一实现 |
| `media` | FormatDetector、Decryptor、TagWriter | 临时文件 → 文件与处理报告 | 标签失败与传输失败区分；不管理队列公平性 |
| `storage` | SqliteRepositories、DbExecutor、MigrationRunner | 实体快照、事务 | 不持数据库事务执行网络或整文件操作 |
| `filesystem` | DestinationResolver、PartialStore、AssetStore | 受授权目录 ID、任务 ID | 路径校验、唯一临时文件、移动与资产存在性 |
| `runtime` | Executors、Admission、RequestContext、ShutdownCoordinator | 任务句柄、截止时间、取消 | 并发和生命周期，不决定歌曲匹配规则 |
| `api/httplib` | 路由、DTO、认证、错误映射 | HTTP ↔ 应用请求 | 只做协议转换和入场检查，不复制平台规则 |
| `media_server/httplib` | RangeHandler、FileResponder、StreamBridge | 授权媒体读取 | 使用独立线程池；不把整个音频放进字符串 |
| `event_server/httplib` | SseResponder、Subscription | 授权订阅 → SSE 帧／心跳 | 独立线程池、连接配额、断开清理；不在下载线程写 socket |
| `observability` | 结构化日志、请求与任务指标 | 去敏事件、计数和耗时 | 不记录 Cookie、票据或 URL 中的签名 |
| `apps/server` | CompositionRoot、ProcessHost | 配置 → 完整运行实例 | 唯一依赖具体实现的组装位置 |

## 用例调用关系

| 用例 | 顺序 | 完成点 |
| --- | --- | --- |
| 搜索 | 认证 → 能力与参数 → 短请求入场 → 逐源 Provider → 聚合 | 截止时间内返回已完成结果及逐源状态 |
| 创建下载 | 参数 → 输出目录权限 → 幂等与去重事务 → 唤醒调度 | 事务已提交，返回 task_id |
| 执行下载 | 领取 → 解析 → 探测 → 传输 → 文件处理 → 发布资产 | 文件与资产记录已完成协调提交 |
| 手动换源 | 原曲元信息 → 允许来源搜索 → 版本匹配 → 有限探测 | 返回候选及理由，不自动修改原曲身份 |
| 文件获取 | 身份／票据 → 资产权限 → 文件句柄 → Range 响应 | 下游传输结束，不创建新的平台下载 |
| 在线播放 | 建立会话 → 解析确定录音 → 每次 Range 建上游流 | 请求结束释放传输额度，会话可继续用于下一次 seek |

## 关键内部契约

下表是接口职责描述，不是本轮要编写的 C++ 代码。

| 契约 | 责任与失败语义 |
| --- | --- |
| `Result<T>` | 项目自有结果类型，包含值或 DomainError；不把 C++23 `std::expected` 当作 C++20 标准能力 |
| `RequestContext` | principal、request_id、绝对截止时间、取消 token、凭证选择；任务复制必要值，不持悬空引用 |
| `ProviderDescriptor` | 来源 ID、能力、品质提示、目录类型和账号需求；名称与可用状态分开 |
| `ITrackSearch` 等能力端口 | 每次执行一个平台操作，接收上下文和平台专属游标，返回统一分页 |
| `IAssetResolver` | 输入歌曲与品质策略，返回内部资源候选；对外投影删除 URL、请求头和密钥 |
| `HttpTransport.request` | 有界 JSON／文本响应，保留 HTTP 状态、响应头及分类错误 |
| `HttpTransport.receive` | 响应头先校验，再把字节块交给消费者；消费者拒绝或取消能停止读取 |
| `TaskRepository` | 创建、租约领取、按版本转换、终态与幂等事务；所有转换检查当前状态 |
| `TaskEventPort` | 分发已提交的公开任务投影；按 owner 隔离，不进行 socket 写入；失败使订阅重新同步 |
| `TaskControlPort` | 对当前 execution_id 请求停止；信号发出不等于执行结束 |
| `AssetStore` | 分配任务独占工作路径、验证最终路径、发布文件、打开只读资产句柄 |
| `TagWriter` | 声明支持格式与字段，返回成功字段、警告、致命错误；不暗中转码 |
| `SecretStore` | 按 principal、source、credential_id 读取秘密，普通 API 只得到状态 |
| `DownloadQueuePort` | 唤醒／停止调度、请求取消；SQLite 快照是事实来源，内存通知不是持久队列 |

客户端租约包含目标 origin、凭证版本、代理与 TLS 策略，执行期间独占。即使某个 httplib 接口允许跨线程调用，也不让一个请求的 stop／Cookie／header 修改影响另一个请求。

## 拟建仓库目录

```text
music-dl/
  README.md
  docs/
    research/
    design/cpp20-refactor/backend-service/
  third_party/                     # 外部项目源码，独立于本项目实现
    cinatra-source/
    cofetch-source/
    cpr-source/
    go-music-dl/
    music-lib-source/
  cpp/
    CMakeLists.txt
    CMakePresets.json
    cmake/                         # 工具链、编译选项、依赖与安装规则
      dependencies/               # FetchContent 声明、固定 tag 与 commit 校验
    apps/
      server/
        main.cpp                   # 参数与 ProcessHost，保持薄入口
        composition_root.cpp
        process_host.cpp
    include/music_dl/
      domain/
        catalog/                   # Track、Album、Playlist、Artist、Page
        assets/                    # 质量、资源、匹配、可用性
        tasks/                     # Task、Batch、状态转换
        identity/                  # Principal、CredentialRef
        error.hpp
        result.hpp
      application/
        catalog/
        assets/
        tasks/
        events/                    # 任务事件用例与订阅模型
        media/
        accounts/
        ports/                     # 仅自有模型的出站接口
    src/
      domain/                      # 值规则、状态转换等非内联实现
      application/
        catalog/
        assets/
        tasks/
        events/                    # 按 principal 分发，持久状态提交后发布
        media/
        accounts/
      providers/
        registry/
        common/                    # 平台共用编码等，不存通用业务策略
        netease/
          descriptor.cpp
          search.cpp
          collections.cpp
          resolve.cpp
          lyrics.cpp
          parsing/
          protocol/
        qq/
          descriptor.cpp
          search.cpp
          collections.cpp
          resolve.cpp
          parsing/
          protocol/
        .../                       # 首批确认后逐个平台增加
      downloads/
        scheduler.cpp
        task_runner.cpp
        retry_policy.cpp
        resume_policy.cpp
        finalizer.cpp
        recovery.cpp
      media/
        format/
        decrypt/
        tags/
      infrastructure/
        transport/httplib/
        storage/sqlite/
          repositories/
          migrations/
        filesystem/
        secrets/
        config/
        observability/
      runtime/
        executors/
        admission/
        cancellation/
        shutdown/
      api/httplib/
        server.cpp
        middleware/
        dto/
        routes/
          info.cpp
          catalog.cpp
          assets.cpp
          downloads.cpp
          media_control.cpp
          credentials.cpp
          administration.cpp
      media_server/httplib/
        server.cpp
        range_handler.cpp
        file_responder.cpp
        stream_bridge.cpp
      event_server/httplib/
        server.cpp
        sse_responder.cpp
        subscription.cpp
    resources/
      config/                      # 无凭证的示例配置
    tests/
      unit/                        # 协议向量、解析、匹配与状态机
      integration/                 # 本地模拟上游、HTTP、SQLite 与文件
      contract/                    # API 示例、错误和权限边界
      fixtures/                    # 去凭证响应与小音频样本
    packaging/
      windows/
      linux/
      docker/
```

`include/` 仅放确实跨模块使用的公共头；实现细节头跟随对应 `src/` 目录。不要求每个表或路由单独建一个库，也不为未来平台先创建空目录。

运行数据不在源码树或程序安装目录：数据库、凭证、缓存和工作文件放到配置的 data_dir；用户音频放到 destination 对应目录。具体运行布局见[配置与部署](configuration-and-deployment.md)。

## 拟建 CMake 目标

| 目标 | 依赖方向 |
| --- | --- |
| `music_dl_domain` | C++20 标准库 |
| `music_dl_application` | domain，业务端口 |
| `music_dl_providers` | application 端口、domain、选定的 JSON／协议加密适配 |
| `music_dl_downloads` | application 端口、domain |
| `music_dl_infrastructure` | application 端口、httplib、SQLite、JSON、媒体适配等实际依赖 |
| `music_dl_http_api` | application、控制 HTTP、媒体 HTTP 与 SSE 适配 |
| `music-dl-server` | 组装上述目标与运行时 |

这些目标表达边界，可按实际编译收益合并静态库。第三方头不经公共头层层传播。Drogon 若日后启用，应替换 HTTP 与传输适配，不同时维护两套业务实现。

依赖统一通过 CMake FetchContent 获取，Git 源启用浅克隆；固定 tag 与版本校验、非标准上游构建接入、缓存和 Preset 规则见[构建与依赖](build-and-dependencies.md)。

## 新增平台的步骤

先实现需要的能力端口，提供 descriptor 与可用品质描述，再显式加入组装注册表；用离线响应验证解析，协议签名／加密用固定向量，最后按需验证在线行为。显式注册优先，不在首版实现动态 DLL 插件装载。

平台不提供的能力必须缺省标为 unsupported。目录“内置”“实时”“搜索包装”、实际品质降级、试听限制和第三方 relay 依赖均写入描述及结果，不能仅以“支持该平台”概括所有能力。
