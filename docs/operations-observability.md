# 运维观测（0.1.23）

实现于 2026-10-24。复用已有 JSON、HTTP、SQLite 与线程队列，不引入监控客户端库、日志库或外部工具。

## 管理接口

- `GET /api/v1/admin/metrics`：标准 JSON envelope，data.operations 为操作计量；携带 instance_id、started_at、uptime_seconds。
- `GET /api/v1/admin/metrics?format=prometheus`：Prometheus 0.0.4 文本，Content-Type 为 `text/plain; version=0.0.4; charset=utf-8`。
- `GET /api/v1/admin/status`：进程状态、资源占用、线程池及日志状态。

均使用 Bearer token 并要求 admin scope；未认证 401，无权限 403。format 只接受 json／prometheus，未知参数返回 400。响应禁止缓存。监控读取不访问音乐平台，也不经过目录查询的昂贵操作入场额度。

PowerShell 查询示例（令牌从既有安全配置读取，不写入命令历史）：

```powershell
$headers = @{ Authorization = "Bearer $env:MUSIC_SERVICE_MONITOR_TOKEN" }
Invoke-RestMethod 'http://127.0.0.1:17632/api/v1/admin/metrics' -Headers $headers
Invoke-WebRequest 'http://127.0.0.1:17632/api/v1/admin/metrics?format=prometheus' -Headers $headers
```

## 计量口径

计数从当前进程启动累计，重启清零。维度只有固定操作和四种结果；不以 URL、关键词、账户、任务 ID 或其他用户输入作为标签。内部空间固定，不随请求数量增长。

| operation | 一次计量的范围 |
| --- | --- |
| control_request | 控制 HTTP 请求结束，包括健康、失败认证和监控查询自身 |
| upstream_call | 一次进入传输层的 request／receive 调用；包括收取响应体；同 origin 内部重定向不另计，不含入场前等待 |
| download_execution | Runner 每次领取后执行至退出；同任务多次退避／重领可能重复计量 |
| audio_processing | 一次处理器 process 调用，包含解密／校验／标签等该处理器实际执行内容 |
| storage_read / storage_write | 一次数据库读／写事务，含连接／写锁等待；启动恢复的事务也计入 |
| media_request | 媒体 HTTP 请求完成；长流关闭后计量 |
| event_request | 事件 HTTP 请求完成；SSE 连接关闭后计量 |

outcome 固定为 success、failure、canceled、deferred。HTTP 按状态码归类：2xx／3xx 成功、429 延后、其余失败；不能用 HTTP 成功推断整个批次、每个搜索来源或断开的流都成功。上游／处理返回 canceled 错误计取消；下载完成／失败／取消分别计量，入场等待、重试等待或交由恢复的退出计 deferred。异常退出仍记失败（下载按最终 RunReport 分类）。

JSON 每项包含 count、outcomes 和 duration_seconds（sum、max、buckets）。直方图边界为 1、5、10、25、50、100、250、500 ms，1、5、30、300 s 和 +Inf；输出为累计桶，JSON 中 +Inf 的 le_seconds 为 null。Prometheus 指标：

- `music_service_operations_total{operation,outcome}` counter。
- `music_service_operation_duration_seconds{operation}` histogram（_bucket、_sum、_count）。
- `music_service_operation_duration_max_seconds{operation}` gauge。

快照在一个短锁内复制固定数组；格式化在锁外完成。控制请求的当前查询尚未结束时不会计入自身快照。各资源快照分别读取，不能视作跨模块的原子事务。

## 资源与异常信号

Prometheus 导出 ready、uptime_seconds、upstream_connections、upstream_cooldowns、downloads_running／paused、media_responses、event_subscriptions、log_queued_bytes／records，以及 scheduler_errors_total、log_written_total／dropped_total／failures_total。

线程池名固定为 catalog、downloads、processing、media-upstream（后者仅启用代理时存在）；pool_queued／running 为 gauge，pool_completed_total／failed_total／rejected_total 为 counter。completed 表示执行结束且包含 failed，不能相加当成总数。关闭的功能以零或无该线程池表达。

admin/status 用 worker_pools、events.subscriptions 和 logging 对象提供相同资源信息。storage.available 表示数据库已打开，不代表主动磁盘健康检查。当前没有 CPU／RSS／磁盘余量、SQLite 等待队列长度或按错误码拆分的专用指标；失败和延后需结合结构化日志定位。

## 文件日志

```json
{
  "logging": {
    "format": "json",
    "level": "info",
    "file_enabled": true,
    "max_file_bytes": 10485760,
    "retained_files": 5
  }
}
```

前台默认 file_enabled=false，异步输出 stderr，启动 ready 行保留在 stdout；开启后写 data_dir/logs/music-service.log。Windows 服务模式自动启用文件日志，并以 startup.ready 结构化事件报告启动。

max_file_bytes 支持 64 KiB～1 GiB，retained_files 支持 0～20，默认活动文件 10 MiB 加 5 个编号归档（正常写入合计最多 60 MiB）。写前超过阈值时依次滚动 .1～.N，0 表示不留归档。支持中文路径、追加及重启后的大小检查。已有超大文件、旧配置留下的更多归档不会被启动阶段批量删掉，应按部署保留策略处理。

文件写入、刷新和轮转只在 AsyncLogSink 后台线程执行。队列最多 1024 条／1 MiB，满时丢弃并累计 dropped；停机排空预算 500 ms，超时明确累计丢弃且不会无限等磁盘。写入／轮转失败累计 failures，不能继续越过上限追加；后续写入会重新打开尝试恢复。单条超过文件大小上限也计失败。先取得实例锁再打开文件日志，重复启动不会轮转正在运行实例的日志。

日志沿用统一秘密脱敏与单行编码。初始化文件路径失败会拒绝启动；运行期失败可由 admin/status 和 metrics 观察。控制台／服务默认日志级别不改变；日志级别筛选不影响指标采样。

## 取消换源与兼容边界

跨平台匹配、手动换源和自动换源均已取消。没有候选接口或对应业务分支；目录源缺少音源时明确拒绝，不借用另一平台音频。同源 allow_lower 品质策略继续保留。

旧请求中 disabled 的 fallback 和 null match_selection 仅作兼容，启用或提交目标返回 400。旧配置 automatic_fallback=false 可继续读取，true 拒绝；match_seconds、handles.matches 仅校验旧值后忽略。任务持久化保留 selection.kind=same 和原指纹编码，同源 schema 1～4 可恢复、复用；manual／auto 编码显式报错，不静默改成同源任务。

## 验证

Windows MSVC Debug 构建与本地回环测试覆盖：指标并发、延迟桶、异常、401／403、JSON／文本导出、过载控制接口响应、真实本地下载／处理／媒体／SSE 计量、日志脱敏／轮转／重启追加、归档路径失败、超大记录、零归档及 Windows 服务命令钩子。

测试使用已有离线平台替身和本地音频样本，不等同于真实音乐平台或 Windows SCM 安装验收。

2026-10-24 最终验证：

- Windows x64 MSVC Debug／Release 构建通过，Release 可执行文件版本 0.1.23，7,707,648 字节（约 7.35 MiB）。
- 全量 CTest 593 项：586 通过、6 个在线／样本测试按配置跳过、1 个既有失败。失败为 config_test.cpp 的 “features the build lacks cannot be enabled”：原有未提交 OAuth 改动已接受 app_profiles，旧测试仍要求拒绝；本次审查基线已复现，本次未修改该断言或 OAuth 行为。
- Release 实际进程在独立 data_dir 启动：JSON 返回 8 类操作、control_request=601，未认证指标请求为 401，管理员 Prometheus 导出为 200。异步日志快照 written=605、dropped=0、failures=0；两份文件分别 43,165 与 65,367 字节，均在配置的 64 KiB 上限内。该隔离冒烟进程最终强制结束，优雅停机由集成测试覆盖。
- 构建日志：out/windows-observability-build-final.log、out/windows-observability-release-build.log；全量日志：out/windows-observability-ctest-final.log；实际进程摘要：out/windows-observability-smoke-result.json。
