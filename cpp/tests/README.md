# 测试目录

- unit：领域规则、请求签名向量、响应解析和状态迁移。
- integration：可控上游、数据库、文件、取消与恢复。
- contract：公共 API、平台能力和扩展契约。
- fixtures/providers：按平台与上下文版本组织的去凭证样本。

当前没有测试源码或测试框架，BUILD_TESTING 默认关闭。配置和空构建成功不代表通过业务测试；未来通过 add_test 注册真实测试，CI 使用 ctest --no-tests=error 避免空测试误报成功。
