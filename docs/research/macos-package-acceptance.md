# macOS 独立包验收

本次补齐 macOS 独立运行库、平台专用安装布局、launchd 用户服务、签名检查和可下载 CI 产物。验证日期为 2026-10-24，使用 GitHub 的 macOS 15 原生 ARM64／Intel 运行器，实际系统版本均为 15.7.9。两个架构的最终打包与包运行检查均已通过。

## 源码与证据

- 业务源码基线：`49619fa719e53c1682c1ae45ec3c12279cd7abbe`，版本 **0.1.26**。
- 打包修复提交：`9ee88117b973d9f7fd5441496e21fb3372bbc20e`，验证分支 `codex/macos-standalone-package`。
- [打包专项运行 40094310300](https://github.com/ddmoyu/music-service/actions/runs/40094310300)：`target=macos-package`；ARM64、Intel 均成功，分别保存了通过检查的压缩包和 SHA-256。
- [完整 macOS 运行 40093408621](https://github.com/ddmoyu/music-service/actions/runs/40093408621)：早期打包提交 `1869ea10d6d91cf25577d88661beef0437731537` 的两个 Debug 构建成功；各发现 **610 项 CTest，604 通过、6 项在线／样本门控跳过、0 失败**。系统 Keychain HTTPS（各 1 用例／7 断言）与前台运行冒烟也通过。ARM64 的旧打包脚本未识别 LLVM 的裸 `@loader_path` 搜索路径而失败；已完成 Debug 验证的 Intel 旧构建在修复版重跑后取消。该运行整体不是成功状态，只采用上述已完成步骤的结果，最终包成功的证据来自专项运行。

这些提交只包含本次 macOS 打包改动，未混入工作区中并行开发的 0.1.27 账号／会员功能。当前工作区后续整理提交时，仍需对最终提交运行所需 CI；本记录不替代其验收，也未创建 GitHub Release 或镜像。

## 补齐内容与检查范围

| 内容 | 实现与验证 |
| --- | --- |
| 独立 C++ 运行库 | 收集已使用的 LLVM 20 `libc++`、`libc++abi`、`libunwind`，补齐许可材料；没有新增业务依赖。非系统依赖不在允许范围内、离开所选 LLVM 安装目录、重名冲突或解析失败时明确报错 |
| 可搬移安装包 | 将动态库引用改为包内 `@loader_path`，移除构建 RPATH；保留系统库和 Security／CoreFoundation 框架。macOS 包不再携带 Linux systemd 脚本 |
| 独立包实际运行 | CI 临时移开 Homebrew LLVM 的真实目录，将包解压到中文及空格路径，以系统 PATH 运行；验证版本、配置、初始化、健康检查、token 认证、未认证拒绝及运行中管理命令被拒 |
| 正常停止 | 前台 SIGTERM 后按监听、数据库、实例锁顺序收尾；退出后离线身份管理可用 |
| launchd | 当前登录用户的 LaunchAgent 安装、启动、停止、重启、卸载；停止后日志有锁释放记录，重启后原 token 可用，卸载保留数据库和下载目录；拒绝覆盖已有 agent |
| 签名与元数据 | 改写和剥离后 ad-hoc 签名，对每个 Mach-O 做严格签名校验；记录架构、实际最低系统版本、文件大小与 SHA-256 |
| 打包边界测试 | 9 项标准库 unittest，覆盖含空格路径、loader／executable 相对路径、裸相对 RPATH、缺失运行库、越界运行库、非允许依赖和 Mach-O 元数据解析 |
| 下载后校验 | 下载 Actions ZIP 并核对 GitHub artifact digest；核对内部 tar.gz 的 SHA256SUMS、程序和三份运行库的 manifest 哈希、执行权限及许可证原文 |

`macos-package` 专项跳过 Debug C++ 测试与在线音乐闭环，只在打包改动未修改业务代码时用于复验。普通 CI 的完整矩阵仍保留原检查；在线音乐步骤允许失败，不能从作业成功推断各音乐平台都可用。

## 本次产物

| 架构 | tar.gz 字节数 | 主程序字节数 | 三份运行库字节数合计 | 包内普通文件字节数合计 | 最低 macOS |
| --- | ---: | ---: | ---: | ---: | --- |
| ARM64 | 4,311,837 | 12,545,216 | 1,637,616 | 14,312,063 | 15.0 |
| Intel x86_64 | 4,638,357 | 13,461,024 | 1,549,184 | 15,139,439 | 15.0 |

上述大小来自实际上传和下载的 0.1.26 产物；包内文件总量不包含文件系统分配单元开销，也不包含构建缓存或调试产物。

- [ARM64 Actions 产物](https://github.com/ddmoyu/music-service/actions/runs/40094310300/artifacts/12560038848)，内含 `music-service-0.1.26-macos-arm64.tar.gz` 与 `SHA256SUMS`。
- ARM64 tar.gz SHA-256：`bc02db9ff23e87fc6ffeb24cb9033884d3c92f0d256e24b905123114eaaed5ed`。
- [Intel Actions 产物](https://github.com/ddmoyu/music-service/actions/runs/40094310300/artifacts/12559994806)，内含 `music-service-0.1.26-macos-x86_64.tar.gz` 与 `SHA256SUMS`。
- Intel tar.gz SHA-256：`a9fdc2c09606589fbd5bd1367374892315a5c8b01ca32b618e36f7cc082f74d4`。

Actions 产物保留 14 天，本次到期日为 2026-11-07；这不是长期 Release 下载地址。下载后先在压缩包所在目录运行 `shasum -a 256 -c SHA256SUMS`，再解压并按 [macOS 使用说明](../../packaging/macos/README.md)初始化。

## 尚未验证的分发边界

当前预览包为 **ad-hoc 签名，未公证**。已提供指定 Developer ID Application 身份的签名入口，但真实证书签名、Apple 公证与带下载隔离属性的 Gatekeeper 放行仍需要发布者的证书／账号，未执行；不能从 `codesign --verify` 推断公证成功。未承诺 macOS 15 之前的系统或 Rosetta 运行结果，launchd 验证针对已登录用户会话，不是系统级 LaunchDaemon。
