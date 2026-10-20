# 测试目录

测试框架为 Catch2 v3，仅在 BUILD_TESTING=ON 时获取；Debug 预设默认开启。测试通过 `catch_discover_tests` 注册，`ctest --preset <name>` 在没有测试时报错，避免空测试误报成功。

- dependencies：固定依赖的版本与基本可用性（JSON、SQLite、本机回环 HTTP、Mbed TLS 客户端构造），不含业务行为。
- unit：领域规则、请求签名向量、响应解析和状态迁移。
- integration：可控上游、数据库、文件、取消与恢复。
- contract：公共 API、平台能力和扩展契约。
- fixtures/providers：按平台与上下文版本组织的去凭证样本。
