# 第三方依赖

版本只在 [Versions.cmake](Versions.cmake) 维护，获取与校验见 [Dependencies.cmake](Dependencies.cmake) 和 [VerifyDependency.cmake](VerifyDependency.cmake)。全部依赖在配置阶段拉取到各构建目录的 `_deps/`，不使用 third_party 研究目录，也不回退到系统中的同名库。

| 依赖 | 版本 | 固定提交／校验 | 用途 | 许可证 |
| --- | --- | --- | --- | --- |
| cpp-httplib | v0.62.0 | `74ef3c42188acd6391f027c4337fb0e1a68c0024` | 控制／媒体／SSE 入口与上游 HTTP 客户端 | MIT |
| Mbed TLS | v3.6.7（LTS） | `068ff080b369adfac81509f9b57b2afabaf82dc5` | cpp-httplib 的 TLS 后端 | Apache-2.0 OR GPL-2.0-or-later |
| nlohmann/json | v3.12.0 | `55f93686c01528224f448c19128836e7df245f72` | 协议边界 JSON | MIT |
| SQLite | 3.54.1 amalgamation | SHA3-256 `92281c86…df28e30` | 持久化 | Public domain |
| TagLib | v2.3.2 | `deadc2990767dfbda0701e0ab35fdeea653db08f`（子模块 utfcpp v4.2.0 `2d8e20b2…`） | MP3／FLAC／MP4 标签读写（audio_processing） | LGPL-2.1 或 MPL-1.1，按 MPL-1.1 静态链接 |
| Catch2 | v3.16.1 | `08092139210881f01a34e60c5f2fd6afa0ea4024` | 测试，仅 BUILD_TESTING=ON 时获取 | BSL-1.0 |

## 规则

- Git 依赖使用 `GIT_SHALLOW`、发布 tag，并在每次配置时校验：实际 HEAD 等于期望提交（tag 解引用后的 commit），工作区没有修改、暂存或未跟踪文件，子模块均已初始化且位于记录的提交、内部无修改。任一不符即配置失败。`FETCHCONTENT_SOURCE_DIR_<NAME>` 离线覆盖与复用的构建目录同样适用。
- SQLite 官方不提供 Git 发布 tag，使用官方 amalgamation 归档：下载时由 FetchContent 校验站点公布的归档 SHA3-256；此外每次配置都校验 `sqlite3.c`、`sqlite3.h` 的固定 SHA3-256（[Versions.cmake](Versions.cmake)），因此源码覆盖不能绕过版本校验。解压使用当前时间而非归档内时间，升级后的源码总比旧目标文件新，Ninja 会重新编译。
- cpp-httplib 不使用其自带 CMakeLists（会搜索系统 OpenSSL、zlib、brotli），由 [adapters/httplib.cmake](../adapters/httplib.cmake) 提供头文件目标并显式启用 Mbed TLS；压缩暂不启用。SQLite 由 [adapters/sqlite.cmake](../adapters/sqlite.cmake) 编译为静态库。
- TagLib 用自带 CMakeLists 构建为静态库，只启用 MP4 与 Vorbis（FLAC／Ogg）格式（MPEG 始终包含），关闭 zlib、绑定、示例与其 CppUnit 测试；utfcpp 只取其固定子模块（禁用 find_package 并预置头文件路径，不回退系统安装）。TagLib 的目标只导出安装树的头文件路径，由 [adapters/taglib.cmake](../adapters/taglib.cmake) 以系统头文件提供源码树路径与生成的 taglib_config.h；MSVC 下以 /utf-8 编译其源码。
- 许可：TagLib 双许可 LGPL-2.1／MPL-1.1，本项目按 MPL-1.1 使用：静态链接不要求公开本项目源码，只有对 TagLib 自身文件的修改须按 MPL 提供；发布包需附带 TagLib 的 COPYING.MPL（T37／T40 打包时落实）。
- 升级时同时修改 tag 与 commit，并在 Windows 与 Linux 重新运行依赖测试。

## 选择 Mbed TLS 的原因

cpp-httplib v0.62.0 支持 OpenSSL、Mbed TLS 和 wolfSSL。Mbed TLS 是纯 CMake 项目，可在 Windows、Linux 和容器中以同一方式从源码构建；OpenSSL 在 Windows 上无法同样通过 FetchContent 构建，需要另装预编译包。该版本的 Mbed TLS 后端在 Windows 读取系统证书库、在 Linux 读取系统 CA 路径，主机名与证书链校验的具体行为由 T09 验证。

## 已记录的体积

源码目录（含 `.git`，Windows 调试构建）：Mbed TLS 约 191 MB（含 framework 子模块），nlohmann/json 约 186 MB，Catch2 约 18 MB，cpp-httplib 约 14 MB，SQLite 约 11 MB，TagLib 约 11 MB（含 utfcpp）。调试版 tag.lib 约 37 MB。产物：调试版依赖测试程序 7.8 MB；调试版 mbedcrypto／mbedtls／mbedx509 静态库约 3.4／1.7／0.5 MB；发布版 SQLite 静态库约 3.1 MB。发布包体积待服务程序存在后在 T37 测量。
