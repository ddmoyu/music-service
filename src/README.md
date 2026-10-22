# 源码

每个模块一个目录，头文件（`.hpp`）与实现（`.cpp`）放在一起。所有模块以 `src/` 为包含根，例如 `#include "domain/result.hpp"`、`#include "application/ports/runtime/admission.hpp"`。C++ 命名空间与目录对应：`music_service::domain`、`music_service::application`、`music_service::infrastructure`。

只在单个模块内部使用的头文件放进该模块的 `detail/` 或具体子目录（如 `domain/detail/`、`infrastructure/config/address.hpp`），其他模块不得包含。公共类型不暴露 httplib、JSON、SQLite 或操作系统类型。

## 模块与依赖

| 目录 | 职责 | 当前状态 |
| --- | --- | --- |
| `domain` | Result 与错误码、平台引用、曲目与分页、品质判定、按字节的容器识别（含 HTML／JSON 错误体）、账号绑定与授权身份、任务状态迁移与取消规则、下载意图与指纹 | 静态库，T02 |
| `application` | `events/`：任务事件中心（按 principal 订阅、全局与每 principal 订阅上限、每订阅事件数／字节／单事件上限、同任务未发送版本合并、溢出关闭要求重同步、发布不等待订阅者）与提交后发布的任务／发布仓储装饰器；`tasks/`：下载意图准备（固定账号绑定、目标目录、默认值）、幂等创建与重放、活动去重、已完成结果复用复核、取消、手动重试、列表与有限批次；`media/`：已发布文件访问（归属、目录 ACL、发布大小与 missing 标记）、媒体票据（只存哈希、有界内存、绑定目标与签发 token）与 media-access 访问描述；`catalog/`：搜索（多来源有界并发、逐源结果与部分成功）、单曲详情、分享链接解析、音源解析与有限探测；缓存、在途合并、游标与 asset 句柄均按完整授权身份分区；`transport/`：URL 规范化、每跳请求与重定向规则、按 origin 逐跳入场的 UpstreamClient；`accounts/`：服务身份与 token 离线管理、token 认证、平台凭证（Cookie 增删改、校验、默认选择、账号绑定、租用、孤立秘密回收）；`ports/`：平台能力与 ProviderBundle 契约、ProviderDirectory、授权校验端口、token 生成与交付、上游入场与租约、时钟、上游 HTTP、音频处理（AudioProcessor 与处理计划）、任务／发布／身份／凭证／目录绑定仓储、目标目录文件（DestinationFiles）、SecretStore 等出站端口；`config/`：生效配置 ServiceConfig；用例按任务陆续加入 | 静态库，T02／T03／T08／T09／T13／T14／T18／T19 |
| `infrastructure` | `transport/`：cpp-httplib + Mbed TLS 上游传输；`auth/`：token 生成、哈希与安全交付文件；`config/`：配置加载与校验；`observability/`：结构化日志、脱敏、关联 ID、指标；`clock.hpp`；`filesystem/`：路径约束、安全文件原语、实例锁、数据目录与目标根、目标目录内的文件指纹（SHA-256）与不覆盖移动；`secrets/`：文件 SecretStore；`storage/`：SQLite 数据库、迁移与各仓储；后续加入上游传输 | 静态库，T03～T05、T08、T09 |
| `providers` | `registry/`：ProviderRegistry（bundle 契约校验、按需创建能力对象、授权 profile 校验器、providers 分页）与按 `MUSIC_SERVICE_PROVIDERS` 生成的内置清单；`kuwo/`：酷我协议包（搜索、详情、链接、标准品质音源）；`fivesing/`：5sing 协议包（搜索、详情、链接、歌词、standard／high 音源） | 静态库，T07／T12／T22 |
| `downloads` | `finalizer`：已验证文件的发布（拒绝未通过验证或验证后变化的文件、按确定性候选名预留且不占用未知文件、不覆盖的重命名与位置迁移、资产与 completed 同事务、失败转 FINALIZATION_FAILED）；`recovery`：启动时按发布意图与文件证据补提交、迁移、转 waiting_retry／failed，目标离线时保留，canceling 收尾；`resume_policy`：半文件续传判定（资源身份、总长与强校验器均成立才从持久检查点续传，否则重下）；`scheduler`：持久任务的公平领取（principal 轮转、入场就绪提示跳过受限来源、暂停只停领取、停机时停止并保存检查点）；`runner`：一次执行的解析、流式写盘与续传、入场延后、有限重试、processing 与发布、取消收尾 | 静态库，T16、T17 |
| `audio_processing` | `format/`：有界窗口读取与 MP3 帧结构校验（标签跳过、截断、损坏、Xing／VBRI 帧数、实测时长与码率）；基线处理器 audio.verify v1 与按 id／版本查找的 ProcessorCatalog；解密与标签写入随 T30 及平台加入 | 静态库，T15 |
| `runtime` | 有界执行器、统一上游入场账本（AdmissionLedger）、截止定时器、停机序列 | 静态库，T06 |
| `api` | 控制入口（cpp-httplib）：Bearer 认证与 scope、JSON 信封与错误映射、请求 ID、CORS、请求体与类型限制；health、info、providers、limits、destinations、credentials、credential-defaults、admin/status、admin/download-policy、files 与 media-access 路由 | 静态库，T10 |
| `event_server` | SSE 入口（cpp-httplib，独立监听与线程池）：`GET /events/v1/downloads` 的鉴权、scope、订阅额度与 JSON 错误，stream.ready、task.updated、注释心跳，连接期限、心跳时复核 token、溢出或停机结束流 | 静态库，T18 |
| `media_server` | 媒体入口（cpp-httplib，独立监听与线程池）：`/media/v1/files/{file_id}`（Bearer）与 `/media/v1/tickets/{token}` 的单段 Range、416、If-Range、If-None-Match、HEAD、Content-Disposition，按 media_limits 发头前取得、结束或断开时归还的响应额度，日志中的票据脱敏；`/media/v1/assets/{asset_id}`（Bearer）与指向 asset 的票据经桥接转发上游音频（media_proxy，T34）：Range、HEAD、416 与 If-Range 按上游实际应答转发，有界缓冲背压，断开即取消上游，单独的上游额度与 media_upstream 线程池 | 静态库，T19、T34 |
| `server` | music-service 可执行程序：命令行、离线身份管理命令、ProcessHost 启停顺序与依赖组装、目标目录绑定校验、监听前的发布与取消恢复 | 静态库 server_core + 可执行程序，T10、T16 |

依赖方向：application 依赖 domain；providers、downloads、audio_processing、infrastructure、runtime 和三个 HTTP 入口依赖 application 的端口与领域类型；只有 `server` 把具体实现组装在一起，业务层不反向依赖 HTTP、数据库或某个平台。

尚未实现的模块以 CMake INTERFACE 目标记录依赖，实现时改为普通库并明确 PUBLIC／PRIVATE。逐文件声明源码，不使用 GLOB；不通过空函数或空 main 伪造可运行产物。详细契约见[内部接口](../docs/design/cpp20-refactor/backend-service/internal-contracts.md)。
