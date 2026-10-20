# cpp-httplib 运行时与后台任务

阻塞 I/O 的执行器、任务状态机与资源边界如下。

## 一个进程，三套 HTTP 监听

控制 API 使用独立的 `httplib::Server`，默认监听 17632；文件和音频代理监听 17633；SSE 事件监听 17634。三个实例共享服务、授权、SQLite 和配置，分别使用独立 TaskQueue。未启用的媒体／事件功能不启动相应监听。

长响应的 ContentProvider 仍会占用服务工作线程；仅把上游请求放进另一个线程池，不能释放负责向客户端发送音频的服务器线程。因此这里在监听和线程池层面隔离媒体流，不能把 content provider 当成非阻塞 HTTP。[httplib 固定版本说明][httplib-readme]、[接口与线程池源码][httplib-source]

客户端从 `/api/v1/info` 获得 media_base_url 和 events_base_url，或使用返回的完整链接。反向代理可将三套路径映射到同一外部 origin；直接部署使用三个端口，不要求前端推算端口关系。

## 执行器和额度

数值默认值只在[配置与部署](configuration-and-deployment.md#配置来源与生效时机)维护，本页说明调度行为；修改配额不必改写多个规范表或架构图。

| 资源 | 配置归属 | 调度与排队 |
| --- | --- | --- |
| 控制／SSE／媒体 HTTP 池 | control、events、media | 各自有界队列；SSE／媒体响应额度低于线程容量，保留拒绝和预检余量 |
| 昂贵控制操作 | admission | 非阻塞获取全局及 principal 额度，超额即响应 |
| 短上游请求池／聚合 fan-out | upstream、catalog | 有界排队，按来源／principal 公平推进，共享请求截止 |
| 下载执行槽／实际上游传输 | downloads | 持久任务排 SQLite；执行槽与实际 source 的网络额度分别管理 |
| processing 池 | processing | 有界队列；下载可等处理，处理不能反向等下载 |
| 可选 helper | external_adapters | 联合预留进程与已验证网络上界，真实退出后释放 |
| 媒体响应／上游池 | media_limits、media_upstream | 已获响应额度才建流，不排长队；本地文件不占上游池 |
| 全部上游物理连接 | upstream | 活动与空闲连接都受总上限约束，不能靠释放操作租约无限缓存连接 |
| SQLite writer／读取连接 | storage | 单 writer、有限读取，短事务、有界排队与超时 |

搜索与媒体隔离不保证任意负载下完全无延迟。慢连接、CPU、磁盘和数据库仍可能影响全进程；通过读写超时、容量和公平调度把影响控制在明确范围内。

请求已到业务入场层：单调用方额度不足返回 429，整体容量不足返回 503，并附 Retry-After。若连接在 httplib TaskQueue 入队之前就被拒绝，可能只能关闭连接，不能承诺一定发出 JSON 429／503；客户端应将连接失败与业务限流分别处理。

### 不允许的等待关系

控制 HTTP 线程可以等待有界短请求池，但短请求池不能反向等待控制线程。下载线程可以提交 processing 或数据库操作；processing 和数据库执行器不能反向等待下载线程。任何线程不在持有共享调度锁或数据库事务时等待网络、文件处理或另一个 future。

执行器派发与上游入场分开。已明确请求目标的网络操作先入场再派发；Provider 内部构造下一条请求时也只能非阻塞申请额度，不能让短请求工作线程阻塞在某个平台的 semaphore 上。无法入场时将 Deferred 交回协调者，释放 worker，不在同池提交并等待子任务。

平台数不决定线程数。注册表按需创建能力对象；平台、操作、origin 与共享应用账号配额组均可施加额度，跨源任务按实际请求的 source 计入。429 的 Retry-After、退避和熔断按实际影响范围记录，单个账号失效不能停用其他账号。配置和缓存要求见[平台扩展](provider-extensibility.md#8-大量平台的运行方式)。

外部工具不得在已占满的同一个池内等待它依赖的任务；外部进程名额在派发前获取，超时先请求停止再按收尾预算终止进程树，真实退出后才归还。原生传输与外部传输的网络预算必须分别可计量；外部 profile 须声明并验证工具内部连接／重试上限，按其上界预留公共连接额度，无法控制时不声明满足严格总连接上限的传输能力。

### 上游入场与租约

| 组件 | 拥有的状态与决定 |
| --- | --- |
| downloads/Scheduler | principal 公平性、持久任务领取、到期唤醒、下载执行槽；不拥有平台连接计数 |
| runtime/admission | 唯一的上游额度账本、可运行等待项、来源／origin／共享应用组冷却及租约 |
| 应用服务／Runner | 准备具体操作、授权上下文与恢复位置；决定是否继续、延后或按业务规则失败 |
| HttpTransport／外部执行器 | 校验租约与实际目标匹配、执行 I/O、报告限流反馈；不另外建立平台 semaphore |

每次网络操作提交 AdmissionRequest：principal、实际 source、operation、traffic_class（short／download／media／auth）、AuthIdentityKey、规范化 origin、稳定 rate_limit_group、deadline，以及所需连接数／helper 进程数。auth 也使用短请求池及其限额。应用配额组由可信管理配置和平台描述确定，不接受客户端任意覆盖，不随 credential_version 或 app_token_version 改变。

UpstreamAdmissionPort.try_acquire 返回 Granted（独占 UpstreamLease）、Deferred（受阻维度与最早重试时刻）或截止／取消错误。满足所有相关额度且有可用执行容量时才同时预留，失败不留下部分租约；执行器提交失败立即撤销未开始预留。配额检查与内存账本更新不跨网络、磁盘或 DB 等待。缓存命中和纯解析不申请网络额度。

同源解析、实际音轨传输和代理分别申请各自操作的租约。例如 spotify 元信息调用只占 spotify 的短请求／账号组额度；解析确定 youtube 音源后，按 youtube 和实际 CDN origin 申请下载额度。不能用原曲身份占满整个任务生命周期，也不能拿一次解析租约覆盖后续全部 HTTP。需要授权刷新时先结束原响应并释放其租约，再独立入场刷新，避免等待自己占用的额度。

重定向必须先结束上一跳，再按新 origin 和允许传播的授权身份重新入场；actual source 仍是该资源的来源。无法立即入场则把待执行目标与安全恢复信息交回协调者，不能持有旧租约等待新额度。多步协议暂停时保留有界、非秘密的继续信息；无法安全继续的步骤只能按适配器声明重新执行幂等读取，不能自动重放已发送的非幂等授权操作。

短请求协调者把 Deferred 操作放入原有有界待执行队列，按 principal／可运行来源公平推进，不新增无界队列；容量不足或总截止时间到达时走既有 429／503／超时规则。媒体建流不排长队，无额度即在发头前拒绝；已发头时终止流。下载按下节持久延后，不占 worker 等待。额度归还可唤醒等待者，冷却到期用定时器，不以反复占用 worker 的忙轮询实现。

并发租约在实际 I/O 结束后才可归还；请求停止、HTTP 已返回或 worker 句柄超时都不等于操作结束。客户端连接可复用但不是额度本身，新操作仍须新租约；物理连接池同时计入活动与空闲连接上限，不能归还操作租约后无限保留空闲 socket。Retry-After 冷却与速率窗口独立于并发租约，释放连接不会重置它们。helper 按声明上界一次预留，由实际执行器消耗；其内部已预留连接不再重复申请，结束后才释放，超出声明上界的实现不能启用。

## httplib 适配规则

| 位置 | 设计 |
| --- | --- |
| 服务 TaskQueue | 显式设置基础／最大线程与队列容量，不依赖库版本默认值 |
| Client 所有权 | 一次请求独占一个 ClientLease；连接复用仅在 origin、完整 AuthIdentityKey、代理、TLS 策略一致时进行；每次操作另验证 UpstreamLease |
| 普通响应 | JSON／文本及错误响应使用 upstream.max_response，超过上限中断 |
| 下载响应 | response handler 校验状态与头，content receiver 按块写任务独占文件，不累积完整 body |
| 传输取消 | stop_token → 当前传输句柄；在接收回调中检查，并使用适配器验证过的 stop／关闭方式 |
| TLS | 校验证书链与主机名；按实际编译后端配置可信根，不借关闭验证解决上线问题 |
| 凭证 | Cookie／OAuth token／API key 经显式上下文传入；禁止全局“当前平台账号”，敏感头不得跨来源继承 |
| 重定向 | 每跳重新校验地址和协议，跳数受 upstream.redirect_max 限制，清除不应跨来源发送的敏感头 |
| 明文 HTTP | 默认只允许 HTTPS。平台包可在 descriptor 中按“主机 + 路径前缀 + 操作”登记确实只有明文的端点，配置只能关闭、不能扩大该清单；明文请求不得携带 Cookie、Authorization、token 或签名秘密。HTTPS→HTTP 降级跳转默认拒绝，仅已登记的音频 CDN 下载可接受并剥离全部敏感头；明文响应同样经大小、格式和资源证据校验 |
| 错误 | 保留网络类别、HTTP 状态、平台业务码；不把 403 一概说成断网 |

`ThreadPool` 构造参数及 `Get` 的 response handler／content receiver、`stop()` 等均在已检查源码中存在；组合行为须经集成验证。[固定版本源码][httplib-source]

### 截止时间与请求生命周期

搜索总预算和每来源预算使用 catalog 配置；客户端 timeout_ms 的允许范围见 API，来源截止不得晚于总截止。排队时间也计入预算。底层连接、读与写超时分别设置，不能拿某个 socket 空闲超时冒充端到端总截止时间。

等待过程中监测前端断开和取消；超时后聚合返回已完成结果，并请求取消剩余工作。任务只持 shared operation state 的值拷贝，不捕获稍后已失效的 Request／Response 引用。迟到结果不得再写响应，也不得覆盖新搜索。

DNS、TLS 或系统调用能否即时中断需要按适配器验证。在尚未能即时中断的路径上，保留任务句柄与额度直到其真实退出；设置连接和空闲上限，避免把未结束工作隐藏成“已取消且不占资源”。

## 搜索与缓存

缓存键使用完整 [AuthIdentityKey](data-models.md#账号绑定与授权身份)，再包含地区、实现上下文版本、操作、查询、过滤条件与页游标，防止同 principal 下两个同版本账号互相污染。缓存、失败抑制、上下文、游标和在途查询合并使用同一身份规则，不由各服务自行省略字段。缓存条目数、字节预算和 TTL 使用配置的 cache；受权限数据只在对应 principal 内共享。

平台禁用、凭证更新／撤销、应用身份变化使对应缓存失效；默认账号经 credential_defaults 事务切换，新请求使用新绑定；隐式默认选择的句柄还校验 selection_version。旧在途工作只可写入自己的旧身份分区，不能重新填充当前身份缓存；旧版本分区按预算清理。旧游标不得在新身份下继续，过期／变更响应见[模型](data-models.md#分页与部分成功)。失败不缓存为成功空列表；可按 cache 中的短期错误抑制预算合并重复失败，并保留错误类别。SourceResult.cache 必须说明是否命中与数据时间。

匹配请求只有完整身份键和查询语义相同时才能共用在途查询，每位等待者有自己的截止时间；最后一位等待者退出后才取消可共享的操作。匹配上下文记录原曲及各候选的身份键；新选择时核验，失效返回 410 RESOURCE_EXPIRED。已接收任务使用持久目标与绑定，不再依赖匹配句柄。asset 同样绑定创建身份，新媒体请求检查绑定仍可租用且版本有效，失效要求重新解析；已入场操作按其独立租约收尾。

## 下载状态机

```text
queued → resolving → probing → downloading → processing → finalizing → completed
   │         │          │            │             │            │
   │         └──────────┴────────────┴─────────────┴────────────┴→ failed
   │                              │
   │                       waiting_retry → resolving / probing / downloading
   │
   └→ canceled

resolving / probing / downloading / processing → canceling → canceled
waiting_retry → canceled
```

finalizing 不能长期停留：同文件系统重命名失败、目标冲突无法确认归属、磁盘或权限错误，以及恢复时意图存在但产物缺失且策略不允许重做，均通过条件转换进入 failed（error.code 为 FINALIZATION_FAILED），释放目标预留并保留诊断材料，不标 completed。允许重做的情形回到 waiting_retry（wait_reason=upstream_retry，resume_stage=resolving），按正常逻辑尝试计数。canceling 期间出现的错误仍以 canceled 结束，错误只记录在诊断中。

waiting_retry 区分 wait_reason=upstream_retry（可重试上游失败）与 admission（等待本地入场／已知配额冷却）。resolving／probing／downloading 可在实际 I/O 停止并保存恢复点后进入该状态；再次领取从允许的 resume_stage 推进，资源失效时先重新解析。图中的失败出口不表示所有失败都使用同一重试规则。`probing` 可在已有足够证据时跳过；processing 至少完成必需格式与长度验证，可跳过可选标签。

`completed`、`failed`、`canceled` 为终态。手动重试 failed／canceled 任务创建新的 task_id、设置 retry_of，不把旧终态任务原地改成 queued；新的客户端可保留旧失败记录。自动网络重试仍属于原任务，不需要客户端调用 retry。

### 入队与调度

先完成身份与请求结构校验并查已有幂等记录；新请求在事务外准备 NormalizedDownloadIntent、账号绑定、目标与必要上下文，再在短事务内复核版本、权限、容量并提交幂等、指纹、任务与批次。提交成功才能返回 202；提交失败返回 503，不能先告诉前端已创建再依赖稍后落盘。

下载调度器按 principal 轮转，再按各 principal 内的 created_at、next_retry_at 和空闲执行槽选择任务；领取前查询入场管理者的就绪提示，跳过 original_track.source 或已知实际来源当前受限的任务，提示不是租约。此时不按 original_track.source 预占网络额度。非终态任务与批次受 download_limits 约束，批次创建也纳入相同额度。被平台限制的队首任务不阻止其他可运行任务。

领取任务通过条件更新和执行租约完成，同一任务同时只能有一个 runner。内存唤醒失败时由周期性 DB 扫描兜底；一个 data_dir 同时只允许一个进程，因此首版不承担跨节点租约协调。

Runner 遇到 Deferred 时，先确认旧 I/O 已结束，再带 execution_id/version 保存 waiting_retry、wait_reason=admission、next_retry_at、resume_stage、attempt_open 与恢复证据；提交后归还执行槽和全部操作租约。下一次领取产生新 execution_id，继续同一逻辑 attempt；如果尚未开始第一次尝试，attempt 仍为 0。只有真正开始一次新逻辑尝试才调用 begin_attempt 递增计数，不因本地繁忙耗尽配置的上游重试预算。旧世代回调不得修改新执行。

admission 延后不保存短期签名 URL 或秘密；必要时按持久 ref 重建资源并重新核验 partial。公共 waiting_retry 不增加另一套任务状态，wait_reason 只说明为何等待，resume_stage 为受校验的内部恢复位置。首次 begin_attempt 时固定绝对任务 deadline；此前的排队和入场延后只受队列规则约束，不消耗任务预算，避免本地拥塞让尚未开始的任务超时。deadline 固定后，入场等待、重启和冷却均不延长它；DB 无法提交延后状态时保留执行所有权并进入既有存储故障处理，不能先释放再让另一个 Runner 领取。

### 执行阶段

1. 读取 normalized_intent，按固定 AccountBinding 租用最新有效授权版本，记录本次 AuthIdentityKey；不跟随默认账号变化。
2. 根据原始 ref／持久手动目标获取必要上下文，执行品质及固定的换源策略，每次具体上游操作单独入场。
3. 检查资源是否试听、品质是否满足、大小与空间是否允许；获得一致性证据。
4. 分配目标目录内 `.music-dl-work/{task_id}/`，打开唯一临时文件。
5. 按 actual_track.source、实际 origin 和账号组取得下载租约，再协商 Range 和流式写盘，检查大小上限与短响应，周期性更新进度和检查点。
6. 释放下载网络额度，再将必要解密、验证、标签处理投给 processing 池。
7. 完成前最后检查取消；通过条件转换进入 finalizing，写发布意图。
8. 同文件系统重命名、记录资产并标记 completed，返回 file_id 与处理报告。

最大文件大小、传输空闲、逻辑尝试与首次尝试开始后的绝对预算使用 downloads／download_timeouts；尝试预算不含该尝试因 admission 暂停的时间，绝对预算包含。暂停与重启不会重置 deadline 或已消耗次数；已超出预算的任务明确失败。长期、慢速需求可调大预算，不能无限重试。

### 进度与完成

接收字节在内存中更新，普通公开／持久检查点按 event_stream.progress_interval_ms 节流，重要状态转换立即更新。公开快照 version 与可见进度同步递增，允许 ETag 条件轮询。总大小未知时返回 total_bytes=null，禁止伪造 100%；completed 是发布结果，不是仅网络接收完成。

文件已下载但标签 best_effort 失败时，任务完成且带 warnings；required 模式失败则不发布资产。写独立歌词也有完成结果，不能发起异步写入后立即宣称所有文件已经成功。

### SSE 事件传输

主通道为 SSE，每个前端复用一条订阅接收其 principal 的全部任务变化。普通进度按配置节流，状态转换在持久提交后立即发布；只有已提交的公开版本才能通知。详情与恢复流程见 [API 9.4](api-contract.md#94-sse-下载事件)。

官方 [SSE 服务端示例](https://github.com/yhirose/cpp-httplib/blob/74ef3c42188acd6391f027c4337fb0e1a68c0024/example/ssesvr.cc) 使用 `set_chunked_content_provider`、`text/event-stream` 和 `DataSink`。这是阻塞式长响应，不是协程 I/O。每条订阅由事件 Server 工作线程发送；下载线程仅投递有界通知，不直接对慢客户端写 socket。

每订阅事件数、编码字节总量与单事件大小同时遵守 event_limits。同任务尚未发送的进度可替换为较新版本；不能静默丢弃终态。超限或内部发布失败时标记需要重新同步并关闭受影响订阅，客户端重连查询事实状态。发布失败不得回滚已提交任务。任务状态以 SQLite 为准，首版不建立持久事件日志。

等待事件使用可唤醒的条件变量；心跳、写空闲和单连接期限使用 event_stream 配置，到期正常断开重连。DataSink 写失败、客户端断开、身份撤销或服务退出时，注销订阅、释放队列和额度。撤销检查至少在心跳时执行；控制 API 的短请求总截止时间不套用到 SSE。所有订阅状态使用受控共享所有权，不捕获已销毁的 Request／Response。

事件流已发头后不追加 JSON 错误信封；关闭流后由客户端恢复。正常停机唤醒所有事件等待者并关闭订阅，再销毁事件分发器；退出不等待正常连接期限。心跳、限额、重连和慢消费者纳入集成验证。

### 取消竞争

queued／waiting_retry 可立即转 canceled。运行阶段先持久记录 cancel_requested，并进入 canceling，然后取消当前请求或让工作在安全点退出；非可中断的标签库需要等待安全结束，不能从 UI 假装已终止。

进入 finalizing 的条件转换与取消命令由同一持久版本检查排序：取消先到则不能发布；finalizing 先到则取消返回 409 TASK_FINALIZING。completed 后取消也返回 409 TASK_TERMINAL，删除文件是另一种操作，首版不把取消隐含为删文件。

## 重试、换源和续传

| 失败 | 动作 |
| --- | --- |
| 瞬时连接中断／空闲超时 | 按 downloads.transport_retries 和 retry 配置进行有限退避与抖动 |
| 本地额度不足／已知配额冷却 | waiting_retry，wait_reason=admission；释放执行槽，按恢复位置继续，不额外消耗 attempt |
| 实际请求收到 429／上游 5xx | 按 RetryPolicy 结束本次逻辑尝试，waiting_retry 的 wait_reason=upstream_retry；尊重有上限的 Retry-After，总尝试数受 downloads.max_attempts 限制 |
| CDN 403／404／410 | 不盲目重复同一 URL；重新解析同一实际曲目，检查资源身份后再续传或重下 |
| 会员／凭证失效／能力不支持 | 返回业务错误，不用网络重试掩盖；auto 换源仅对明确允许的类别执行 |
| 磁盘不足／路径无权限／标签 required 失败 | 明确失败，等待用户处理后创建重试任务 |
| 用户取消 | 不重试、不自动换源 |

waiting_retry 释放执行线程与在途额度，next_retry_at 存 DB，由调度器到时重新领取，不能让下载工作线程 sleep 很久。upstream_retry 同时关闭旧尝试（attempt_open=false）并设置 resume_stage=resolving；admission 保持原 attempt_open 和已验证恢复位置。重新领取只能由 begin_attempt 为已关闭的尝试开始新计数。

续传要求部分文件与资源身份匹配：优先使用强 ETag、可用的 Last-Modified／If-Range、总大小、平台文件 hash 和实际歌曲／品质。只有 URL 相同、文件名相同或时长相近不够可靠；证据不足就重下。

用户取消后清理未发布工作文件；失败工作文件按 filesystem 配置保留供诊断与受控检查，手动新建重试任务不承诺复用它。service_shutdown 与崩溃恢复所需工作文件依据活动任务及发布意图保留，不能套用终态清理规则删除。

请求 `Range: bytes=N-` 后严格核对 206 的 Content-Range 起点和总长；200 覆盖重下；416 仅在总长恰好等于已完成长度且身份与校验成立时进入完成验证，否则重新解析或重下。资源改变必须丢弃旧前缀，不能把不同品质或录音拼接。

换源在开始传输前确定实际曲目并记录；已经写入的字节不能继续拼接另一来源。若策略最终选择其他来源，必须使用新的工作资源身份与从零开始的字节流。

## 崩溃恢复与优雅退出

| 持久状态 | 启动恢复 |
| --- | --- |
| queued／waiting_retry | 保留等待原因、恢复位置、尝试开闭及等待时刻；检查预算后重建入场申请，不能从 DB 恢复旧网络租约 |
| resolving／probing | 清理旧进程租约，重做解析，保留尝试与预算 |
| downloading | 校验部分文件和检查点，重新解析并验证身份，续传或安全重下 |
| processing | 使用保留的传输完成文件重做处理；不在被半改写的文件上盲目继续 |
| finalizing | 根据 finalization_intent、文件存在性与证据补齐发布；无法补齐时按[崩溃矩阵](internal-contracts.md#7-文件发布与崩溃矩阵)转 waiting_retry 或 failed |
| canceling | 完成清理后 canceled，不擅自恢复成可下载任务 |
| 终态 | 保留历史，懒检查对应文件存在性 |

恢复不重置预算，也不依赖旧签名 URL。正常停机先将 readiness 置失败、停止接收新任务与领取任务，再按 shutdown 的软收尾预算等待在途工作；随后以 service_shutdown 原因请求停止、保存检查点、释放媒体流、关闭监听并等待可控工作退出，最后关闭 DB。

service_shutdown 与用户取消不同，保留可恢复工作；退出后服务管理器若强制终止，启动仍按上表恢复。取消请求不保证不支持中断的第三方代码能在软收尾预算内返回，需在接入验证中排除无界操作，并由外部服务管理器设置最终停机上限。

## 媒体流桥接

httplib Client 没有可以直接拿来当通用异步流的原生协程契约。媒体桥接采用受限生产者／消费者：

```text
媒体上游池：建立请求 → 得到并校验最终响应头 → 写入有界字节缓冲
                                     │ headers-ready
媒体 Server：等待头部／错误 → 设置状态与 ContentProvider → 缓冲 → DataSink
```

每条播放流使用 media_stream.bridge_buffer 上限，超过则阻塞生产者读取；缓冲耗尽时消费者有超时等待。内存预算还要计算库缓冲、TLS 和线程栈，不能只统计桥接缓冲。

必须在向下游提交头之前完成上游状态、Range 和有限刷新判断。不能先返回 200，再发现上游 403／416，却把错误页接进音频。请求中途失败、下游头已经提交时，只能终止流并记原因，不能再发一段 JSON 错误作为音频尾部。

未过期 asset 在发头前发生上游 403，可刷新同一资源地址一次，沿用请求 Range 并核对身份；无法确认一致性时返回 409 RESOURCE_CHANGED，要求重新解析并由客户端重新取得媒体地址。已到期 asset 直接返回 410，不在旧句柄下延寿。解析时的 auto fallback 不延伸为传输中的自动替曲。

文件发送直接从只读资产句柄按范围读取，不占媒体上游线程。所有 content provider、releaser 和上游 worker 共享该次 HTTP 传输状态；任一侧取消、断开或失败都唤醒另一侧并释放额度，不保存前端播放位置或维护播放会话。直链模式不经过 StreamBridge，也不占后端传输额度，后端不能观测其客户端播放进度。详情见[API 媒体契约](api-contract.md)。

## 验证场景

优先验证：满负载媒体下控制请求仍受明确预算约束；多前端同时搜索不出现同池等待；取消实际终止请求；跨用户 Cookie 不串用；200／206／416 与资源变化不损坏文件；重启覆盖所有任务阶段；文件发布与 DB 崩溃窗口可恢复；慢消费者内存有界；过期票据不能创建新媒体请求。

[httplib-readme]: https://github.com/yhirose/cpp-httplib/blob/74ef3c42188acd6391f027c4337fb0e1a68c0024/README.md
[httplib-source]: https://github.com/yhirose/cpp-httplib/blob/74ef3c42188acd6391f027c4337fb0e1a68c0024/httplib.h
