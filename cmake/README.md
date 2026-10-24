# 构建支持

ProjectOptions.cmake 维护公共 C++20 要求（MSVC 另加 /utf-8、/permissive- 等）与项目代码的警告选项；警告只作用于项目目标，不作用于第三方源码。MUSIC_SERVICE_WARNINGS_AS_ERRORS 可把警告升级为错误。

- dependencies：集中声明、获取并校验固定版本的第三方源码，见[依赖说明](dependencies/README.md)。
- adapters：为第三方源码提供受控目标，包括 cpp-httplib（启用 Mbed TLS）、SQLite 与 TagLib。
- toolchains：`macos-llvm.cmake` 使用 Homebrew LLVM 20／libc++，在编译器检测前显式选择 macOS SDK；可通过 `CMAKE_OSX_SYSROOT` 或 `SDKROOT` 选择其他 SDK。

不写入开发机绝对路径；本机差异放在已忽略的 CMakeUserPresets.json。尚无实现的安装包不导出。

## 构建与测试

需要 CMake 3.30+、Ninja、Git、支持 C++20 的编译器及首次获取依赖时的网络访问。命令均在仓库根目录执行。Debug 默认包含测试，Release 默认只构建服务及其依赖；构建目录为 `out/build/<预设名>`。

Windows 使用 MSVC，先初始化 Visual Studio 的 x64 开发环境：

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug --parallel 3
ctest --preset windows-msvc-debug --timeout 120
```

Linux 使用本机架构的 GCC：

```bash
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug --parallel 3
ctest --preset linux-gcc-debug --timeout 120
bash packaging/linux/smoke.sh out/build/linux-gcc-debug/src/server/music-service
```

macOS 使用本机架构的 Homebrew LLVM 20 及其配套 libc++：

```bash
brew install cmake ninja llvm@20
export MUSIC_SERVICE_LLVM_ROOT="$(brew --prefix llvm@20)"
cmake --preset macos-clang-debug
cmake --build --preset macos-clang-debug --parallel 3
ctest --preset macos-clang-debug --timeout 120
bash packaging/linux/smoke.sh out/build/macos-clang-debug/src/server/music-service
```

以上并发值可按机器内存调整。Release 使用对应的 `*-release` 预设；Windows、Linux 和 macOS 均可加 `--target package` 生成发行包。

macOS 默认以 15.0 为部署目标，打包时用 Python 3.9+ 标准库和 Apple 开发工具收集当前 LLVM 20 运行库、改写为包内相对路径、剥离调试符号并签名；成品不要求 Homebrew／LLVM。默认 ad-hoc 签名，可通过 `MUSIC_SERVICE_MACOS_SIGN_IDENTITY` 指定已有 Developer ID Application 身份；签名不等于公证。使用方法与实际最低系统版本说明见 [macOS 发行包](../packaging/macos/README.md)。部署检查与 CI 架构范围见[发布与部署](../packaging/README.md)。
