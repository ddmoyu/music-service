# 第三方依赖

版本只在 [Versions.cmake](Versions.cmake) 维护，获取与校验见 [Dependencies.cmake](Dependencies.cmake) 和 [VerifyDependency.cmake](VerifyDependency.cmake)。全部依赖在配置阶段拉取到各构建目录的 `_deps/`，不使用 third_party 研究目录，也不回退到系统中的同名库。

| 依赖 | 版本 | 固定提交／校验 | 用途 | 许可证 |
| --- | --- | --- | --- | --- |
| cpp-httplib | v0.62.0 | `74ef3c42188acd6391f027c4337fb0e1a68c0024` | 控制／媒体／SSE 入口与上游 HTTP 客户端 | MIT |
| Mbed TLS | v3.6.7（LTS） | `068ff080b369adfac81509f9b57b2afabaf82dc5` | cpp-httplib 的 TLS 后端 | Apache-2.0 OR GPL-2.0-or-later |
| nlohmann/json | v3.12.0 | `55f93686c01528224f448c19128836e7df245f72` | 协议边界 JSON | MIT |
| SQLite | 3.54.1 amalgamation | SHA3-256 `92281c86…df28e30` | 持久化 | Public domain |
| Catch2 | v3.16.1 | `08092139210881f01a34e60c5f2fd6afa0ea4024` | 测试，仅 BUILD_TESTING=ON 时获取 | BSL-1.0 |

## 规则

- Git 依赖使用 `GIT_SHALLOW`、发布 tag，并在配置后比对实际 HEAD 与期望提交（tag 解引用后的 commit）；不一致即配置失败。`FETCHCONTENT_SOURCE_DIR_<NAME>` 离线覆盖同样要求是位于期望提交的 Git 检出。
- SQLite 官方不提供 Git 发布 tag，使用官方 amalgamation 归档与站点公布的 SHA3-256，由 FetchContent 校验哈希。
- cpp-httplib 不使用其自带 CMakeLists（会搜索系统 OpenSSL、zlib、brotli），由 [adapters/httplib.cmake](../adapters/httplib.cmake) 提供头文件目标并显式启用 Mbed TLS；压缩暂不启用。SQLite 由 [adapters/sqlite.cmake](../adapters/sqlite.cmake) 编译为静态库。
- 升级时同时修改 tag 与 commit，并在 Windows 与 Linux 重新运行依赖测试。

## 选择 Mbed TLS 的原因

cpp-httplib v0.62.0 支持 OpenSSL、Mbed TLS 和 wolfSSL。Mbed TLS 是纯 CMake 项目，可在 Windows、Linux 和容器中以同一方式从源码构建；OpenSSL 在 Windows 上无法同样通过 FetchContent 构建，需要另装预编译包。该版本的 Mbed TLS 后端在 Windows 读取系统证书库、在 Linux 读取系统 CA 路径，主机名与证书链校验的具体行为由 T09 验证。

## 已记录的体积

源码目录（含 `.git`，Windows 调试构建）：Mbed TLS 约 191 MB（含 framework 子模块），nlohmann/json 约 186 MB，Catch2 约 18 MB，cpp-httplib 约 14 MB，SQLite 约 11 MB。产物：调试版依赖测试程序 7.8 MB；调试版 mbedcrypto／mbedtls／mbedx509 静态库约 3.4／1.7／0.5 MB；发布版 SQLite 静态库约 3.1 MB。发布包体积待服务程序存在后在 T37 测量。
