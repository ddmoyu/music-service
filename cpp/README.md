# C++ 后端工程

已完成 T01：固定版本的第三方依赖已接入（cpp-httplib + Mbed TLS、nlohmann/json、SQLite，测试用 Catch2），配置阶段联网获取并校验版本，见[依赖说明](cmake/dependencies/README.md)。尚无业务源码、平台实现或服务入口；现有测试只验证依赖本身。

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

需要 CMake 3.30+、Ninja、Git、支持 C++20 的编译器，以及首次配置时的网络访问。以下命令均在 cpp 目录执行：

```powershell
# Windows：先进入 x64 Native Tools / Developer PowerShell 环境，确保 cl 与 ninja 可用。
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug
```

```sh
# Linux：需要 gcc、g++ 与 ninja。
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug
ctest --preset linux-gcc-debug
```

Debug 预设默认开启 BUILD_TESTING，Release 预设（windows-msvc-release、linux-gcc-release）不构建测试。预设按系统、编译器与配置隔离构建目录；Linux 使用当前主机架构，ARM64 等发布组合须单独验证。开发机差异可以通过 CMakeUserPresets.json 覆盖，不能提交绝对工具链路径。

Windows 上应使用官方 CMake 或 Visual Studio 自带的 CMake。MSYS2／MinGW 发行的 cmake 内置 curl 不带 CA 证书，下载 SQLite 归档时会证书校验失败。MinGW 不是支持的工具链。

目前不产生 music-dl-server 或安装包。依赖测试通过只证明依赖版本与工具链可用，不代表业务、跨平台运行或发布验收。CI 见仓库根目录 `.github/workflows/ci.yml`，在 Windows MSVC 与 Linux GCC 上执行同样的预设。

## 后续实现入口

下一项为 T02（领域规则与最小应用端口）。先完成酷我单平台闭环；外部工具、多平台和 Spotify 目录按开发计划逐步接入，不增加 GUI 或默认外部运行时。

- [模块与目录设计](../docs/design/cpp20-refactor/backend-service/modules-and-layout.md)
- [构建与依赖规则](../docs/design/cpp20-refactor/backend-service/build-and-dependencies.md)
- [多平台扩展设计](../docs/design/cpp20-refactor/backend-service/provider-extensibility.md)
- [平台开发顺序](../docs/design/cpp20-refactor/platform-roadmap.md)
