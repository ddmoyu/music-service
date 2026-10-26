# Docker 使用教程

自动发布流程见[工作流说明](../release-workflow.md)。当某版本的预览 Release 已发布后，可用
ghcr.io/ddmoyu/music-service:版本号 替换本文的 music-service:local；同一标签自动选择 amd64 或 arm64。
镜像内 /usr/local/share/music-service 包含许可、配置说明和前端接入材料，接入步骤见[前端指南](../../docs/frontend-integration.md)。

本教程从源码构建镜像，再启动 HTTP 后端服务。当前尚未发布镜像仓库中的版本，不能直接用 `docker pull ghcr.io/ddmoyu/music-service` 获取成品；后续交付安排见[发布说明](../RELEASING.md)。

CI 已在 Linux amd64／arm64 原生环境验证镜像构建、启动、认证、正常停止和同卷重建。Compose 示例、真实 NAS 与网络文件系统尚未运行验收；详细证据见[跨系统验收](../../docs/research/cross-platform-ci.md)。

## 1. 构建镜像

需要可运行 Linux 容器的 Docker 环境和项目源码，构建时需要联网获取基础镜像、构建工具与固定版本的源码依赖。Windows／macOS 上使用 Linux 容器环境。编译器在构建阶段使用，运行镜像不携带编译工具。

在仓库根目录执行。下面 Docker 命令均写成单行，可用于 Bash 或 PowerShell；涉及 HTTP 请求时分别给出两种写法。

```text
docker version
docker build --build-arg BUILD_JOBS=3 -f packaging/docker/Dockerfile -t music-service:local .
docker run --rm music-service:local version
```

默认构建宿主机对应的 Linux 架构。内存较少时将 `BUILD_JOBS` 改为 `2` 或 `1`；构建时的内存占用不等于服务运行内存。运行镜像基于固定 digest 的 Ubuntu 24.04，包含程序、系统运行库和 CA 证书。

## 2. 首次初始化

使用两个命名卷：`music-service-data` 保存数据库、身份和秘密等状态，`music-service-downloads` 保存下载文件及工作目录。它们独立于容器生命周期，删除容器不会自动删除命名卷，参见 [Docker 数据卷说明](https://docs.docker.com/engine/storage/volumes/)。

```text
docker volume create music-service-data
docker volume create music-service-downloads
docker run --rm -v music-service-data:/data -v music-service-downloads:/downloads music-service:local check-config
docker run --rm -v music-service-data:/data -v music-service-downloads:/downloads music-service:local init
```

每条命令成功后再继续。`init` 用于首次创建身份，并把 token 写到 `/data/secrets/bootstrap-token.txt`；终端只显示文件位置。重启或重建同一个实例时直接使用原卷，不重复初始化。服务、初始化与后续管理命令必须挂载相同的两个卷；同一个数据卷只运行一个服务实例。

## 3. 启动并检查服务

```text
docker run -d --name music-service --restart unless-stopped --stop-timeout 60 --log-driver json-file --log-opt max-size=10m --log-opt max-file=3 -p 127.0.0.1:17632:17632 -p 127.0.0.1:17633:17633 -p 127.0.0.1:17634:17634 -v music-service-data:/data -v music-service-downloads:/downloads music-service:local
docker inspect --format '{{.State.Health.Status}}' music-service
docker logs --tail 100 music-service
```

健康状态开始为 `starting`，正常启动后变为 `healthy`；若为 `unhealthy` 或容器反复退出，先查看日志。镜像每 15 秒执行一次健康检查，检查就绪状态，不代表所有音乐平台都能访问。

| 宿主机地址 | 用途 |
| --- | --- |
| `http://127.0.0.1:17632` | 控制 API、健康检查、任务查询 |
| `http://127.0.0.1:17633` | 文件和媒体访问 |
| `http://127.0.0.1:17634` | SSE 任务事件 |

三个入口在容器内监听 `0.0.0.0`，这里仅发布到宿主机 loopback。只映射控制端口时，搜索或任务查询可能正常，但返回的媒体地址与 SSE 地址不可达。这是 API 服务，没有内置网页界面。

## 4. 读取 token 并访问 API

等健康状态为 `healthy` 后，从容器读取首次生成的 token。Bash：

```bash
TOKEN="$(docker exec music-service cat /data/secrets/bootstrap-token.txt)"
curl --fail --silent --show-error http://127.0.0.1:17632/health/ready
curl --fail --silent --show-error -H "Authorization: Bearer $TOKEN" http://127.0.0.1:17632/api/v1/info
```

PowerShell：

```powershell
$musicToken = (docker exec music-service cat /data/secrets/bootstrap-token.txt).Trim()
Invoke-RestMethod http://127.0.0.1:17632/health/ready
Invoke-RestMethod http://127.0.0.1:17632/api/v1/info -Headers @{ Authorization = "Bearer $musicToken" }
```

`/health/ready` 不要求认证；普通 API 需要 `Authorization: Bearer <token>`，未提供时返回 401。服务 token 与音乐平台 Cookie 是不同的凭证。接口与后续搜索、下载调用见 [API 契约](../../docs/design/cpp20-refactor/backend-service/api-contract.md)。

将 token 保存到客户端或其他私密位置，确认可以使用后删除首次交付文件：

```text
docker exec music-service rm /data/secrets/bootstrap-token.txt
```

删除这个文件不会撤销 token。需要新 token 时，先停服务，再按[服务命令说明](../../src/server/README.md)执行 `token issue`；不要用重新初始化替代 token 管理。

## 5. 日常管理与持久化

```text
docker logs --tail 100 -f music-service
docker stop --timeout 60 music-service
docker start music-service
```

`docker logs -f` 的 Ctrl+C 只退出日志跟随。服务通过 SIGTERM 收尾；应用的 `shutdown.soft_seconds` 默认 15 秒，本例预留 60 秒容器宽限期。若增加应用停机预算，应同时调整容器的 `--stop-timeout`。Docker 超时后会强制结束进程，不能以一次空闲停止很快推断所有负载都能在默认 10 秒内停止，见 [Docker 停止行为](https://docs.docker.com/reference/cli/docker/container/stop/)。

管理身份时先停止服务，使用相同卷执行命令，然后重新启动：

```text
docker stop --timeout 60 music-service
docker run --rm -v music-service-data:/data -v music-service-downloads:/downloads music-service:local token list --principal prn_local
docker start music-service
```

重建容器时，先停止并执行 `docker rm music-service`，再执行第 3 节的启动命令。保留相同卷即可保留当前格式的任务和身份；不要删除数据卷，也不要把 `docker volume prune` 当成更新步骤。

下载文件位于 `/downloads`。客户端可通过文件 API 读取；也可以用 `docker cp` 按 API 返回的相对路径复制指定成品到宿主机，`.files` 产物按整个目录复制。

若希望宿主机直接访问下载目录，可将每条命令里的 `-v music-service-downloads:/downloads` 都替换为 `--mount "type=bind,source=<已有宿主机绝对目录>,target=/downloads"`。目录必须预先存在，且容器内 UID **10001** 可写；数据卷仍保留为单独的 `/data`。使用 Linux 权限或 NAS ACL 给该 UID 授权，不通过改成 root 运行绕过目录问题。切换挂载前先停止服务并保留原下载文件，不能让已有任务指向一个空目录。

备份时停止服务，配套保存配置、整个数据卷和需要保留的下载卷，复制完成后再启动。程序的在线 `backup` 命令只复制数据库，不包含 `secrets/` 或音频。版本格式发生变化时不会自动迁移，按[版本切换说明](../RELEASING.md#数据与版本切换)使用新卷，保留旧数据。

## 6. 自定义配置与局域网访问

以 [config.json](config.json) 为模板，把自定义配置保存到源码树之外。启动、`init`、`check-config` 和离线管理时都增加同一挂载参数，放在镜像名称之前；替换占位路径为真实的绝对路径：

```text
--mount "type=bind,source=<配置文件绝对路径>,target=/etc/music-service/config.json,readonly"
```

新增或改变挂载、端口映射时，先停止并删除旧容器，再按新参数重建，保持原数据卷。仅修改已经挂载的配置时，先停止服务，再用相同配置和卷运行 `check-config`，成功后启动。程序不会把配置变化自动应用到已运行进程。

远程客户端的 `127.0.0.1` 指向客户端自身。局域网部署需要同时：

1. 将三个端口发布到宿主机的局域网地址，例如 `-p 192.168.1.10:17632:17632`，另外两个端口同理。
2. 将配置中的三个 `public_urls` 改为客户端实际可达的地址；若改变宿主端口，URL 也必须使用新端口。

已有 HTTPS 反向代理时，配置其真实 HTTPS 入口，并将控制、媒体、事件分别转发到对应端口，保留 API 路径、认证头和 Range 请求；SSE 需要持续转发，不能等响应结束才输出。

如果仅在可信局域网中明确使用 HTTP，将模板中的对应字段修改为以下内容，IP 替换为实际宿主地址，其余字段保留：

```json
{
  "public_urls": {
    "api_base": "http://192.168.1.10:17632",
    "media_base": "http://192.168.1.10:17633",
    "events_base": "http://192.168.1.10:17634"
  },
  "security": {"allow_insecure_remote": true}
}
```

这个开关只允许远程明文地址，不启用加密；公网部署使用 HTTPS。独立浏览器前端跨域访问时还需在 `cors.allowed_origins` 填入该前端的确切 origin，详见[配置说明](../../config/README.md)。

## 7. Compose 示例

下面是与前述默认配置对应的本地构建示例，保存为仓库根目录的 `compose.yaml`。Compose V2 的用法见 [Docker 官方说明](https://docs.docker.com/reference/cli/docker/compose/run/)。该示例尚未经过项目 CI 的 Compose 运行验收。

```yaml
name: music-service
services:
  music-service:
    build:
      context: .
      dockerfile: packaging/docker/Dockerfile
      args:
        BUILD_JOBS: "3"
    image: music-service:local
    pull_policy: never
    restart: unless-stopped
    stop_grace_period: 60s
    ports:
      - "127.0.0.1:17632:17632"
      - "127.0.0.1:17633:17633"
      - "127.0.0.1:17634:17634"
    volumes:
      - data:/data
      - downloads:/downloads
    logging:
      driver: json-file
      options:
        max-size: "10m"
        max-file: "3"
volumes:
  data:
    external: true
    name: music-service-data
  downloads:
    external: true
    name: music-service-downloads
```

命名卷使用第 2 节创建的外部卷。首次部署执行下列步骤；已经用前文初始化过同一卷时，跳过 `init`。从手动容器切换时先停止并删除原 `music-service` 容器，以免端口冲突或同时占用数据卷。

```text
docker compose config
docker compose build
docker compose run --rm music-service check-config
docker compose run --rm music-service init
docker compose up -d
docker compose ps
docker compose logs --tail 100 music-service
```

Compose 生成容器名，后续使用服务名操作：第 4 节读取 token 的命令改为 `docker compose exec -T music-service cat /data/secrets/bootstrap-token.txt`，删除交付文件同样改用 `docker compose exec -T`。停止使用 `docker compose stop`，重建使用 `docker compose up -d --build`；只改应用 JSON 配置并沿用绑定挂载时，校验后执行 `docker compose restart`。以上操作保留外部命名卷。

使用第 6 节的自定义配置时，在服务的 `volumes` 列表中再加入 `"<配置文件绝对路径>:/etc/music-service/config.json:ro"`，并按客户端实际访问方式调整 `ports`。Compose 的初始化、检查和运行都会复用该挂载。

## 常见问题

| 现象 | 检查项 |
| --- | --- |
| `pull access denied` | 当前须先构建 `music-service:local`；尚无官方已发布镜像 |
| `INSTANCE_LOCKED` | 服务或另一条管理命令正在使用同一数据卷，先停止占用者 |
| 401 | 使用服务 token，检查 Bearer 头；音乐平台 Cookie 不能替代服务 token |
| 搜索可用，媒体或事件不可达 | 三个端口是否都映射，`public_urls` 是否与客户端实际访问地址一致 |
| `/downloads` 不可写 | 挂载目录已存在且 UID 10001 有写权限；不要把下载目录放进 `/data` |
| 远程 HTTP 配置被拒绝 | 使用 HTTPS；可信局域网明文访问需显式设置 `security.allow_insecure_remote` |
| 数据库／快照格式不匹配 | 当前开发阶段不做历史迁移，保留旧卷并用新卷初始化 |
| `UPSTREAM_TIMEOUT`、`UPSTREAM_FAILED` | 查看具体平台错误与网络条件；容器健康不等于上游音源可用 |
