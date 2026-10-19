# 配置、部署与服务生命周期

## 产品与运行边界

本项目是纯后端，以无 GUI 的独立进程 `music-dl-server` 交付。构建与启动不依赖 Qt、Electron、Tauri、WebView、图形桌面、显示服务、浏览器或音频输出设备；命令行用于启动、配置校验、初始化和管理。播放器、交互式界面与客户端本机保存由外部客户端负责，部署验证不要求启动任何前端。

跨平台和 Docker 是必需能力。原生运行、系统服务、NAS 与容器复用同一套业务代码和 HTTP 契约；操作系统差异通过适配层处理。文档中的“音乐平台”指网易云等上游来源，“运行平台”指操作系统、CPU 架构与运行环境，二者分别管理。

## 配置来源与生效时机

采用显式 JSON 配置，加载顺序为内置缺省 → 指定配置文件 → 已列明环境变量 → 已列明命令行覆盖项。未知键、类型错误、端口冲突或非法范围在启动阶段报错，不静默忽略。

相对路径相对于配置文件所在目录解析，不相对于进程偶然的工作目录。解析后保存规范化绝对路径；程序安装目录与可写数据目录分离。用户可以移动程序文件，不应因此丢失任务历史。

| 配置组 | 内容与初始值 |
| --- | --- |
| `config_version` | 1；配置版本与 API、数据库版本分别管理 |
| `control` | host=127.0.0.1，port=17632，workers=16，connection_queue=64 |
| `media` | enabled 按构建／阶段启用，host=127.0.0.1，port=17633，workers=8，connection_queue=16 |
| `features` 媒体项 | file_access 随下载闭环启用；media_proxy=false、direct_media_urls=false，在线能力按验证结果启用 |
| `auth` | mode=bearer；principal、token 哈希与 scope 的唯一事实来源为 SQLite |
| `events` | enabled 随下载闭环启用，host=127.0.0.1，port=17634，workers=8，connection_queue=16 |
| `event_limits` | 全局订阅 6／每 principal 2；每订阅最多 128 条、256 KiB；单事件最大 16 KiB |
| `event_stream` | progress_interval_ms=1000，heartbeat_seconds=15，write_idle_seconds=10，max_connection_seconds=1800 |
| `public_urls` | 对外 api_base、media_base 与 events_base；远程部署显式配置，不信任任意 Host／Forwarded 头拼 URL |
| `http` | body_limit=256 KiB，控制读写空闲各 10 秒，keep_alive_timeout=3 秒，单连接请求数上限 50 |
| `admission` | 昂贵控制请求全局 8／每 principal 2；调用者超出 429，整体不足 503 |
| `upstream` | short_workers=8，queue=64，per_source_short=2，max_total_connections=16，max_response=2 MiB |
| `catalog` | 总查询预算 10 秒／上限 15 秒，每来源 5 秒，单请求 fanout=4，最多 8 个来源 |
| `downloads` | workers=3，per_source=2，max_attempts=3，transport_retries=2，文件最大 2 GiB |
| `download_limits` | 每 principal 非终态任务最多 500，全局 2000，批次最多 100 |
| `download_timeouts` | 连接 10 秒、读取空闲 30 秒、一次尝试 10 分钟、首次开始后绝对预算 30 分钟 |
| `processing` | workers=2，queue=16；外部工具如启用，必须另设可取消的时间上限 |
| `media_limits` | 全局响应 6／每 principal 2，其中播放上游最多 4、本地文件最多 4 |
| `media_stream` | bridge_buffer=256 KiB，connect_timeout=5 秒，read/write idle=30 秒，单流最长 30 分钟 |
| `lifetimes` | 解析描述 5 分钟，游标 10 分钟，匹配上下文 5 分钟，媒体访问／票据默认 60 秒；不设播放会话寿命 |
| `storage` | SQLite WAL，单 writer、队列 128、读取连接最多 2、数据库等待预算 500 ms |
| `cache` | catalog 128 项／总估算 16 MiB／60 秒，失败抑制最多 5 秒；其他短期句柄另有总量限制 |
| `shutdown` | 在途工作软收尾预算 15 秒，之后保存检查点并请求停止 |
| `filesystem` | 失败任务工作文件默认最多保留 24 小时；用户取消清理未发布工作文件，活动恢复文件不按终态 TTL 删除 |

普通上游连接默认 3 秒，读取空闲不超过来源预算；所有操作受总截止时间和全局并发约束。

短期内存句柄另设实例上限：context 10000、cursor 5000、asset 1000、match 1000、media_ticket 2000，总估算额外 32 MiB；每 principal 不超过实例上限的可配置份额。到期优先清理，正在传输的请求持有独立资源引用直到结束；无法接收新句柄时返回 SERVICE_BUSY。媒体访问描述不另建句柄表，不保存播放器状态。

## 监听地址与端口冲突

- 独立服务／NAS／Docker 使用配置的固定端口；占用时启动失败并报告监听项与地址，由部署配置和客户端同步修改。所有必需监听成功后才报告 ready。
- 桌面程序管理的子进程可将本机监听端口设为 0，由系统选择空闲端口。绑定完成后，后端通过专用启动通道输出实际 api_base_url、media_base_url、events_base_url 和 instance_id；桌面启动器传给前端。普通日志与启动消息分离，消息不含 token。
- 前端先从配置或启动器取得 API 地址，再读取 info 中的媒体／事件地址，不按端口偏移推算。动态本机模式公布实际 loopback 地址；远程部署仍使用显式 public_urls／反向代理地址。
- 独立网页使用用户配置的服务地址或同源反向代理，不能直接读取本机进程启动通道。端口被其他进程占用时不终止它，也不把它视为本服务。

## 示例配置

未列项使用默认值。

```json
{
  "config_version": 1,
  "data_dir": "./data",
  "control": {"host": "127.0.0.1", "port": 17632, "workers": 16, "connection_queue": 64},
  "media": {"enabled": true, "host": "127.0.0.1", "port": 17633, "workers": 8, "connection_queue": 16},
  "events": {"enabled": true, "host": "127.0.0.1", "port": 17634, "workers": 8, "connection_queue": 16},
  "public_urls": {
    "api_base": "http://127.0.0.1:17632",
    "media_base": "http://127.0.0.1:17633",
    "events_base": "http://127.0.0.1:17634"
  },
  "auth": {"mode": "bearer"},
  "cors": {"allowed_origins": ["http://127.0.0.1:5173"]},
  "destinations": [{"id": "dst_default", "name": "默认下载", "root": "./downloads", "principals": ["prn_local"]}],
  "upstream": {"short_workers": 8, "queue": 64, "per_source_short": 2, "max_total_connections": 16},
  "downloads": {"workers": 3, "per_source": 2, "max_attempts": 3, "transport_retries": 2},
  "features": {"file_access": true, "media_proxy": false, "direct_media_urls": false, "automatic_fallback": false, "transcoding": false},
  "logging": {"format": "json", "level": "info"}
}
```

启用功能必须已包含在构建中，否则启动失败。principal 与 token 由初始化／管理配置建立。

file_access／media_proxy 要求 media.enabled=true。S1 提供文件出口和票据；media_proxy 单独控制在线代理。direct_media_urls 不依赖媒体监听，外发条件见 [API](api-contract.md)。

events.enabled=true 同样要求构建包含 SSE 功能，且 event_limits 全局订阅上限至少比 events.workers 少 2。事件监听复用身份配置和允许的 CORS origins，具有独立长连接超时，下载闭环默认启用 SSE，关闭时由 info 声明降级。

### 环境变量与命令行

首版仅支持确定的覆盖项：MUSIC_DL_CONFIG、MUSIC_DL_DATA_DIR、MUSIC_DL_CONTROL_LISTEN、MUSIC_DL_MEDIA_LISTEN、MUSIC_DL_PUBLIC_API_URL、MUSIC_DL_PUBLIC_MEDIA_URL、MUSIC_DL_DOWNLOAD_DIR。最后一项只覆盖默认 destination 根目录，不允许从普通 HTTP 请求改写。

事件监听补充 MUSIC_DL_EVENTS_LISTEN、MUSIC_DL_PUBLIC_EVENTS_URL；覆盖规则与控制／媒体一致。

命令行提供 config、data-dir、check-config、init、version；端口和线程数走配置。init 不覆盖已有实例；check-config 校验配置、能力和路径。

## 凭证与 TLS

SQLite 统一保存 token 哈希、principal 和 scopes。init 通过受权限保护的 bootstrap-token.txt 一次性交付明文 token，取走后删除；该文件不参与运行时认证。日志只打印文件位置。平台 Cookie 放 SecretStore，DB 保存 secret_ref。

首版使用操作系统文件权限保护秘密。后续系统密钥管理作为 SecretStore 适配，备份须包含对应密钥策略。

入站 TLS 使用 httplib 或反向代理；出站校验证书链与主机名。启用平台却缺少必需 HTTPS 构建时启动失败。

TLS 后端与信任根见[技术栈](../technology-stack.md)，平台协议加密单独选择。

## 运行数据布局

```text
data_dir/
  instance.lock
  service.sqlite3
  service.sqlite3-wal              # 由 SQLite 维护
  service.sqlite3-shm
  secrets/
    bootstrap-token.txt            # 初始化一次性交付；取走后删除，不是运行时事实来源
    credentials/                  # SecretStore 私有文件
  cache/                           # 可丢弃，不存唯一任务事实
  logs/                            # 文件日志启用时使用；容器可仅 stdout

destination_root/
  .music-dl-work/
    tsk_xxx/
      input.part
      processed.part
      checkpoint.json              # 可恢复证据的受控副本
  Artist/
    Album/
      Track.ext
```

工作文件与最终文件位于同一目标文件系统，便于重命名发布；data_dir 可以位于其他磁盘。checkpoint 文件与数据库版本互相核对，不能仅信任某个残留 .part 大小。目标目录离线、权限改变或 NAS 挂载断开要转成文件错误，不回落到意外的系统目录。

命名规则只在服务端配置，模板字段白名单展开，移除路径穿越与非法字符，校验 symlink／reparse point 解析后的实际目标仍位于授权根内。重名通过预留和后缀处理，不覆盖既有资产。

## 启动与退出

启动顺序：加载并校验配置 → 获取实例锁 → 打开 DB 并迁移 → 初始化身份与 SecretStore → 验证目标目录 → 组装 Provider 与传输 → 恢复 finalization 与任务检查点 → 启动工作池和监听 → readiness 成功 → 领取队列。

迁移、必需目录或监听失败时，不以“部分初始化成功”继续对外接受任务。可选平台的临时网络故障不阻止服务启动，平台可用性通过查询单独报告。

退出按运行时文档执行。控制入口先拒绝新工作，下载保存恢复状态；媒体请求关闭并释放上游额度；数据库最后关闭。没有外部管理器时进程仍可手动常驻运行，开机自启和异常重启属于部署集成。

SSE 退出时停止接受订阅，唤醒等待并关闭现有流，注销订阅后销毁分发器。管理状态补充订阅数、待发字节、合并次数、溢出断开与写超时计数，避免只观测下载工作池。

## 部署方式

### 系统、架构与支持级别

以下是设计目标，当前尚未创建 C++ 工程，均不表示已经构建或运行通过。Windows、Linux 与 Docker 为必需目标；首批架构按下表建议推进，版本下限和正式发布组合由实际验证确定。

| 运行目标 | CPU／镜像平台 | 计划与交付 |
| --- | --- | --- |
| Windows 原生 | x64 | 建议首批；独立可执行程序及实际运行依赖，验证前台运行与 Windows 服务集成 |
| Linux 原生 | x86_64、ARM64 | 建议首批；分架构二进制，标明发行版、最低内核／glibc 与 C++ 运行库要求 |
| Linux 容器 | `linux/amd64`、`linux/arm64` | 建议首批；Dockerfile、Compose 示例与对应架构镜像，正式发布时提供多架构清单 |
| NAS | 与设备系统、架构及容器能力匹配 | 优先复用 Linux 容器；原生包按具体型号和系统验证，专有套件另行决定 |
| macOS 原生 | ARM64、x86_64 | 后续候选；保持设计兼容，原生发布、最低系统版本与 launchd 集成待明确 |
| 其他组合 | Windows ARM64、Linux ARMv7、musl／Alpine、FreeBSD、Android 等 | 暂不承诺正式支持；存在明确需求后补工具链和运行验证 |

同一份源码按目标平台分别构建。Linux Docker 镜像内运行 Linux 程序；Docker 构建不会把 Windows `.exe` 转换为 Linux 程序。操作系统、CPU 架构和运行库兼容性分别验证，不能由“支持 Docker”推导为支持所有硬件。[Docker 多平台构建](https://docs.docker.com/build/building/multi-platform/)

### 跨平台实现约束

| 方面 | 设计要求与验证内容 |
| --- | --- |
| 工具链与运行库 | 分别验证 MSVC、GCC 及需要的 Clang 组合；固定 C++20 特性、最低系统与 CPU 指令集，不用构建机 `native` 指令集作为通用发布默认值；glibc 与 musl 单独构建验证 |
| 系统接口 | Windows／POSIX API 限于进程、文件、权限、网络、秘密等适配实现；业务层不直接依赖 Win32、注册表、系统服务管理器或某个 shell |
| TLS 与协议加密 | 目标系统均须具备所需算法和 HTTPS 身份校验；Schannel／CNG 仅可用于 Windows 适配，Linux／容器须有对应实现；验证系统信任根、CA 文件与自定义信任配置 |
| 路径与文件名 | API 文本用 UTF-8，文件系统边界显式转换；覆盖中文、emoji、长路径、保留名、非法字符、大小写与 Unicode 差异，不假定目录分隔符或盘符格式 |
| 文件语义与权限 | 验证 Windows ACL、Unix UID／GID、文件锁、占用文件、重命名与原子发布；下载工作文件与最终文件同一文件系统，运行账户拥有必要权限 |
| 服务生命周期 | 前台运行是共同入口；Linux／容器处理 SIGTERM／SIGINT，Windows 服务停止与控制台退出接入同一收尾流程；无需交互输入，失败使用非零退出码 |
| 网络与代理 | 显式配置监听、公开地址和出站代理；验证 IPv4、启用时的 IPv6、DNS、TLS 和代理行为，不依赖交互用户的浏览器或系统代理设置 |
| 时间与资源 | 持久时间与日志时间使用明确时区，建议 UTC；超时和预算使用单调时钟；线程、连接与缓存按部署配置限额，在低内存设备及容器 CPU／内存限制下验证 |
| 可选外部工具 | 如启用 FFmpeg，提供匹配操作系统与架构的工具或配置其路径；安全传参、可取消退出，不把 bash、PowerShell 或特定桌面软件作为共同运行依赖 |
| 交付与升级 | 发布包列明系统、架构、运行库、功能和依赖；在无开发工具的干净环境验证启动、HTTPS、下载、恢复和退出；升级前备份，数据库迁移后不承诺任意降级 |

`data_dir` 中的 SQLite WAL 数据库须位于受支持的本地文件系统，不放 SMB／NFS 共享目录；部署在 NAS 上也不等于数据库可放网络共享。下载目录可单独指向经验证的挂载目录，并验证断开与重命名行为。[SQLite WAL 限制](https://sqlite.org/wal.html)

### 运行与集成方式

| 环境 | 设计要求 |
| --- | --- |
| Windows 本机 | 独立可执行进程，可由系统服务管理或显式启动；数据在安装目录之外；服务账号可访问目标目录 |
| Linux 系统服务 | 前台进程由 systemd 等外部管理器监管；运行用户、配置路径、重启策略和停止预算明确，不在程序内部自行后台化 |
| 桌面程序打包 | 前端连接本机已有实例，或明确管理打包进程；若要求关窗口后继续，不能随前端一起杀后端 |
| Linux 主机／NAS | 对应架构二进制、受管理的长期进程、明确用户与目录权限；验证 x86_64／ARM64 而非只宣称支持 |
| Docker | 前台无界面进程、容器内绑定适当地址、映射启用的三个端口或经代理；data_dir 和下载目录使用持久卷 |
| 同域反向代理 | `/api/v1` 与健康路径转控制，`/media/v1` 转媒体，`/events/v1` 转事件；SSE 关闭代理缓冲与压缩，代理空闲超时大于心跳间隔（初始 60 秒）；媒体配置适合长流的超时 |
| 远程多个客户端 | public URLs 使用客户端可达地址；每调用方有 token，按需要选择共享或独立 principal |

容器须绑定可访问接口；public URL 使用客户端可达的宿主／代理地址。

首版单实例，SQLite 目录禁止多进程并写；容器数据使用持久卷。

### Docker 与 Compose 交付约束

- Dockerfile 采用构建阶段与运行阶段分离；运行镜像仅包含程序、运行依赖、CA 证书和必要资源，固定基础镜像版本并记录 digest。glibc 路线先验证，musl／Alpine 不因镜像体积直接视为兼容。[多阶段构建](https://docs.docker.com/build/building/multi-stage/)
- 默认非 root 运行，明确 UID／GID 与持久卷所有权；不依赖 privileged 或 host 网络。配置可只读挂载，`data_dir` 与下载目录可写并持久化，临时目录和日志去向明确。
- 容器内通过 exec 形式的 ENTRYPOINT 直接运行前台服务；若有启动脚本，最终 exec 服务。必须实测收到停止信号后执行收尾，Compose 停止宽限期大于配置的停机预算。[ENTRYPOINT 与信号](https://docs.docker.com/reference/dockerfile/#entrypoint)
- Compose 示例显式配置启用入口的容器监听、宿主映射、public URLs、持久卷和运行用户；本机原生默认 loopback，容器按需监听 `0.0.0.0`，不把容器内部 `127.0.0.1` 公布为远程访问地址。
- 日志默认输出 stdout／stderr，健康探测使用现有本地健康接口，不依赖 GUI 或上游音乐平台在线；最小镜像未包含 shell／curl 时，须提供可执行的探测方式。
- 分别验证 amd64／arm64 镜像启动、HTTPS、下载与停止；交叉编译或模拟构建成功只证明构建完成。删除并重建容器后任务与文件仍可恢复，不把可写镜像层作为持久存储。

## 观测与维护

日志最少带时间、级别、request_id／task_id、principal 的非秘密标识、source、操作、耗时、错误码。区分入场等待、平台网络、处理、持久化时间，避免只看到一个总延迟。

管理状态提供控制／短请求／下载／媒体／DB 队列占用、超时／重试／取消计数、任务终态数量与恢复异常。健康接口不联网测所有音乐平台，避免探测本身触发账号或平台压力。

备份 SQLite 使用一致性备份方式，并协调需要保留的资产和秘密；不能在 WAL 活动时只随意复制主 DB 文件。临时文件清理先核对活动任务与发布意图，不能因为看到旧扩展名就删除仍被恢复流程使用的文件。

## 发布前需验证

按上述支持矩阵逐项记录构建、启动、HTTPS、下载、停止与恢复结果；覆盖实际 TLS 组合、目标系统／架构／运行库、NAS 文件系统、端口和 CORS、媒体长流、中文与长路径、权限、容器非 root 与持久卷、停止信号与停机上限、数据库迁移与备份、文件重名和链接路径、依赖分发总大小。支持级别分为设计目标、构建通过、运行验证通过和正式发布，不用单一系统测试代替其他组合。数字默认值在验证后修改本文件与运行时文档，不能只在代码里形成第二套配置事实。
