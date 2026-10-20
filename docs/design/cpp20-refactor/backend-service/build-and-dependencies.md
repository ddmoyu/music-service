# 构建与依赖

[cpp 工程](../../../../cpp/README.md)最低 CMake 为 3.30，提供 Windows MSVC 与 Linux GCC 的 Debug／Release Ninja 预设及测试预设。T01 已按本页规则接入首批依赖，具体版本与许可证见[依赖清单](../../../../cpp/cmake/dependencies/README.md)；模块目标仍为 INTERFACE，没有业务源码、安装包或服务程序。

## 基线

| 项目 | 规则 |
| --- | --- |
| 语言 | C++20，target 显式要求标准，关闭语言扩展 |
| 产品边界 | 纯后端、无 GUI；不获取 Qt、桌面运行时或前端构建依赖，构建和运行均不要求图形会话 |
| 构建 | CMake + Presets；骨架最低设为 3.30，正式支持的工具链组合由实际源码构建确定 |
| 获取 | 顶层 FetchContent，Git 依赖统一浅克隆 |
| 固定版本 | 发布 tag + 期望 commit + 实际 HEAD 校验 |
| 使用 | 模块只链接 target，依赖宏和头文件按目标传播 |
| 默认网络 | cpp-httplib，不同时获取备用框架 |
| 系统组件 | 编译器、Git、SDK、系统库显式查找；不隐式替换第三方版本 |

## 跨平台构建与交付

系统、架构及支持级别以[配置与部署](configuration-and-deployment.md#部署方式)为准。Windows、Linux 和 Docker 从 S0 开始持续验证，不能等到发布阶段才验证系统兼容性；macOS 等候选目标未验证前不声明正式支持。

- 同一份业务源码分别构建目标程序；Windows 产物不能直接放入 Linux 镜像运行。Docker 的构建阶段生成目标 Linux 程序，运行阶段打包它及其运行依赖。
- Preset 和产物名称区分系统、CPU 架构、编译器、Debug／Release、C++ 运行库、glibc／musl、TLS 与功能组合；最低操作系统、运行库和 CPU 指令集写入发布说明。
- 首轮验证 Windows x64／MSVC 与 Linux x86_64／GCC，按首批架构计划补齐 Linux ARM64；Clang 和其他目标纳入相应矩阵。交叉编译显式指定 toolchain／sysroot，所有依赖面向目标平台构建，不混用宿主库。
- Schannel／CNG 等系统实现通过目标条件选择；Linux 和容器必须有覆盖同等必需能力的 TLS／协议加密实现。跨平台公共头和业务模块不暴露 Windows／POSIX 原生类型。
- 核对静态／动态链接后的实际依赖，打包需要的 DLL／共享库、CA 证书与许可文件；不能仅因业务可执行文件生成成功就认定独立部署可用。
- CI 分开记录编译、原生运行与镜像验证；首批发布组合均需在干净目标环境执行启动、HTTPS、下载、停止和持久数据恢复。模拟构建成功不能代替目标设备运行结果。

容器采用多阶段构建，计划交付 Dockerfile、Compose 示例和 `linux/amd64`／`linux/arm64` 镜像。运行用户、持久卷、监听、健康与停止信号的要求集中维护在[Docker 交付约束](configuration-and-deployment.md#docker-与-compose-交付约束)。

## 版本与获取

`GIT_SHALLOW=TRUE` 使用 depth 1，`GIT_TAG` 须为分支或 tag，不能直接使用 commit。本项目固定发布 tag，校验其解析后的 commit 与实际 HEAD；tag 移动或校验失败时停止构建。[CMake Git 参数](https://cmake.org/cmake/help/v3.30/module/ExternalProject.html)

研究提交只用于源码分析，实施时选择验证通过的发布 tag。每项依赖记录 URL、tag、commit、子模块、选项、许可证和版本绑定补丁。

| 属性 | 规则 |
| --- | --- |
| GIT_REPOSITORY | 固定上游或指定镜像 |
| GIT_TAG | 集中维护的发布 tag |
| GIT_SHALLOW | TRUE |
| GIT_PROGRESS | 可启用 |
| GIT_SUBMODULES | 仅获取需要项 |
| 子依赖 | 顶层提前声明，锁定版本与选项 |

顶层声明与 MakeAvailable 遵守 [FetchContent](https://cmake.org/cmake/help/v3.30/module/FetchContent.html)规则。浅克隆只减少历史，工作树、LFS、子模块和发布体积仍需测量。

## 依赖接入

| 依赖 | 接入要求 |
| --- | --- |
| cpp-httplib | v0.62.0；不使用上游 CMakeLists（会搜索系统 OpenSSL／zlib／brotli），由 adapter 提供头文件目标并只启用 Mbed TLS；压缩暂不启用；Windows 固定 `_WIN32_WINNT=0x0A00` |
| nlohmann/json | v3.12.0；关闭自身测试与安装 |
| SQLite | 3.54.1 官方 amalgamation；无 Git 发布 tag，改用官方归档 + SHA3-256 校验，由 adapter 编译为静态库 |
| TLS | Mbed TLS v3.6.7（LTS），只构建静态库；全部目标系统使用同一实现，Windows 读取系统证书库、Linux 读取系统 CA 路径。协议加密（MD5／AES 等）按平台需要另行接入 |
| 压缩、标签 | 按功能开关获取 |
| 测试框架 | Catch2 v3.16.1，仅 BUILD_TESTING 时获取 |

非标准上游构建入口由 cmake/adapters 处理，不散落到业务目录，不静默回退系统第三方库。首次接入在 Windows 与目标 Linux 工具链验证。

### 平台包与外部工具

各 `music_dl_provider_<source>` 独立声明实际依赖，发行 preset 通过 MUSIC_DL_PROVIDERS 列表选择平台；新平台增加注册项和 target，无需修改公共 source 枚举。由构建清单生成显式工厂引用，未选平台不获取或链接它的专属依赖。S1 最小组合仅含酷我及公共基础设施。

外部 adapter 另设构建／运行开关。yt-dlp 等工具与其运行时、媒体工具按选定发布组合显式安装和锁定版本，不纳入默认基础包；本机调用仍经统一 helper 协议。第三方源码依赖沿用 FetchContent 规则，外部可执行工具则记录来源、版本、校验值及适配协议版本，不把 Python 工具伪装成 C++ 链接库。基础包缺少可选工具时仍能独立启动。设计见[多平台扩展](provider-extensibility.md#4-两条接入路径)。

## CMake 布局

```text
cpp/
  CMakeLists.txt
  CMakePresets.json
  CMakeUserPresets.json             # 本机覆盖，不提交
  cmake/
    ProjectOptions.cmake
    dependencies/
      Versions.cmake               # URL、tag、期望 commit
      Dependencies.cmake           # 声明与 MakeAvailable 顺序
      VerifyDependency.cmake
    adapters/
    toolchains/
```

目标划分见[模块与目录](modules-and-layout.md)。先设置第三方功能／测试／示例选项，再 MakeAvailable；编译器、架构、运行库、TLS 和功能组合纳入 Preset。

上述依赖版本、获取与校验文件在首次接入依赖时创建；当前保留说明目录，不用空声明假装依赖已接入。共享预设不包含本机绝对路径，Windows MSVC 预设要求调用环境已初始化编译器与 Ninja；Linux 预设使用目标主机的 GCC，交叉编译工具链另行添加。

## 缓存与验证

- 源码和构建缓存不提交；二进制目录按工具链／架构／配置隔离。
- 共享源码缓存须验证版本；不同工具链不共享已编译依赖。
- 首次联网获取，离线使用预备源码或 source directory 覆盖；FULLY_DISCONNECTED 不会补齐缺失依赖。
- CI key 包含版本、工具链、架构、TLS 和功能选项；镜像不能改变期望版本。
- 补丁独立保存，不直接依赖修改后的本机 FetchContent 缓存。

验证干净获取、浅克隆、版本校验、缓存后离线配置、构建隔离、按需依赖，以及 SQLite／TLS 的目标平台构建。
