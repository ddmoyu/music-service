# 通用身份适配

服务访问 token 的通用基础设施，不放音乐平台专有 OAuth 端点、响应解析或刷新协调。

- `MbedTlsTokenCodec`：token 为 `mst_` 加 43 个 base64url 字符（Mbed TLS CTR_DRBG 产生的 32 个随机字节，以平台熵源播种）；SQLite 只保存整串 token 的 SHA-256 小写十六进制。
- `FileTokenDelivery`：把明文写入新建的仅所有者可读文件并刷写，目标已存在即失败，从不覆盖，也从不读回；数据库提交失败时尽力删除。

账号选择、授权身份键、凭证提交与租用归 `application/accounts`；平台验证／刷新协议按需放入 providers/<source>/auth 并随 ProviderBundle 注册；秘密读写归 infrastructure/secrets，持久化归 storage。
