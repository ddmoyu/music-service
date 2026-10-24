# music-service

项目尚未发布正式版，不维护历史版本兼容性。0.1.26 仅接受当前配置、API、数据库 schema 4 和任务快照 schema 5；旧开发数据不自动迁移，需使用新的 `data_dir`，已有数据不会自动删除。

0.1.25 补齐通用 OAuth 登录会话、浏览器回调、PKCE、后台授权码交换与自动刷新，支持离线导入应用秘密、刷新轮换保护和重启恢复。当前 9 个内置平台尚无 OAuth 工厂；Jamendo 保持匿名获取歌曲和下载，不增加登录要求。接口及真实平台接入边界见[OAuth 流程](src/providers/README.md#oauth-登录回调与自动刷新0125)。

0.1.23 增加管理员 JSON／Prometheus 指标、资源占用状态及有界异步文件日志轮转，配置和口径见[运维观测](docs/operations-observability.md)。跨平台匹配、手动与自动换源已取消；解析与下载只使用请求指定的平台。

跨平台的纯后端音乐服务，通过 HTTP API 提供音乐搜索、音源解析、下载与任务管理。本仓库只包含后端：GUI、网页、桌面等前端在各自独立的仓库中开发，通过版本化 API 接入。服务支持原生进程、系统服务和 Docker 部署，系统与架构范围见[配置与部署](docs/design/cpp20-refactor/backend-service/configuration-and-deployment.md)。

命名约定：项目与仓库名为 `music-service`；CMake 项目与库目标前缀为 `music_service`，C++ 命名空间为 `music_service`，服务可执行程序为 `music-service`，环境变量前缀为 `MUSIC_SERVICE_`，`/api/v1/info` 的 service 字段为 `music-service`。

开发遵循 [项目开发约定](AGENTS.md)：合理控制程序体积，不引入 ytdl、Chrome 等外部独立工具；新增依赖库必须先经用户评审同意。后端同时重视性能、接口易用性、服务响应与稳定性，长耗时任务不得拖住服务，异常与资源使用必须有明确边界。

当前已接入酷我、5sing、网易云、QQ 音乐、酷狗、千千、Jamendo、咪咕、Bilibili 共 9 个原生平台：搜索、详情与解析，持久下载与重启恢复，SSE 与 HTTP 进度，已完成文件与媒体票据；媒体与事件监听默认启用。各平台能力以 `/api/v1/providers` 为准，验收状态见[开发任务](docs/design/cpp20-refactor/development-tasks.md)。

网易云已支持 standard／high／lossless（取决于账号及歌曲权限）、公共歌单／专辑搜索、分类、公共推荐与榜单分页；网易云、酷我、5sing 的集合链接可经公共 API 进入曲目列表。[独立验收客户端](packaging/e2e/collections.md)支持选择或全部分批提交，每批最多 100 项；已受理任务持久执行，未提交页不会在客户端关闭后继续展开。网易云已支持 Hi-Res 请求与严格降级判定，实际可用档位受歌曲与账号权限限制；酷我已支持 320 kbps MP3、FLAC 和歌词。最新验收与适用范围见[补齐计划](docs/design/cpp20-refactor/reference-gap-plan.md)。

网易云扩展还包括：使用默认 Cookie 读取个人自建／收藏歌单、歌手搜索、歌手专辑分页、热门歌曲最多 50 首、YRC 逐字与独立罗马音歌词、播客节目分页及下载。歌词写入可选择 `lrc`／`verbatim`／`romanized`，缺失变体明确回退；节目主播独立展示。扫码登录不在当前范围。接口与边界见[网易云适配器](src/providers/netease/README.md)。

0.1.6 补充原生 NCM 本地导入，以及下载时可选导出独立歌词和封面。使用 `music-service --config config.json import-ncm --input "歌曲.ncm" --principal prn_local --destination dst_default`（服务停止时）；侧文件由 `downloads.output.export_lyrics`／`export_cover` 开启，默认关闭。完整用法、文件访问与恢复规则见[配置说明](config/README.md#ncm-本地导入与侧文件)。

0.1.7 新增 QQ 音乐（`source=qq`）：公共推荐、分类与分类歌单、排行榜、单曲／歌单／专辑／歌手搜索、详情和分页曲目、网页链接、歌词、封面及原生下载。搜索需要有效 QQ 音乐 Cookie；标准／高品质／无损音源取决于歌曲和账号权限，Hi-Res 暂不支持。配置、接口、限制及 Windows 验收方法见 [QQ 适配器](src/providers/qq/README.md)。各平台后续状态见路线图，其他操作系统的专项验收排在最后。

酷狗（`source=kugou`，协议包 0.1.17）已接入 21 项匿名能力：推荐、分类、榜单、四类搜索、歌手资料与歌曲／专辑目录、集合分页、链接、LRC、封面及原生音源。歌手目录实测完整遍历 963 个歌曲条目和 47 张专辑；标准整曲下载、标签／侧文件、复用和重启回读通过 Windows 验收。高品质／无损成功下载待样本，Cookie 和 Hi-Res 尚未接入。用法与边界见[酷狗适配器](src/providers/kugou/README.md)。

酷我 0.1.9 增加黑胶（`hires`）取流、本项目 C++ 完整 FLAC 解码校验及附加尾部清理；24 bit／96 kHz 真实下载、封面与标签、复用和重启回读已验证，用法见 [酷我适配器](src/providers/kuwo/README.md)。

0.1.8 补齐酷我歌手搜索、歌手资料、歌曲／专辑分页和可信专辑封面；歌曲与歌手详情直接读取官方 JSON 接口，不解析 HTML。封面可嵌入 MP3／FLAC，也可独立导出；用法与验收见 [酷我适配器](src/providers/kuwo/README.md)。

千千音乐（`source=qianqian`，协议包 0.1.11）新增匿名单曲／专辑搜索、歌单／专辑详情与分页曲目、分类与热门歌单、链接、歌词／封面及 standard／high／lossless 音源。歌单搜索、榜单、歌手目录、账号登录和 Hi-Res 未接入；VIP 无地址明确报告权限限制。用法与验收见[千千适配器](src/providers/qianqian/README.md)。

Jamendo（`source=jamendo`）支持匿名歌曲／专辑／歌手搜索、官方精选歌单搜索与推荐、原生歌曲分类、官方歌曲榜、详情与集合分页、歌词、封面和标准 MP3 下载。歌单搜索范围为官方精选目录；分类返回歌曲。下载严格检查平台许可，匿名高品质／无损暂未确认可用。用法和验收见 [Jamendo 适配器](src/providers/jamendo/README.md)。

咪咕音乐（`source=migu`）接入匿名四类搜索、官网实时推荐／分类／榜单、详情与集合分页、链接、歌词、封面和原生音源。数字专辑与普通专辑分别映射；试听默认拒绝，严格品质不静默降级。官方 WebP 封面支持原图嵌入与独立导出。能力与验收边界见[咪咕适配器](src/providers/migu/README.md)。

Bilibili（`source=bilibili`）已完成首阶段：匿名视频搜索、BV／分 P 链接、详情、独立 AAC／M4A 标准品质音轨和可选 Cookie 校验。下载前先将分 P 解析为固定 cid；UP 主不作为演唱者。真实下载、标题标签、完整解码、取消、复用与重启回读已通过 Windows 验收；用法及尚未接入的能力见 [Bilibili 适配器](src/providers/bilibili/README.md)。

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

旧构建缓存若选择了已移除的平台，配置会明确报错；可在配置命令中加 `-U MUSIC_SERVICE_PROVIDERS` 恢复当前默认清单，自定义组合则删除失效项。运行配置中的 `providers`、`catalog.default_sources` 和候选来源列表也须移除失效项。

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

- 发行包：`cmake --build --preset windows-msvc-release --target package`（Linux 用 linux-gcc-release）生成 zip／tar.gz，含程序、配置示例、许可文件与 Windows 服务／systemd 安装脚本，说明见 [packaging](packaging/README.md)。MSVC 目标使用静态运行库。
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
