# 上游传输

`HttplibTransport` 用 cpp-httplib（Mbed TLS 后端）实现 `HttpTransport`。规则判断在 `application/transport`，这里只做 I/O：

- 每跳独立的 Client 与连接，结束即关闭，不跨 origin 或身份复用；始终校验证书链与主机名。`ca_bundle` 给出时替换系统信任根（读入内存，中文路径可用），否则使用操作系统根证书。
- 租约必须有效且覆盖目标 origin，否则不发请求；同 origin 重定向在同一租约内继续，跨 origin 重定向放在 `HttpResponseHead::redirect` 交回调用方重新入场。
- `request()` 有界读取，超过上限（含 Content-Length 预检）失败而不截断；`receive()` 先交响应头、再逐块交正文，不缓冲整体。
- `HttpRequest::preserve_content_encoding` 默认关闭。调用方显式开启后可取得原始压缩字节，编码与长度响应头保持原义，传输仍限制线上字节数；调用方负责解压后的大小、完整性、取消和期限检查。千千公开 JSON／歌词接口使用该选项，音频流不启用。
- 截止时间同时用于连接、读写与 httplib 的总超时；取消通过 stop_token 触发 `Client::stop()`，正文接收回调也逐块检查。调用只在 I/O 真正结束后返回。
- 已知限制：Windows 上本地 `shutdown()` 不能唤醒阻塞等待，httplib 客户端也没有可中断读，因此等待响应头期间的取消要到上游应答、空闲超时（默认 30 秒）或截止时间才返回，租约保持到那时。正文传输期间的取消逐块生效。
- httplib.h 在本文件生成代码时产生第三方的 C4702（不可达代码）警告，仅在包含处局部屏蔽。
