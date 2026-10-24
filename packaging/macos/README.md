# macOS 独立发行包

提供 `music-service-<版本>-macos-arm64.tar.gz`（Apple Silicon）和 `music-service-<版本>-macos-x86_64.tar.gz`（Intel）。包内携带构建时已经使用的 LLVM 20 C++ 运行库，运行时不需要安装 Homebrew、LLVM、Python 或编译器。请完整保留 `bin/`、`lib/` 与 `share/`，不要只复制可执行文件。

当前构建目标为 macOS 15.0，两种架构由 macOS 15 原生 CI 验证。包内 `share/music-service/macos-package.json` 记录程序和运行库的实际最低系统版本、架构、大小与 SHA-256；若其最低版本高于 15.0，以实际记录为准。未据此承诺旧版 macOS 或 Rosetta 运行结果。

## 前台运行

解压后，在包的根目录执行。配置和数据放在用户的 Application Support 目录，下载目录单独创建；示例中的相对路径都相对于配置文件。

```bash
state="$HOME/Library/Application Support/music-service"
mkdir -p "$state/downloads"
# 首次配置；已经存在时保留原文件，不覆盖。
test -e "$state/config.json" || cp share/music-service/config.macos.example.json "$state/config.json"
./bin/music-service --config "$state/config.json" check-config
./bin/music-service --config "$state/config.json" init
./bin/music-service --config "$state/config.json" serve
```

逐条确认成功后继续；`init` 只用于新实例，重启原实例时跳过。Ctrl+C 正常停止服务。默认控制／媒体／事件端口为 17632／17633／17634，仅监听本机。配置日志已开启，文件在 `data/logs/music-service.log`，沿用服务的有界轮转设置。

另开终端读取首次生成的 token 并请求 API：

```bash
state="$HOME/Library/Application Support/music-service"
token="$(cat "$state/data/secrets/bootstrap-token.txt")"
curl --fail --silent --show-error -H "Authorization: Bearer $token" http://127.0.0.1:17632/api/v1/info
```

将 token 保存到客户端或私密位置后删除首次交付文件；删除文件不会撤销 token。程序是 HTTP 后端，没有内置网页界面。配置、API 与版本切换说明在源码仓库中。

## 登录后由 launchd 运行

先将完整包放在准备长期使用的位置，完成上述初始化，停止前台服务，然后从包根目录执行：

```bash
bash share/music-service/service/launch-agent.sh install "$HOME/Library/Application Support/music-service/config.json"
```

以当前已登录用户运行，不使用 `sudo`。安装器写入 `~/Library/LaunchAgents/io.github.ddmoyu.music-service.plist` 并启动服务；使用 GUI 登录会话，不是开机时的系统守护进程。注销时停止，登录时启动；异常退出由 launchd 按至少 10 秒间隔重新拉起。`logging.file_enabled` 必须为 true，启动失败时查看配置中 `data_dir/logs/music-service.log`，或停用 agent 后前台运行以查看早期配置错误。

```bash
bash share/music-service/service/launch-agent.sh stop
bash share/music-service/service/launch-agent.sh start
bash share/music-service/service/launch-agent.sh uninstall
```

`stop` 从当前会话卸载任务并等待退出；配置文件仍在，下次登录会再次启动。`uninstall` 停止任务并移除 plist，保留程序、配置、数据和下载文件。更换程序目录、配置路径或更新包时，先卸载旧 agent，再从新位置安装；不要修改正在运行的程序或运行库。离线身份管理也须先停止 agent。

## 签名状态

默认包在改写运行库路径和剥离调试符号后执行 ad-hoc 签名，并检查程序及每个运行库的签名完整性。**ad-hoc 签名不等于 Apple Developer ID 签名或公证**；通过网页下载的未公证预览包可能受 Gatekeeper 限制。不要通过关闭系统安全检查来把预览包当作已公证版本。

发布者已有 Developer ID Application 证书及私钥时，可在构建机钥匙串中配置证书，并使用：

```bash
cmake --preset macos-clang-release -DMUSIC_SERVICE_MACOS_SIGN_IDENTITY="Developer ID Application: Your Name (TEAMID)"
cmake --build --preset macos-clang-release --target package
```

这会使用指定身份、Hardened Runtime 和时间戳签名。公证仍需要发布者的 Apple 账号授权及单独的分发归档提交；不能把本地签名通过当成公证成功。流程见 [Apple 公证说明](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution)。当前 CI 使用 ad-hoc 签名，不声明 Gatekeeper 分发验收通过。

## 从源码打包与验证

以下命令在源码仓库根目录执行；构建需要已有的 CMake、Python 3.9+、Ninja、Git、Apple 开发工具和 Homebrew LLVM 20，成品用户不需要这些工具。

```bash
export MUSIC_SERVICE_LLVM_ROOT="$(brew --prefix llvm@20)"
cmake --preset macos-clang-release
cmake --build --preset macos-clang-release --target package
bash packaging/macos/package_smoke.sh out/build/macos-clang-release/music-service-*.tar.gz
```

打包仅收集程序实际引用的现有 LLVM `libc++`／`libc++abi`／`libunwind`，保留系统动态库与框架引用。发现其他非系统库、运行库冲突或缺失时明确失败，不自动引入依赖。运行库附带 LLVM 20.1.8 对应组件的完整许可证。

包验证覆盖中文和空格目录、包结构、相对动态库路径、签名、前台启动与认证、SIGTERM、锁释放，以及 launchd 安装、停止、重启和卸载。脚本使用当前用户的真实 launchd 会话；如果已经安装同名 agent，会拒绝测试。CI 还会临时移开 Homebrew LLVM 的真实安装目录，验证结束后恢复，再保存压缩包和 SHA-256 到 Actions 产物；这是构建产物，尚不是公开 Release。
