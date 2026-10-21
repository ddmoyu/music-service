# 发布与部署

Windows、Linux、macOS 和 Docker 共用同一程序 `music-service` 与同一 HTTP 契约。CI（`.github/workflows/ci.yml`）在 main 推送及 PR 上执行构建、测试与运行验证。

| 目录 | 内容 | 验证内容 |
| --- | --- | --- |
| `windows/` | `smoke.ps1` | version、check-config、init、serve 就绪、healthcheck、带 token 的 info、无 token 401、运行中管理命令被拒；强制结束后实例锁释放，重启后 token 仍有效 |
| `linux/` | `smoke.sh` | 同上，另以 SIGTERM 停止：退出码 0，停机顺序为监听 → 数据库 → 实例锁，之后管理命令可执行；输出 glibc 与动态库依赖 |
| macOS（复用 `linux/`） | `smoke.sh` 的 Darwin 分支 | 同一套启动、认证、SIGTERM 与锁释放检查；用 `sw_vers`、`otool -L` 输出系统与依赖。CI 使用 macos-15 Apple Silicon、Homebrew LLVM 20／libc++；首轮 CI 成功前不声明已验证支持，尚无独立 macOS 发行包 |
| `docker/` | `Dockerfile`、`config.json`、`smoke.sh`、`e2e.sh` | 多阶段构建；运行镜像只含程序、Ubuntu 24.04 基础库与 CA 证书，非 root（uid 10001）；数据卷上 init、健康检查、发布端口访问、`docker stop` 正常收尾，删除容器后用同一卷重建仍可用；`e2e.sh` 发布三个端口后运行在线酷我闭环，以 `docker restart` 作为重启步骤 |
| `e2e/` | `kuwo_e2e.py` | 在线酷我闭环（仅标准库）：搜索 → 详情 → 解析 → 持久任务（创建连接关闭）→ SSE／HTTP → 文件元数据 → 票据 Range 与完整读取并与发布文件比对 → 客户端保存中途失败而任务仍 completed → 取消与传输竞争（canceled 或 completed 其一）→ 重启后新实例仍提供任务与文件、幂等重放与完成结果复用。`--binary` 自行 init／serve／重启，`--api`＋`--token`＋`--restart-command` 驱动已运行的服务。需要能访问酷我的网络；CI 中该步骤允许失败，海外运行器可能受地区限制 |

## 证书

上游 HTTPS 一律校验证书链与主机名（Mbed TLS）。信任根：Windows 读取系统证书存储；Linux 读取发行版 CA 文件（如 `/etc/ssl/certs/ca-certificates.crt`）；macOS 经 cpp-httplib 读取系统 Keychain（链接 Security 与 CoreFoundation）；容器镜像安装 `ca-certificates`。Windows、Linux 和 macOS CI 都配置了 `[online]` 用例，用系统信任根访问公开 HTTPS 站点；本地开发默认不联网，设置 `MUSIC_SERVICE_ONLINE_TESTS=1` 才运行。

## 容器

```bash
docker build -f packaging/docker/Dockerfile -t music-service .
docker volume create music-data
docker volume create music-downloads
docker run --rm -v music-data:/data -v music-downloads:/downloads music-service init
docker run -d --name music-service -p 127.0.0.1:17632:17632 \
  -v music-data:/data -v music-downloads:/downloads music-service
```

- 基础镜像按 digest 固定（`ubuntu:24.04`，见 Dockerfile）；构建阶段用 pip 安装固定版本的 CMake 3.31.6 与 Ninja，因为发行版 CMake 低于工程要求的 3.30。
- 容器内控制、媒体与事件分别监听 `0.0.0.0:17632`／`17633`／`17634`；镜像配置的 `public_urls` 为 `http://127.0.0.1:` 加同一端口，适用于把三个端口映射到宿主 loopback 同号端口的用法（`-p 127.0.0.1:17633:17633` 等，只映射控制端口时文件与 SSE 不可达）。对外提供服务时必须改为客户端可达的 HTTPS 地址或反向代理地址，配置校验会拒绝远程明文地址。
- `init` 生成的 `bootstrap-token.txt` 位于数据卷 `secrets/`，取走后删除；之后的身份管理须先停止容器，再用同一卷运行 `principal`／`token` 命令。
- ENTRYPOINT 为 exec 形式，服务即 PID 1，直接收到 `docker stop` 的 SIGTERM；停机远短于缺省 10 秒宽限期，Compose 中可显式设置 `stop_grace_period` 大于 `shutdown.soft_seconds`。
- 尚未验证：arm64 镜像、Compose 示例、NAS。
