# 测试用 TLS 材料

仅供本地 HTTPS 上游集成测试使用，不得用于任何部署。有效期至 2036 年。

- `ca.pem`：测试根证书（"music-service test CA"）。签发后私钥已删除，不能再签新证书。
- `server.pem`／`server.key`：由测试根签发，SAN 仅 `DNS:localhost`；用 `https://127.0.0.1` 访问即主机名不匹配。
- `rogue.pem`／`rogue.key`：自签名的 localhost 证书，不受测试根信任，用于验证错误证书链被拒绝。

用 OpenSSL 3.5 生成（EC P-256，有效期 3650 天）：测试根为带 `basicConstraints=critical,CA:TRUE` 的自签名证书；`server.pem` 由测试根签发，`extendedKeyUsage=serverAuth`、`subjectAltName=DNS:localhost`；`rogue.pem` 为自签名、同样只含 `DNS:localhost`。
