# C++20 技术栈选型

服务框架暂定 cpp-httplib，Drogon 作为备选。上游客户端、TLS、平台加密和音频处理分别验证；其他网络库保留为选型参考，不与当前主方案并列推进。

更新日期：2026-10-19。状态：主框架已暂定，配套选型待验证。

主方案的模块、目录、API、控制／媒体／事件三类监听与有界任务执行细节见[后端服务详细设计](backend-service/README.md)。这里保留库能力与取舍证据；初始资源值与协议契约由详细设计维护。

本项目为纯后端，不包含 GUI。Windows、Linux 与 Docker 是选型必需约束，具体架构和支持状态见[配置与部署](backend-service/configuration-and-deployment.md#部署方式)；平台专用优化须有其他目标系统的对应实现。

当前项目的综合排序、Stars、近期提交与 Release 数据见[网络库对比与排序](library-comparison.md)。2026-10-19 用户决定暂定 cpp-httplib 为主方案、Drogon 为备选；阻塞 I/O 不作为排除条件。

## 技术栈总览

| 层次 | 当前方向或候选 | 状态与选择条件 |
| --- | --- | --- |
| 语言 | C++20 | 当前方案基线；验证语言特性与所选依赖，不把协程运行时作为前置条件 |
| 构建 | CMake、CMakePresets | 已确定方向；从基础阶段建立可重复的 Windows／Linux 构建与容器验证 |
| 依赖管理 | FetchContent，Git 源浅克隆 | 已确定；固定发布 tag 并校验期望 commit，不采用浮动分支 |
| HTTP Server | cpp-httplib 主方案，Drogon 备选 | 控制、媒体与 SSE 在同进程使用独立 Server／工作池；其他库保留研究参考 |
| 下载进度 | SSE 主动推送 + HTTP 查询 | 已确定；查询接口独立可用，支持初始状态、恢复与轮询兜底；SSE 不需要额外网络库 |
| 普通 HTTP Client | 先验证 cpp-httplib Client | TLS 或功能必要时独立替换；仍需验证 Cookie、重定向、超时和取消 |
| 下载传输 | 先验证 cpp-httplib 分块接收配合独立任务队列 | 验证 Range、进度、取消、资源一致性和稳定内存；必要时另配 curl |
| TLS | 按网络库支持评估跨平台后端；Windows 可选 Schannel | 必须覆盖 Windows／Linux 与容器；Schannel 是 Windows 适配，不能作为共同实现 |
| 平台加密 | 跨平台加密库；Windows CNG 可作目标适配 | 算法清单明确后再决定，各目标通过相同协议向量；不等同于 TLS 后端选择 |
| JSON | 详细设计首轮拟用 nlohmann/json | 用于 HTTP／平台解析边界，尚未引入；业务公共模型不暴露该类型 |
| 压缩 | zlib 等按协议需求引入 | 用于压缩响应、歌词等；区分 HTTP 自动解压和业务层解压 |
| 音频标签与属性 | TagLib；有限格式自行实现 | 候选，按 MP3、FLAC、M4A 等实际需求验证 |
| 转码与通用媒体处理 | FFmpeg 可选扩展 | 不作为原始下载和基础标签功能的默认强制依赖 |
| 持久化 | SQLite | 详细设计的任务／资产存储基线；后台任务接受前需提交，纯网络预验证可暂不接入 |
| 播放与界面 | 由外部客户端负责 | 本项目不引入 GUI、播放器内核或音频输出设备依赖；管理命令行属于后端入口 |

## 服务框架比较参考

| 维度 | Drogon | cinatra |
| --- | --- | --- |
| 产品定位 | HTTP 应用框架，提供路由、控制器、会话、过滤器及可选数据库模块 | 基于协程的 HTTP 网络框架，提供服务端与客户端 |
| 服务端与客户端 | 两者都有，官方列出 HTTP/1.0 和 HTTP/1.1 支持 | 两者都有，核心类型为 `coro_http_server`、`coro_http_client` |
| 网络模型 | Trantor 事件循环，异步非阻塞网络 I/O | Asio 网络 I/O 与 async_simple 协程体系 |
| C++20 使用方式 | 异步回调、`sendRequestCoro` 等协程接口 | `async_simple::coro::Lazy` 与异步网络接口 |
| Windows | 官方支持 | 需要在目标 MSVC 工具链验证客户端、服务端与所需功能组合 |
| TLS | 当前对应 Trantor 支持 OpenSSL、Botan、none | 已检查版本通过 Asio SSL 和 OpenSSL 实现 TLS |
| 内置 Schannel 后端 | 没有 | 没有 |
| curl 依赖 | 自带 HttpClient 不通过 curl 实现 | 自带 HTTP Client 不要求 curl |
| 下载注意事项 | 常规 HttpClient 在响应完成后回调，没有直接公开逐块写盘的下载接口 | 提供 `async_download`，但已检查实现以 trunc 打开文件，不能直接当作完整断点续传方案 |
| 依赖与裁剪 | Trantor、JsonCpp、zlib 等；ORM、工具、示例等可裁剪 | Header-only 不等于零依赖，仍需考虑 Asio、async_simple、TLS 等 |
| 目前结论 | 当前主方案的备选，启用时验证客户端下载扩展和 TLS 打包 | 保留研究参考，暂不进入当前实施路线 |

Drogon 的框架能力依据[官方说明](https://github.com/drogonframework/drogon#readme)、[HttpClient 接口](https://github.com/drogonframework/drogon/blob/master/lib/inc/drogon/HttpClient.h)及[响应完成后的回调路径](https://github.com/drogonframework/drogon/blob/master/lib/src/HttpClientImpl.cc#L577)。依赖与裁剪选项见[构建配置](https://github.com/drogonframework/drogon/blob/master/CMakeLists.txt)。

Drogon README 主要介绍 OpenSSL，但当前引用的 Trantor 子模块明确支持 `TRANTOR_USE_TLS=openssl|botan|none`。Botan 是可选替代后端，不能据此推断产物一定小于 OpenSSL；需要按实际构建测量。[Trantor 构建源码](https://github.com/an-tao/trantor/blob/78e480f/CMakeLists.txt#L158)

cinatra 的 TLS、下载和协程接口依据固定版本[客户端源码](https://github.com/qicosmos/cinatra/blob/cf095d224cb60ae7dfc623fde51c5c60db03c96d/include/cinatra/coro_http_client.hpp)与[构建配置](https://github.com/qicosmos/cinatra/blob/cf095d224cb60ae7dfc623fde51c5c60db03c96d/cmake/develop.cmake)。该版本客户端部分初始化路径使用 `verify_none`；正式实现需验证如何启用证书链与主机名校验，并覆盖重定向等后续连接路径。

## HTTP 客户端与 curl 的关系

| 候选 | 提供 Server | 执行方式 | 与 curl 的关系 | 当前用途判断 |
| --- | --- | --- | --- | --- |
| 框架自带客户端 | 所属框架提供 | 使用自身网络运行时 | 可不引入 curl | 统一运行时方便，但仍须验证完整传输需求 |
| libcurl multi | 否 | multi_socket 等接口可接入事件循环 | 就是 libcurl | 下载和 Schannel 路线的主要备选 |
| cofetch | 否 | Asio 驱动 libcurl multi，支持回调及 C++20 awaitable | 仍需链接 libcurl | Asio 体系的异步客户端候选，不能替代服务框架 |
| CPR | 否 | 常规 API 同步；GetAsync 等通过线程池执行同步请求 | 对 libcurl 的 C++ 封装 | 简化调用的备选，不等同于事件循环驱动的客户端 |
| cpp-httplib | 有 | 常规实现使用阻塞 socket I/O 和线程池 | 不依赖 curl | 本地或适中并发场景的重点候选；需要控制线程、排队与超时 |

libcurl multi 提供并发传输和事件循环集成方式，但依然需要设计调度、资源生命周期和文件输出。不能在服务事件循环中直接执行 `curl_easy_perform`，也不能把一个自行等待整个批次完成的 multi 循环当成已接入服务运行时。[multi 接口说明](https://curl.se/libcurl/c/libcurl-multi.html)

cofetch 在调用封装层减少样板代码，但并未去掉 curl。CPR 的 Async 命名也不表示底层 socket I/O 已改为外部事件循环驱动；其 `MultiPerform` 同样需要核对执行与等待方式。[cofetch 说明](https://github.com/SSARCandy/cofetch/blob/e25a2f6322e2b7866c22fd78364e3888efa60eef/README.md)、[CPR 异步入口](https://github.com/libcpr/cpr/blob/cdbbd90fb01c29dd52ed3e19f606bb4980e71541/include/cpr/api.h)、[CPR multi 实现](https://github.com/libcpr/cpr/blob/cdbbd90fb01c29dd52ed3e19f606bb4980e71541/cpr/multiperform.cpp)、[cpp-httplib 说明](https://github.com/yhirose/cpp-httplib#readme)

保留两个不同的客户端契约：普通请求返回受大小限制的响应；下载请求向文件或分块消费者输出。选择前者并不自动证明后者可用。

## cpp-httplib 与协程接入

cpp-httplib 当前仍使用阻塞 socket I/O，服务端默认通过线程池处理连接，没有原生 `co_await` HTTP 接口。可以为其增加协程友好的调用封装，但协程库不会自动替换底层网络操作。[官方说明](https://github.com/yhirose/cpp-httplib/blob/74ef3c42188acd6391f027c4337fb0e1a68c0024/README.md)

| 接入方式 | 能得到的效果 | 网络与线程成本 |
| --- | --- | --- |
| 有界线程池配合 awaitable 适配器 | 调用协程挂起，由工作线程执行 Get、Post 等操作，再投递结果恢复 | 每个正在执行的阻塞请求仍占用工作线程，并发受线程池容量限制 |
| 更换 Server 的 TaskQueue | 复用已有线程池或任务调度器 | 被提交的整段连接处理仍阻塞，不能因此变成事件驱动 HTTP Server |
| 有栈协程配合 I/O hook 或传输改造 | 在相关阻塞点主动让出运行线程 | 必须覆盖连接、读写、等待、DNS、TLS 及同步机制，平台相关，不是添加依赖即可完成 |
| 改写异步网络与会话层 | 有机会实现真正由事件循环推进的 HTTP I/O | 改动范围大，需维护状态机、取消、连接池与 TLS，并持续跟进上游 |

线程池适配的基本流程为：调用协程提交阻塞任务并挂起，工作线程调用 cpp-httplib，完成后把结果或异常投递到调用方执行器，再恢复协程。Asio 或 async_simple 可用于调度，但需要实现或选择相应适配器；`co_await` 本身不具有将同步函数转入后台的能力。[Asio 协程支持](https://think-async.com/Asio/asio-1.30.2/doc/asio/overview/composition/cpp20_coroutines.html)、[async_simple 执行器](https://github.com/alibaba/async_simple/blob/main/docs/docs.en/Executor.md)

例如，16 个专用于阻塞 HTTP 的工作线程，在它们都等待网络时只能同时推进约 16 个这样的任务，其余任务排队；创建更多等待协程不会增加传输并发。长时间音频下载与短 JSON 请求应有独立的并发额度，避免互相占满队列。

适配器还需处理取消与超时如何中止实际请求、客户端及缓冲区生命周期、异常传递和恢复线程。取消协程的等待，不等于后台的阻塞请求已经结束。业务协程也不能直接在网络事件循环中调用 Get 或等待 future 的 get。

服务端需单独评估。`new_task_queue` 只决定在哪里执行 `process_and_close_socket`；连接的读取、处理和响应仍由这段同步流程承担。HTTP handler 中启动后台协程后立刻返回，再通过原 Request、Response 引用发送结果，会违反对象生命周期或响应处理约定。若等待后台结果，原服务线程仍被占用；需要长期保持请求并异步回应时，原生异步服务框架的接入更直接。[线程池与 TaskQueue](https://github.com/yhirose/cpp-httplib/blob/74ef3c42188acd6391f027c4337fb0e1a68c0024/README.md#default-thread-pool-support)、[服务端任务提交](https://github.com/yhirose/cpp-httplib/blob/74ef3c42188acd6391f027c4337fb0e1a68c0024/httplib.h#L14633)

当前决定：采用 cpp-httplib 配合有界线程池作为主方案，先验证普通并发、多平台请求与持续下载。若调优后仍存在明确容量或功能缺口，再考虑 Drogon 备选；不预先改造 cpp-httplib 的 I/O 层。

该版本 TLS 后端包含 OpenSSL、Mbed TLS、wolfSSL。线程池或协程适配不会增加 Schannel 后端；Windows 的 CryptoAPI 证书链验证也不等同于使用 Schannel 完成 TLS。[TLS 后端与 Windows 证书处理](https://github.com/yhirose/cpp-httplib/blob/74ef3c42188acd6391f027c4337fb0e1a68c0024/README.md#ssltls-support)

## TLS 与平台加密

TLS 保护 HTTPS 连接；平台签名与加密用于生成参数、计算摘要、处理密钥或解密特定响应和音频。它们属于不同职责：即使用 Schannel 解决 HTTPS，也仍需实现平台协议中的加密算法。

先验证满足 Windows／Linux 和 Linux 容器的共同实现，再评估按系统选用后端。Schannel／CNG 只用于 Windows 适配，不要求 Linux 复用 Windows 系统接口；不同实现必须满足相同的身份校验与协议契约。Botan 等能否接入取决于实际网络库支持，不因加密库跨平台就推断框架已经支持该 TLS 后端。

| 方案 | 适用位置 | 需要明确的条件 |
| --- | --- | --- |
| curl 配合 Schannel | Windows 上游 HTTPS 与下载 | 实际构建的 libcurl 必须启用 Schannel；不是任意 curl 二进制都有该后端 |
| Drogon 配合 Botan | 使用自带 HTTPS 客户端或服务端 | 构建相匹配的 Trantor 与 Botan，验证证书和目标平台兼容性 |
| Drogon 或 cinatra 配合 OpenSSL | 使用各自已支持的 TLS 路径 | 接受该依赖，并测量裁剪后的发布包 |
| libhv 自带 Windows Schannel | 原生 Windows HTTPS 路径 | 已存在实现，但本轮测试发现默认接受未信任的自签名证书，修复前不作为正式出站 HTTPS 方案 |
| 单独接入 Schannel | 自行实现 TLS 传输适配器 | 需处理握手状态、缓冲、证书与主机名校验、连接关闭等，当前不作为首轮实现任务 |

curl 的 TLS 后端及能力差异见[官方比较](https://curl.se/docs/ssl-compared.html)。CPR 使用自带 curl 构建时有 `CPR_FORCE_WINSSL_BACKEND` 选项；若链接已有 curl，则沿用该 curl 的后端，不能仅靠封装层开关改变它。[CPR 构建配置](https://github.com/libcpr/cpr/blob/cdbbd90fb01c29dd52ed3e19f606bb4980e71541/CMakeLists.txt)

平台加密先按拟迁移的平台列出精确算法与字节约定，再确定实现库。检查项包括 AES 模式、填充、密钥与 IV、MD5 或其他摘要、RSA 及大整数需求，以及 Base64、十六进制和文本编码。只引入 AES 库不能覆盖整个平台协议。

Windows CNG、Botan 或裁剪库是否适用，需要逐项对照算法表。LibTomCrypt 涉及 RSA 时还需核对数学后端；tiny-AES 一类库只能覆盖有限 AES 功能，不负责摘要、RSA 和完整协议封装。各实现必须用固定输入输出向量对照 Go 版本，而不是只检查请求是否偶尔成功。

## FFmpeg 与轻量媒体处理

原 Go 实现中，MP3 直接写 ID3v2.3，其他部分格式通过 FFmpeg 写入元数据；本地媒体信息和 TUI 试听还涉及 ffprobe、ffplay。去掉元数据处理中的 FFmpeg，不代表同时替代了转码、媒体探测和播放。[原实现分析](../../research/go-music-dl/README.md#音频下载)、[原元数据处理源码](https://github.com/guohuiyuan/go-music-dl/blob/ddf2a4e550ca67086ff1f06edc5d15c7d4c8663d/core/service.go#L1700)

| 实际需求 | 轻量方向 | 边界 |
| --- | --- | --- |
| 保存平台返回的 MP3、FLAC 等文件 | 直接下载原始字节 | 不需要为了保存而转码 |
| 写歌名、歌手、专辑、封面、歌词 | 优先评估 TagLib | 按格式验证标签类型、封面和歌词字段兼容性 |
| 读取时长、码率、采样率等 | TagLib 支持的属性读取或有限格式解析 | 不能保证覆盖 ffprobe 的全部媒体探测能力 |
| 自行写标签 | 先限制到明确的 ID3v2 或 FLAC 元数据子集 | 需保留未知块、处理尺寸边界、保证重复写入不破坏文件 |
| 转码、重采样、复杂容器处理 | 需要相应编解码或容器库；可保留可选 FFmpeg | TagLib 不提供通用转码 |
| 试听播放 | 浏览器播放，或另选播放器后端 | TagLib 不能替代 ffplay；浏览器格式能力需独立验证 |

TagLib 的定位是音频标签与属性处理，支持多种常见格式，适合作为第一轮验证对象。[TagLib 官方说明](https://taglib.org/)

优先验证 MP3、FLAC、M4A 的标签读写与音频数据保真。对于暂不支持的格式，可保存原音频并输出独立歌词、封面文件，或按选项调用外部 FFmpeg。自行实现标签写入的工作量、格式覆盖和维护成本应与引入库的实际体积一起比较。

## 可组合的技术路线

这些组合是评估路线，不是已经选定的技术栈。

| 路线 | 组合 | 主要收益 | 关键验证 |
| --- | --- | --- | --- |
| 统一使用 Drogon | Drogon Server、HttpClient、Trantor OpenSSL 或 Botan | 单一网络运行时，服务框架能力完整 | 大文件分块接收、背压、取消与 TLS 打包 |
| 统一使用 cinatra | cinatra Server、Client、Asio、async_simple、OpenSSL | 协程接口集中，已有下载入口 | 证书校验、续传、文件输出与取消 |
| Server 与 Client 分开 | 候选 Server，出站 libcurl multi，Windows Schannel | TLS 与下载能力可独立选择 | multi_socket 接入、线程归属和退出流程 |
| 引入 cofetch | 候选 Server、cofetch、Asio、curl Schannel | 利用现有异步封装 | 与服务运行时的调度衔接、取消和文件输出 |

Drogon 使用 Trantor，cofetch 使用 Asio；cinatra 的 Lazy 与 Asio awaitable 也不自动通用。混合时需桥接完成回调，或明确运行在独立循环并投递结果。是否引入第二套事件循环，需要通过实际工程验证其收益。

若服务仅面向本机，可以先提供本地 HTTP；这不影响出站客户端使用 HTTPS。需要对外提供 HTTPS 时，再分别配置服务端证书与客户端 TLS 后端。

## PhotonLibOS CppServer 与 IXWebSocket

以下为 PhotonLibOS、CppServer、IXWebSocket 的源码评估，尚未进行 Windows 编译、实际平台请求或性能测试。它们保留为研究参考，当前主方案仍为 cpp-httplib，备选为 Drogon。

| 维度 | PhotonLibOS | CppServer | IXWebSocket |
| --- | --- | --- | --- |
| HTTP Server 与 Client | 都有，另提供流式消息、代理和文件 I/O 抽象 | 都有，基于 Asio 的 TCP、TLS 与 HTTP 封装 | 都有，核心定位为 WebSocket，HTTP 功能测试较少 |
| 并发模型 | 有栈协程与事件引擎；另有 C++20 协程辅助接口 | Asio 异步读写，主要通过回调和 future 接口使用 | HTTP 异步请求交给后台线程；Server 每连接一个线程 |
| Windows | 主分支已有 MinGW x64 工具链、兼容层和 AFD 配合 IOCP 实现；README 的平台列表仍只列 Linux、macOS | 官方列出 Windows，并提供 Visual Studio 等构建路径 | 官方列出 Windows |
| 自带 HTTPS 后端 | OpenSSL | OpenSSL，当前主构建配置要求该依赖 | Windows 可用 Mbed TLS 或 OpenSSL 等；Apple 另有原生后端 |
| 内置 Schannel | 未提供 | 未提供 | 未提供，旧的未实现占位后端早已移除 |
| 音频流式下载 | 响应可通过流接口分块读取，适合进一步验证 | 常规 HTTPResponse 累积响应体，需扩展或另配下载客户端 | 有 onChunkCallback、进度和取消接口，支持不累积完整响应体 |
| 当前用途判断 | 有能力覆盖，但 Windows 工具链与运行时适配成本需先验证，暂不优先 | 可作为补充候选，HTTP 客户端完备性与下载能力是重点 | 可用于 WebSocket 或较低并发 HTTP，当前不优先作为统一主框架 |

### PhotonLibOS

Photon 的主模型是有栈协程：调用网络和文件接口时以接近顺序代码的方式编写逻辑，由运行时在等待 I/O 时切换协程。它不要求业务全部改成 `co_await`，仓库也提供 `thread/coro20.h` 辅助接口，因此不能简单归类为不支持 C++20。[运行时说明](https://github.com/alibaba/PhotonLibOS/blob/240cf60f69ad8084a846507abb266366c643ed70/README.md)、[C++20 接口](https://github.com/alibaba/PhotonLibOS/blob/240cf60f69ad8084a846507abb266366c643ed70/thread/coro20.h)

Windows 状态需要同时看文档和代码。README 仍将官方平台组合列为 Linux、macOS 与 GCC、Clang，但主分支已经存在 Windows MinGW x64 交叉编译工具链、Windows 兼容层及 AFD 配合 IOCP 的事件引擎。因此不能说完全没有 Windows 实现，也不能由这些文件直接认定 MSVC 和正式发布环境已经验证通过。若继续评估，先完成目标工具链下的 HTTP、HTTPS、文件下载和退出测试。[MinGW 工具链](https://github.com/alibaba/PhotonLibOS/blob/240cf60f69ad8084a846507abb266366c643ed70/CMake/toolchain-windows-mingw64.cmake)、[Windows 事件引擎](https://github.com/alibaba/PhotonLibOS/blob/240cf60f69ad8084a846507abb266366c643ed70/io/iocp.cpp)

HTTP 消息本身提供 `read`、`write` 和流式传输接口，适合分块接收音频。原生 HTTP 实现和 curl 模块是分开的，`PHOTON_ENABLE_LIBCURL` 默认打开，可在不用该模块时评估关闭；自带 TLS 模块仍依赖 OpenSSL，没有因此切换到 Schannel。[消息流接口](https://github.com/alibaba/PhotonLibOS/blob/240cf60f69ad8084a846507abb266366c643ed70/net/http/message.h)、[模块与依赖配置](https://github.com/alibaba/PhotonLibOS/blob/240cf60f69ad8084a846507abb266366c643ed70/CMakeLists.txt)

如果主要部署在 Linux 并同时重视网络与文件 I/O，Photon 值得进一步验证。按当前跨平台覆盖、精简依赖和 HTTP 应用开发的要求，暂不进入 cpp-httplib 主方案与 Drogon 备选之外的实施路线。

### CppServer

CppServer 提供 HTTP、HTTPS 的 Server 与 Client，基于 Asio 异步读写，可以在 C++20 工程中作为网络组件评估。公共接口主要通过继承 Session、Client 并处理回调使用；扩展客户端返回 `std::future`，这不自动等于可直接 `co_await` 的任务。API 路由、JSON 转换和业务层组织需要自行补充。[官方说明](https://github.com/chronoxor/CppServer/blob/00caf5cf93d5656c598649ce77ba7981eb170779/README.md)、[HTTP Client 接口](https://github.com/chronoxor/CppServer/blob/00caf5cf93d5656c598649ce77ba7981eb170779/include/server/http/http_client.h)

其主构建直接要求 OpenSSL，并链接 CppCommon 和 Asio；当前没有可直接启用的 Schannel 替代配置。依赖数量和发布包是否更小需要实际测量。[构建配置](https://github.com/chronoxor/CppServer/blob/00caf5cf93d5656c598649ce77ba7981eb170779/CMakeLists.txt)

HTTP 响应体持续追加到内部缓存，常规 Client 在响应体完成后通知业务，不适合直接承载多个大型无损音频的内存缓冲。已检查的 `ReceiveBody` 主要按 Content-Length 或尾部条件判断完成，没有通用 chunked 解码状态机；分块传输、压缩解码、重定向与 Cookie 管理不能按通用下载客户端的能力默认成立。[Client 实现](https://github.com/chronoxor/CppServer/blob/00caf5cf93d5656c598649ce77ba7981eb170779/source/server/http/http_client.cpp)、[响应体解析](https://github.com/chronoxor/CppServer/blob/00caf5cf93d5656c598649ce77ba7981eb170779/source/server/http/http_response.cpp#L588)

可把它作为偏底层的异步 Server 补充候选，出站请求和下载另配成熟客户端。若希望一个框架直接覆盖 API 组织、平台请求和下载，仍需比较额外实现成本。

### IXWebSocket

IXWebSocket 支持 Windows，也提供 HTTP Client 与 Server。Windows TLS 可选择 Mbed TLS 或 OpenSSL，能够评估减少 OpenSSL 依赖，但不是使用系统 Schannel。更新记录明确记载旧 Schannel 占位实现已移除。[TLS 构建配置](https://github.com/machinezone/IXWebSocket/blob/e8c76ecdbcb55de3582db9e9b7372258ca1c824d/CMakeLists.txt)、[变更记录](https://github.com/machinezone/IXWebSocket/blob/e8c76ecdbcb55de3582db9e9b7372258ca1c824d/docs/CHANGELOG.md)

底层 socket 可以非阻塞，但 HTTP Async 接口采用一个 HttpClient 的后台线程从队列取出请求，再逐个执行；同一实例的一次长下载会占用该工作线程。Server 为每个连接创建线程。它可让调用方快速返回，却与少量共享事件循环承载大量并发请求的设计不同。[HTTP 调度实现](https://github.com/machinezone/IXWebSocket/blob/e8c76ecdbcb55de3582db9e9b7372258ca1c824d/ixwebsocket/IXHttpClient.cpp#L33)、[并发模型说明](https://github.com/machinezone/IXWebSocket/blob/e8c76ecdbcb55de3582db9e9b7372258ca1c824d/docs/design.md)

它提供 `onChunkCallback`、进度回调和取消标志，适合少量并行的流式文件请求；但续传仍需自行处理资源一致性和 Range 语义。官方 README 明确说明 HTTP 功能没有 WebSocket 部分经过那么多测试。[HTTP 使用说明](https://github.com/machinezone/IXWebSocket/blob/e8c76ecdbcb55de3582db9e9b7372258ca1c824d/docs/usage.md)、[项目定位](https://github.com/machinezone/IXWebSocket/blob/e8c76ecdbcb55de3582db9e9b7372258ca1c824d/README.md)

如果主要需要 WebSocket、较少连接和简单回调接口，可以使用。当前项目主要是平台 HTTP 调用、批量搜索和下载，不因附带 HTTP 能力而优先引入专门的 WebSocket 库。

## libhv

libhv 同时提供非阻塞 HTTP Server、异步 HTTP Client、Windows 支持和流式响应回调。本轮评估保留为研究参考；其自带 Schannel 后端在本轮本地测试中未拒绝未信任的自签名证书，不能直接作为正式出站 HTTPS 方案。当前采用 cpp-httplib 主方案，Drogon 备选。

评估基准：`3c655014fa67d696daa2928596a9cccba7d2d0fe`，2026-10-19。

| 需求 | 能力与判断 |
| --- | --- |
| 异步 Server | 事件循环驱动网络 I/O，提供路由、JSON、请求上下文和异步响应；部分名为 async 的业务处理器在线程池执行 |
| 异步 Client | `HttpClient::sendAsync`、`AsyncHttpClient::send`，同一事件循环管理多个连接；并非逐请求占用一个阻塞线程 |
| Windows | 官方支持 Visual Studio，提供 Windows CI；本轮 MSVC x64 Release 静态库与 curl 示例构建通过 |
| TLS 后端 | OpenSSL、GnuTLS、Mbed TLS，以及平台原生后端；Windows 未选择前三者时使用 Schannel |
| 原生 Schannel | 确实存在，非占位；但默认信任校验存在已复现的问题 |
| 流式下载 | `http_cb` 的 `HP_BODY` 回调接收分块；有回调时 HTTP/1 解析器不将完整正文追加到 body |
| Range 与进度 | 提供 Range 设置、正文回调和示例；持久续传、资源身份校验仍由下载模块完成 |
| 背压与取消 | 低层 Channel 有暂停和恢复读取；高层下载仍需封装有界写盘队列、实际终止与错误传播 |
| C++20 | 核心 C、上层 C++11 回调接口可在 C++20 方案中使用；本轮未验证 C++20 应用接入，也未发现同等定位的原生 co_await HTTP 接口 |
| curl 依赖 | 默认 `WITH_CURL=OFF`，原生 HTTP 实现不依赖 libcurl；示例名为 curl 不表示链接了 libcurl |

### 服务端与客户端执行模型

服务端网络使用事件循环，业务处理器需要按实际类型区分：`http_ctx_handler`、`http_state_handler` 在 I/O 线程运行，`http_async_handler` 则交给 `hv::async` 线程池。平台请求应继续走异步 Client，不在 I/O 线程调用同步 `send()` 或等待 future。[服务处理器定义](https://github.com/ithewei/libhv/blob/3c655014fa67d696daa2928596a9cccba7d2d0fe/http/server/HttpService.h#L38)

客户端将任务投递到 EventLoop，并为不同连接维护 Channel、解析器、超时和连接池，可以同时推进多个请求。`AsyncHttpClient` 可以接收外部 EventLoop；常规 `HttpClient::sendAsync` 首次调用会建立自己的客户端循环线程，仍属于事件驱动网络，而非单线程逐个等待请求完成。[异步客户端接口](https://github.com/ithewei/libhv/blob/3c655014fa67d696daa2928596a9cccba7d2d0fe/http/client/AsyncHttpClient.h)、[连接与读取实现](https://github.com/ithewei/libhv/blob/3c655014fa67d696daa2928596a9cccba7d2d0fe/http/client/AsyncHttpClient.cpp)

Windows 构建与运行路径由[构建说明](https://github.com/ithewei/libhv/blob/3c655014fa67d696daa2928596a9cccba7d2d0fe/BUILD.md#L48)和[Windows CI](https://github.com/ithewei/libhv/blob/3c655014fa67d696daa2928596a9cccba7d2d0fe/.github/workflows/CI.yml)覆盖。当前事件后端按配置使用 wepoll 或 WSAPoll，不将 README 中的 IOCP 标签等同于所有 Windows 构建都直接使用 IOCP。[事件后端选择](https://github.com/ithewei/libhv/blob/3c655014fa67d696daa2928596a9cccba7d2d0fe/event/iowatcher.h)

### TLS 后端与本地验证

README 主要列出 OpenSSL、GnuTLS、Mbed TLS，但 `hssl.h` 在 Windows 未启用这三项时定义 `WITH_WINTLS`，`ssl/wintls.c` 的 `hssl_backend()` 返回 `schannel`，构建链接 Windows 系统安全库。因此这条路径可以不带 OpenSSL 和 libcurl。[后端选择](https://github.com/ithewei/libhv/blob/3c655014fa67d696daa2928596a9cccba7d2d0fe/ssl/hssl.h#L7)、[Schannel 实现](https://github.com/ithewei/libhv/blob/3c655014fa67d696daa2928596a9cccba7d2d0fe/ssl/wintls.c)、[构建选项](https://github.com/ithewei/libhv/blob/3c655014fa67d696daa2928596a9cccba7d2d0fe/CMakeLists.txt)

该版本 Schannel 握手把 `verify_server_cert` 固定为 0，并设置 `ISC_REQ_MANUAL_CRED_VALIDATION`，随后没有完成相应的证书验证流程；`hssl_connect` 中第三步调用也被注释。不能仅凭系统 TLS 后端就推断已经正确校验服务端身份。[握手代码](https://github.com/ithewei/libhv/blob/3c655014fa67d696daa2928596a9cccba7d2d0fe/ssl/wintls.c#L422)

本地验证使用 Windows、CMake 4.4.4、Visual Studio 18 2026、MSVC 19.51、x64 Release。构建选项为 `BUILD_SHARED=OFF`、`BUILD_STATIC=ON`、`BUILD_EXAMPLES=ON`、`WITH_CURL=OFF`、`WITH_OPENSSL=OFF`、`WITH_GNUTLS=OFF`、`WITH_MBEDTLS=OFF`；成功生成静态库及官方 `examples/curl.cpp` 对应的客户端。

TLS 测试服务只监听回环地址，由 .NET SslStream 使用临时生成的 TLS 1.2 自签名证书；证书包含 localhost 域名，未加入信任证书库。测试命令没有关闭证书验证的参数。两次测试使用同样的证书生成和本地服务逻辑：

| 客户端 | 观察结果 | 判断 |
| --- | --- | --- |
| libhv 自带 curl 示例，`-v --timeout 8` | HTTP 200，收到正文，退出码 0 | 未拒绝未信任证书，信任校验测试失败 |
| Windows 系统 curl，`-v --max-time 8` | `SEC_E_UNTRUSTED_ROOT`，退出码 60 | 正确拒绝未信任证书，对照成立 |

该测试验证的是当前版本、默认原生后端和同步客户端路径；异步 HTTPS、过期证书、错误主机名、握手分片和 HTTPS 服务端证书加载还需单独验证。服务端 Schannel 当前从本机证书存储查找证书，与直接加载 PEM 文件的后端不同。当前 ALPN 接口只在 OpenSSL 后端暴露，也不能把 Windows Schannel 路线视为已具备同等 HTTP/2 协商能力。[TLS 接口限制](https://github.com/ithewei/libhv/blob/3c655014fa67d696daa2928596a9cccba7d2d0fe/ssl/hssl.h#L86)

### 流式下载与完整下载器的差距

请求设置 `http_cb` 后，`HP_HEADERS_COMPLETE` 可读取响应头，`HP_BODY` 按块消费数据，`HP_MESSAGE_COMPLETE` 表示消息完成。HTTP/1 解析器在存在回调时不自动累积完整 body，避免无损音频必须整曲缓冲。[解析器](https://github.com/ithewei/libhv/blob/3c655014fa67d696daa2928596a9cccba7d2d0fe/http/Http1Parser.cpp#L75)

官方 wget 示例使用正文回调写盘，也演示 Range 与已有临时文件追加，但示例主要使用同步 `cli.send()`。Range 返回状态、Content-Range、ETag 或资源一致性检查仍需补齐，不能把示例直接当作可靠的持久续传实现。[wget 示例](https://github.com/ithewei/libhv/blob/3c655014fa67d696daa2928596a9cccba7d2d0fe/examples/wget.cpp)

异步客户端同样接收 `http_cb`，但回调在事件循环执行。直接 `fwrite` 或标签写入可能阻塞该循环；改成写盘队列后还需在队列满时限制读取，不能只转移到无限增长的内存队列。Channel 的 `stopRead`、`startRead` 可作为低层基础，高层下载适配器如何取得并管理 Channel 仍需设计。[Channel 接口](https://github.com/ithewei/libhv/blob/3c655014fa67d696daa2928596a9cccba7d2d0fe/evpp/Channel.h#L123)

另一个待验证点是重定向与重试：当前 `sendRequest` 会将请求的 `http_cb` 移入响应；跨连接重新发起任务时，需检查流回调是否保留，避免最终音频回到整曲缓冲。还需确保只保存最终成功响应、取消可及时关闭传输、写盘错误能够停止任务。[回调转移与重发路径](https://github.com/ithewei/libhv/blob/3c655014fa67d696daa2928596a9cccba7d2d0fe/http/client/AsyncHttpClient.cpp#L287)

### 当前采用建议

libhv 暂不进入当前实施路线。后续若重新评估，可比较其 Server 配合独立 curl 的组合；若使用原生客户端，先解决并回归验证 TLS 身份校验、流回调重发和下载取消问题。改用 Mbed TLS 或 OpenSSL 时，也分别验证证书链、主机名及配置是否传入异步连接，不能仅切换编译选项就视为完成。

## 其他讨论过的方案

| 方案 | 当前记录 |
| --- | --- |
| Proxygen | 同属异步 HTTP 基础设施候选，功能和依赖体系较多；Windows、构建体积及 TLS 路径需要专门验证，当前不优先投入 |
| Asio 配合 Beast | 更底层的自行组合路线；需承担更多 HTTP 会话、连接与传输管理 |

这些记录保留比较背景，不改变 cpp-httplib 暂定主方案、Drogon 备选的状态。参考：[Proxygen](https://github.com/facebook/proxygen)、[Boost.Beast](https://www.boost.org/doc/libs/latest/libs/beast/doc/html/index.html)、[libhv](https://github.com/ithewei/libhv)。

## 新候选的比较项目

新增候选至少补充以下信息：

1. Server、Client、代理和流式文件处理分别支持到什么程度。
2. socket I/O 是事件循环驱动，还是线程池包装同步请求。
3. Windows、目标编译器、协程类型、取消机制和退出方式。
4. TLS 后端、证书链与主机名校验、代理和重定向支持。
5. Cookie、请求头、超时、连接复用、Range 和资源恢复能力。
6. 直接及传递依赖、可裁剪模块、静态或动态分发、许可证要求。
7. 相同构建条件下的发布包大小、并发内存、CPU 占用和实现复杂度。

性能与体积以同一机器、编译器、构建类型、功能开关和测试数据比较，不用不同项目 README 的峰值数据直接排序。

## 版本记录

| 项目 | 本轮参考版本 |
| --- | --- |
| cinatra | `cf095d224cb60ae7dfc623fde51c5c60db03c96d` |
| cofetch | `e25a2f6322e2b7866c22fd78364e3888efa60eef` |
| CPR | `cdbbd90fb01c29dd52ed3e19f606bb4980e71541` |
| PhotonLibOS | `240cf60f69ad8084a846507abb266366c643ed70` |
| CppServer | `00caf5cf93d5656c598649ce77ba7981eb170779` |
| IXWebSocket | `e8c76ecdbcb55de3582db9e9b7372258ca1c824d` |
| libhv | `3c655014fa67d696daa2928596a9cccba7d2d0fe`，已进行本机 MSVC 构建与原生 TLS 负面用例验证 |
| cpp-httplib | `74ef3c42188acd6391f027c4337fb0e1a68c0024`，核对阻塞 I/O、TaskQueue 与协程接入边界 |
| Drogon | 2026-10-19 查看主分支；最终验证工程另锁定提交 |
| Drogon 引用的 Trantor | `78e480f`，用于核对 TLS 后端 |
| curl、TagLib | 参考官方文档；正式集成时再锁定具体版本和构建选项 |
