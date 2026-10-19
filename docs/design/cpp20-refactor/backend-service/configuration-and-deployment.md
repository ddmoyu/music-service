# 配置、部署与服务生命周期

本文件描述拟实现的运行约定。没有创建配置文件、Windows 服务、容器镜像或启动脚本；以下示例仅用于设计评审。

## 配置来源与生效时机

采用显式 JSON 配置，加载顺序为内置缺省 → 指定配置文件 → 已列明环境变量 → 已列明命令行覆盖项。未知键、类型错误、端口冲突或非法范围在启动阶段报错，不静默忽略。

相对路径相对于配置文件所在目录解析，不相对于进程偶然的工作目录。解析后保存规范化绝对路径；程序安装目录与可写数据目录分离。用户可以移动程序文件，不应因此丢失任务历史。

| 配置组 | 内容与初始值 |
| --- | --- |
| `config_version` | 1；配置版本与 API、数据库版本分别管理 |
| `control` | host=127.0.0.1，port=17632，workers=16，connection_queue=64 |
| `media` | enabled 按构建／阶段启用，host=127.0.0.1，port=17633，workers=8，connection_queue=16 |
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
| `lifetimes` | 解析描述 5 分钟，游标 10 分钟，匹配上下文 5 分钟，播放会话 30 分钟，票据默认 60 秒 |
| `storage` | SQLite WAL，单 writer、队列 128、读取连接最多 2、数据库等待预算 500 ms |
| `cache` | catalog 128 项／总估算 16 MiB／60 秒，失败抑制最多 5 秒；其他短期句柄另有总量限制 |
| `shutdown` | 在途工作软收尾预算 15 秒，之后保存检查点并请求停止 |
| `filesystem` | 失败任务工作文件默认最多保留 24 小时；用户取消清理未发布工作文件，活动恢复文件不按终态 TTL 删除 |

普通上游连接默认 3 秒，读取空闲不超过来源预算；每一次具体操作仍受总截止时间约束。provider 的较长超时只能在服务上限内调整，不能突破全局并发或媒体隔离。

短期内存句柄另设实例上限：context 10000、cursor 5000、asset 1000、match 1000、playback_session 200、media_ticket 2000，总估算额外 32 MiB；每 principal 不超过实例上限的可配置份额。到期优先清理，活跃会话不能被随意淘汰；无法接收新句柄时返回 SERVICE_BUSY。

## 示例配置片段

下面仅展示主要启动项，未列项使用上表缺省；例子不包含可用凭证。

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
  "auth": {"mode": "bearer", "token_store": "./data/secrets/access-tokens.json"},
  "cors": {"allowed_origins": ["http://127.0.0.1:5173"]},
  "destinations": [{"id": "dst_default", "name": "默认下载", "root": "./downloads", "principals": ["prn_local"]}],
  "upstream": {"short_workers": 8, "queue": 64, "per_source_short": 2, "max_total_connections": 16},
  "downloads": {"workers": 3, "per_source": 2, "max_attempts": 3, "transport_retries": 2},
  "features": {"automatic_fallback": false, "transcoding": false},
  "logging": {"format": "json", "level": "info"}
}
```

media.enabled=true 需要该阶段已实现／构建的媒体功能，否则启动校验失败；示例不是让尚不存在的工程立刻运行。principal 与访问 token 通过初始化／管理配置建立，不能由前端自己声明一个 owner_id。

events.enabled=true 同样要求构建包含 SSE 功能，且 event_limits 全局订阅上限至少比 events.workers 少 2。事件监听复用身份配置和允许的 CORS origins，具有独立长连接超时，不能直接继承控制请求总时限。首次目标版本的下载闭环默认启用 SSE；兼容部署可关闭并由 info 声明降级。

### 环境变量与命令行

首版仅支持确定的覆盖项：MUSIC_DL_CONFIG、MUSIC_DL_DATA_DIR、MUSIC_DL_CONTROL_LISTEN、MUSIC_DL_MEDIA_LISTEN、MUSIC_DL_PUBLIC_API_URL、MUSIC_DL_PUBLIC_MEDIA_URL、MUSIC_DL_DOWNLOAD_DIR。最后一项只覆盖默认 destination 根目录，不允许从普通 HTTP 请求改写。

事件监听补充 MUSIC_DL_EVENTS_LISTEN、MUSIC_DL_PUBLIC_EVENTS_URL；覆盖规则与控制／媒体一致。

命令行拟提供 config、data-dir、check-config、init、version 等选项；端口和 worker 数主要走配置，避免两个并存但解释不同的参数系统。init 只生成初始身份／token 存储与目录，不覆盖已有实例；check-config 只检查配置、能力与路径，不创建下载任务。

## 凭证与 TLS

访问 token 返回给用户后，仅存可验证哈希与身份信息；初始化时写入受权限保护的引导文件，日志只打印文件位置，不打印 token。平台 Cookie 放在独立 SecretStore，数据库只记录 secret_ref。

首版至少以操作系统文件权限保护数据与秘密，不声称已实现跨平台静态加密。Windows 凭证保护或 Linux 密钥管理若后续启用，应作为 SecretStore 实现替换，不改变 Provider 或 API；备份与恢复也要包含相应密钥策略。

入站 TLS 可由经验证的 httplib TLS 构建或反向代理终止；出站 HTTPS 由传输适配器校验证书链与主机名。不能因为本地 API 是 HTTP，就认为访问平台不需要 TLS。启用上游平台却没有可用 HTTPS 构建时，启动失败，不偷偷改用明文接口。

具体 TLS 后端、信任根与可分发依赖继续按[技术栈研究](../technology-stack.md)选择；这里不强制采用尚未验证的 Schannel 接入，也不把平台协议 AES／RSA 与 HTTPS 后端当成同一件事。

## 运行数据布局

```text
data_dir/
  instance.lock
  service.sqlite3
  service.sqlite3-wal              # 由 SQLite 维护
  service.sqlite3-shm
  secrets/
    access-tokens.json             # 哈希与 principal 描述
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

| 环境 | 设计要求 |
| --- | --- |
| Windows 本机 | 独立可执行进程，可由系统服务管理或显式启动；数据在安装目录之外；服务账号可访问目标目录 |
| 桌面程序打包 | 前端连接本机已有实例，或明确管理打包进程；若要求关窗口后继续，不能随前端一起杀后端 |
| Linux 主机／NAS | 对应架构二进制、受管理的长期进程、明确用户与目录权限；验证 x86_64／ARM64 而非只宣称支持 |
| Docker | 前台无界面进程、容器内绑定适当地址、映射启用的三个端口或经代理；data_dir 和下载目录使用持久卷 |
| 同域反向代理 | `/api/v1` 与健康路径转控制，`/media/v1` 转媒体，`/events/v1` 转事件；SSE 关闭代理缓冲与压缩，代理空闲超时大于心跳间隔（初始 60 秒）；媒体配置适合长流的超时 |
| 远程多个客户端 | public URLs 使用客户端可达地址；每调用方有 token，按需要选择共享或独立 principal |

容器内若仍只绑定 127.0.0.1，单纯映射端口不等于外部可访问；部署配置需显式改为容器接口地址。API 返回宿主／代理可达的 public URL，不能把容器内部 localhost 返回给远程前端。

多个副本不能直接共享同一个 SQLite 目录同时写；首版为单实例。容器重建后恢复数据依赖持久卷，进程重启恢复能力不能替代磁盘数据保存。

## 观测与维护

日志最少带时间、级别、request_id／task_id、principal 的非秘密标识、source、操作、耗时、错误码。区分入场等待、平台网络、处理、持久化时间，避免只看到一个总延迟。

管理状态提供控制／短请求／下载／媒体／DB 队列占用、超时／重试／取消计数、任务终态数量与恢复异常。健康接口不联网测所有音乐平台，避免探测本身触发账号或平台压力。

备份 SQLite 使用一致性备份方式，并协调需要保留的资产和秘密；不能在 WAL 活动时只随意复制主 DB 文件。临时文件清理先核对活动任务与发布意图，不能因为看到旧扩展名就删除仍被恢复流程使用的文件。

## 发布前需验证

实际 TLS 组合、Windows 构建、Linux／目标 NAS 架构、端口和 CORS、媒体长流、目录权限、重启与停机上限、数据库迁移与备份、文件重名和链接路径、依赖分发总大小。数字默认值在验证后修改本文件与运行时文档，不能只在代码里形成第二套配置事实。
