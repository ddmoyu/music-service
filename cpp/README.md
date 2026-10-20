# C++ 后端工程

当前只初始化工程结构和构建边界，没有 .cpp／.hpp、平台实现、服务入口或测试代码。第三方依赖尚未接入，配置过程不需要下载依赖。

## 目录与职责

```text
cpp/
  CMakeLists.txt
  CMakePresets.json
  cmake/                         构建选项、依赖、适配与工具链
  include/music_dl/              公共领域类型、应用契约与端口
  src/
    domain/                     领域模型与纯规则
    application/                用例与依赖倒置端口
    providers/                  注册、平台协议，酷我优先
    downloads/                  任务执行、重试、恢复与发布协调
    audio_processing/           格式、解密与标签处理
    infrastructure/             网络、存储、文件与秘密等实现
    runtime/                    执行器、统一上游入场、取消与停机
    api/httplib/                控制 HTTP 入口
    media_server/httplib/       文件与媒体入口
    event_server/httplib/       SSE 入口
  apps/server/                   服务入口与依赖组装
  resources/config/             无秘密的配置资源
  tests/                         单元、集成、契约与样本
  packaging/                     Windows、Linux 与 Docker
```

## 模块边界

application 依赖 domain；providers、downloads、audio_processing、infrastructure、runtime 和三个 HTTP 入口依赖 application 的端口与领域类型。具体实现由 apps/server 组装，业务层不反向依赖 HTTP、数据库或某个平台。

当前模块以 CMake INTERFACE 目标表达依赖关系，别名统一为 music_dl::domain、music_dl::application 等。这些是构建骨架，不是已实现的头文件库；后续增加真实源码时将相应目标改为普通库，并按 PUBLIC／PRIVATE 明确依赖。不通过空函数或空 main 伪造可运行产物。

公共头只放跨模块接口，私有头跟随实现；逐文件声明源码，不使用 GLOB 自动扫描。平台独立注册，新增平台不修改公共路由、任务状态机或增加平台专表。

application/tasks 归一下载意图，domain 定义指纹规则，downloads 执行已保存的意图；稳定 AccountBinding 与带版本的 AuthIdentityKey 分开。application/accounts 协调刷新，平台包注册授权协议，infrastructure/auth 只放服务认证等通用适配。downloads/Scheduler 领取持久任务，runtime/admission 统一管理上游租约；各自只维护自己的状态。详细契约以[内部设计](../docs/design/cpp20-refactor/backend-service/internal-contracts.md)为准，当前不增加空接口或实现代码。

## 配置与检查

需要 CMake 3.30+、Ninja 和支持 C++20 的编译器。以下命令均在 cpp 目录执行：

```powershell
# Windows：先进入 x64 Native Tools / Developer PowerShell 环境，确保 cl 与 ninja 可用。
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
```

```sh
# Linux：需要 g++ 与 ninja。
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug
```

对应 Release 预设为 windows-msvc-release 和 linux-gcc-release。预设按系统、编译器与配置隔离构建目录；Linux 使用当前主机架构，ARM64 等发布组合须单独验证。开发机差异可以通过 CMakeUserPresets.json 覆盖，不能提交绝对工具链路径。

目前构建没有可编译的业务源文件，也不产生 music-dl-server 或安装包。CMake 配置成功只证明工程骨架和本机构建环境可用；没有运行时、跨平台功能或发布验收结论。BUILD_TESTING 可打开测试接线，但尚无真实测试。

## 后续实现入口

先完成酷我单平台闭环；外部工具、多平台和 Spotify 目录按开发计划逐步接入。工程初始化阶段不固定尚未验证的第三方库版本，不增加 GUI 或默认外部运行时。

- [模块与目录设计](../docs/design/cpp20-refactor/backend-service/modules-and-layout.md)
- [构建与依赖规则](../docs/design/cpp20-refactor/backend-service/build-and-dependencies.md)
- [多平台扩展设计](../docs/design/cpp20-refactor/backend-service/provider-extensibility.md)
- [平台开发顺序](../docs/design/cpp20-refactor/platform-roadmap.md)
