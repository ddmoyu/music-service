# music-service

跨平台的纯后端音乐服务，通过 HTTP API 提供音乐搜索、音源解析、下载与任务管理。本仓库只包含后端：GUI、网页、桌面等前端在各自独立的仓库中开发，通过版本化 API 接入。服务支持原生进程、系统服务和 Docker 部署，系统与架构范围见[配置与部署](docs/design/cpp20-refactor/backend-service/configuration-and-deployment.md)。

命名约定：项目与仓库名为 `music-service`；CMake 项目与库目标前缀为 `music_service`，C++ 命名空间为 `music_service`，服务可执行程序为 `music-service`，环境变量前缀为 `MUSIC_SERVICE_`，`/api/v1/info` 的 service 字段为 `music-service`。

当前已接通酷我闭环，并接入 5sing 作为第二个平台（T22）：搜索、详情与解析，持久下载与重启恢复，SSE 与 HTTP 进度，已完成文件与媒体票据；媒体与事件监听默认启用。S1 关口（T20）的验收状态以[开发任务](docs/design/cpp20-refactor/development-tasks.md)中的最新实施记录为准。

## 目录结构

```text
music-service/
  CMakeLists.txt               # 顶层 C++20 工程
  CMakePresets.json            # Windows MSVC／Linux GCC／macOS LLVM 构建与测试预设
  cmake/                       # 编译选项、依赖获取与校验、第三方适配、工具链
  src/                         # 全部源码，头文件与实现放在同一模块目录
    domain/                    # 值对象与纯规则，无 I/O
    application/               # 用例、出站端口（ports）与配置类型（config）
    providers/                 # 平台适配，酷我优先
    downloads/                 # 持久任务调度、执行、重试、恢复与发布
    audio_processing/          # 格式验证、解密与标签
    infrastructure/            # 配置、日志、传输、存储、文件与秘密等实现
    runtime/                   # 执行器、统一上游入场、取消与停机
    api/                       # 控制 HTTP 入口
    media_server/              # 文件与媒体入口
    event_server/              # SSE 入口
    server/                    # music-service 可执行程序：main 与依赖组装
  tests/                       # 依赖、单元、集成与契约测试及去凭证样本
  config/                      # 配置键名说明与无秘密的示例配置
  packaging/                   # Windows、Linux、macOS 与 Docker 运行验证
  docs/                        # 研究记录与设计文档
  third_party/                 # 研究用外部源码，不参与构建
  .github/workflows/           # CI：Windows、Linux、macOS 与 Docker
```

- 头文件与实现放在同一模块目录，所有模块都以 `src/` 为包含根，写作 `#include "domain/result.hpp"`。本项目只产出服务程序、不发布 C++ 库，因此不设单独的 `include/` 目录。
- `src/server` 是后端自身的进程入口（参数解析、生命周期、依赖组装），不是前端。
- 第三方依赖在配置阶段按固定版本拉取到构建目录，见[依赖说明](cmake/dependencies/README.md)。`third_party/` 只放研究用的参考源码，目录说明见[第三方源码说明](third_party/README.md)。

模块划分、依赖方向与目标结构见[模块与目录设计](docs/design/cpp20-refactor/backend-service/modules-and-layout.md)，各模块职责见 [src 说明](src/README.md)。

## 构建与测试

需要 CMake 3.30+、Ninja、Git、支持 C++20 的编译器，以及首次配置时的网络访问。命令在仓库根目录执行：

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

```sh
# macOS：使用 LLVM 20 及其 libc++，以支持 C++20 jthread / stop_token。
brew install cmake ninja llvm@20
export MUSIC_SERVICE_LLVM_ROOT="$(brew --prefix llvm@20)"
cmake --preset macos-clang-debug
cmake --build --preset macos-clang-debug --parallel 3
ctest --preset macos-clang-debug
bash packaging/linux/smoke.sh out/build/macos-clang-debug/src/server/music-service
```

- Debug 预设默认开启测试；Release 预设（windows-msvc-release、linux-gcc-release、macos-clang-release）不构建测试。构建目录为 `out/build/<预设名>`，按系统、编译器与配置隔离。Linux／macOS 使用当前主机架构；macOS CI 首先覆盖 `macos-15` 的 Apple Silicon，Intel 和其他发布组合须单独验证。
- macOS 预设使用 Homebrew LLVM 的编译器、C++ 头文件和运行库，并记录运行库搜索路径。当前用于构建与 CI 验证，不能将依赖本机 Homebrew 路径的产物直接当作独立发行包。
- 开发机差异放在已忽略的 CMakeUserPresets.json，不提交绝对工具链路径。
- Windows 上使用官方 CMake 或 Visual Studio 自带的 CMake。MSYS2／MinGW 发行的 cmake 内置 curl 不带 CA 证书，下载依赖会失败；MinGW 不是支持的工具链。
- 测试通过只说明相应检查覆盖的部分正确，不代替端到端和发布验收；服务程序位于 `out/build/<预设名>/src/server/`。

## 文档入口

- [项目文档](docs/README.md)
- [C++20 重构设计](docs/design/cpp20-refactor/README.md)
- [开发任务与实施记录](docs/design/cpp20-refactor/development-tasks.md)
- [配置键名](config/README.md)
