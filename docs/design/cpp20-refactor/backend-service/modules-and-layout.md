# 模块划分与目录结构

业务职责见[功能模块](feature-modules.md)，操作和所有权见[内部契约](internal-contracts.md)。

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

## 公共边界

- domain 保存值对象和规则；application 定义用例及端口。
- providers 实现平台协议；downloads 编排执行；infrastructure 适配网络、存储、文件和秘密。
- api、media_server、event_server 只做协议转换与入场控制，共享应用服务。
- application/media 管理媒体授权，audio_processing 处理文件，media_server 传输字节。
- 跨模块使用项目类型和 Result<T>；C++20 不依赖 C++23 的 std::expected。

## 目录

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
      audio_processing/
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

## CMake 目标

| 目标 | 依赖方向 |
| --- | --- |
| `music_dl_domain` | C++20 标准库 |
| `music_dl_application` | domain，业务端口 |
| `music_dl_providers` | application 端口、domain、选定的 JSON／协议加密适配 |
| `music_dl_downloads` | application 端口、domain |
| `music_dl_infrastructure` | application 端口、httplib、SQLite、JSON、媒体适配等实际依赖 |
| `music_dl_http_api` | application、控制 HTTP、媒体 HTTP 与 SSE 适配 |
| `music-dl-server` | 组装上述目标与运行时 |

目标可按编译收益合并，第三方头不经公共头传播。audio_processing 初期可编入 infrastructure。Drogon 替换 HTTP／传输适配，业务用例共用。

依赖获取与版本规则见[构建与依赖](build-and-dependencies.md)。

## 新增平台



实现能力端口与 descriptor → 离线验证解析和签名 → 验证账号及在线行为 → 在组装层注册。首版静态注册，不加载动态插件。

能力缺失标记 unsupported；目录来源、品质降级、试听和第三方 relay 依赖保留在描述与结果中。
