# 通用身份适配

本目录预留服务 Bearer token 的校验等通用基础设施实现，不放音乐平台专有 OAuth 端点、响应解析或刷新协调。

账号选择、授权身份键、刷新合并与条件提交归 application/accounts；平台验证／刷新协议按需放入 providers/<source>/auth 并随 ProviderBundle 注册；秘密读写归 infrastructure/secrets，持久化归 storage。当前没有实现代码。
