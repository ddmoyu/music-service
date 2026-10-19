# C++ 网络库对比与选型排序

2026-10-19 用户暂定 cpp-httplib 为主方案，Drogon 作为备选，其他库保留选型参考。下列排名解释这一选择的背景；CPR、cofetch 单独作为出站客户端比较，不自动引入当前实现。

统计日期：2026-10-19。Stars 采集于北京时间 14:21 左右，提交统计截止北京时间 14:21。范围为用户提供链接的 10 个 C++ 网络库；go-music-dl、music-lib 用作功能与协议参考，不参加 C++ 网络库排名。此前提及但未由用户提供链接的 Asio、Beast 等不在本次统计范围。

## 评价依据

当前使用场景包含 Windows、C++20、前端同时发起多个搜索、平台 HTTP 请求、歌单与榜单接口、音频下载，并希望控制依赖体积。普通并发可以通过线程池实现，原生异步 I/O 和协程是可选收益，不作为准入门槛。

综合排序优先考虑当前场景的接入与维护成本、平台和 TLS 可用性、HTTP 与下载功能，其次参考维护活跃度与社区规模。没有测量发布包体积和统一性能指标，不据此给出精确性能分数。

Stars 反映关注规模，不能证明协议实现完整或不存在缺陷。提交数用于比较近期活动，不能直接等同于质量、响应速度或稳定性。

## Stars 与维护数据

下表按 Stars 降序排列。最近提交与 Release 日期统一使用北京时间；Release 为 GitHub `releases/latest` 指向的版本。

| Stars 排名 | 库 | Stars | 近 30 天提交 | 近 90 天提交 | 最近提交 | 最新 Release 与发布日期 |
| --- | --- | ---: | ---: | ---: | --- | --- |
| 1 | [cpp-httplib](https://github.com/yhirose/cpp-httplib) | 16,914 | 167 | 284 | 2026-10-19 | [v0.62.0](https://github.com/yhirose/cpp-httplib/releases/tag/v0.62.0)，2026-10-19 |
| 2 | [Drogon](https://github.com/drogonframework/drogon) | 14,347 | 26 | 50 | 2026-10-18 | [v1.9.14](https://github.com/drogonframework/drogon/releases/tag/v1.9.14)，2026-10-12 |
| 3 | [Proxygen](https://github.com/facebook/proxygen) | 8,375 | 138 | 361 | 2026-10-19 | [v2026.10.12.00](https://github.com/facebook/proxygen/releases/tag/v2026.10.12.00)，2026-10-12 |
| 4 | [libhv](https://github.com/ithewei/libhv) | 7,563 | 12 | 35 | 2026-10-13 | [v1.3.4](https://github.com/ithewei/libhv/releases/tag/v1.3.4)，2025-10-25 |
| 5 | [CPR](https://github.com/libcpr/cpr) | 7,456 | 12 | 17 | 2026-10-19 | [1.15.1](https://github.com/libcpr/cpr/releases/tag/1.15.1)，2026-10-13 |
| 6 | [cinatra](https://github.com/qicosmos/cinatra) | 2,185 | 2 | 3 | 2026-10-03 | [1.0.0](https://github.com/qicosmos/cinatra/releases/tag/1.0.0)，2026-04-29 |
| 7 | [CppServer](https://github.com/chronoxor/CppServer) | 1,653 | 10 | 12 | 2026-10-18 | [1.0.6.0](https://github.com/chronoxor/CppServer/releases/tag/1.0.6.0)，2026-05-28 |
| 8 | [PhotonLibOS](https://github.com/alibaba/PhotonLibOS) | 1,231 | 18 | 47 | 2026-10-16 | [v0.9.6](https://github.com/alibaba/PhotonLibOS/releases/tag/v0.9.6)，2026-08-21 |
| 9 | [IXWebSocket](https://github.com/machinezone/IXWebSocket) | 799 | 8 | 14 | 2026-10-16 | [v12.1.0](https://github.com/machinezone/IXWebSocket/releases/tag/v12.1.0)，2026-10-15 |
| 10 | [cofetch](https://github.com/SSARCandy/cofetch) | 9 | 2 | 2 | 2026-09-29 | [v0.1.2](https://github.com/SSARCandy/cofetch/releases/tag/v0.1.2)，2026-07-10 |

维护活动按近 30 天提交数排序，数量相同时用近 90 天数量区分：

1. cpp-httplib：167 / 284。
2. Proxygen：138 / 361。
3. Drogon：26 / 50。
4. PhotonLibOS：18 / 47。
5. libhv：12 / 35。
6. CPR：12 / 17。
7. CppServer：10 / 12。
8. IXWebSocket：8 / 14。
9. cinatra：2 / 3。
10. cofetch：2 / 2。

这里统计默认分支第一父链上的提交，包括合并、文档、CI 和依赖更新。没有用 GitHub `updated_at` 或 `pushed_at` 替代提交统计：仓库收到 Star、其他分支推送等行为不等于默认分支代码更新。例如 cinatra 仓库近期有 push，但默认分支最近提交仍是 2026-10-03。

代码活动与版本发布分开看：libhv 最近仍有提交，但最新 GitHub Release 发布于 2025-10-25；cinatra 的近期提交较少，也不能仅据此判断项目已停止维护。

## 功能特点

| 库 | Server 与 Client | 并发与协程 | Windows 与 TLS | 音频下载相关能力 |
| --- | --- | --- | --- | --- |
| cpp-httplib | 都有，接口直接，Header-only | 阻塞 I/O 配合线程池；可并发处理请求，无原生协程 HTTP API | Windows；OpenSSL、Mbed TLS、wolfSSL，没有内置 Schannel TLS 后端 | 有分块接收和流式内容接口；线程、超时、任务与续传策略需控制 |
| Drogon | 都有，应用框架较完整，含路由、控制器、过滤器等 | Trantor 事件循环、回调和 C++20 协程 | Windows；当前对应 Trantor 支持 OpenSSL、Botan，没有内置 Schannel | 常规 Client 偏完整响应，原生大文件分块输出需进一步验证或另配客户端 |
| cinatra | 都有，基于 C++20 的 HTTP 框架 | Asio 与 async_simple 协程体系 | 需验证目标 Windows 配置；已检查 TLS 路径使用 OpenSSL | 有异步下载入口；续传、取消与证书校验路径仍需验证 |
| libhv | 都有，包含路由和多种协议 | 事件驱动网络，Client 回调；部分业务 async handler 使用线程池 | Windows 本机 MSVC 构建通过；多后端，含原生 Schannel | 有不累积整曲的正文回调；背压、重定向与取消需封装；原生 Schannel 信任校验存在已复现问题 |
| CppServer | 都有，偏底层 Session 与 Client 封装 | Asio 异步读写，主要使用回调和 future | Windows 官方支持；主构建依赖 OpenSSL | 常规响应体缓存；chunked、流式下载和通用客户端功能需补充验证 |
| IXWebSocket | 都有，核心定位为 WebSocket | HTTP Client 后台线程队列，Server 每连接线程 | Windows；Mbed TLS、OpenSSL 等，没有内置 Schannel | 有正文分块、进度和取消；官方说明 HTTP 部分测试少于 WebSocket |
| PhotonLibOS | 都有，兼有文件 I/O 与协程运行时 | 有栈协程、事件引擎，另有 C++20 辅助接口 | 主分支已有 MinGW 和 Windows 事件引擎；MSVC 路线需验证；原生 TLS 使用 OpenSSL | 有响应流与文件抽象；需要接入其调度模型 |
| Proxygen | 提供 HTTP Server 与 Client 基础设施 | Folly 事件体系，覆盖 HTTP/2、HTTP/3 等复杂协议场景 | Windows 适配与庞大依赖体系需额外投入；没有直接满足当前偏好的 Schannel 配置 | 传输能力强，工程与会话管理复杂度较高 |
| CPR | 只有 Client | 普通常规请求同步，Async 接口主要在线程池执行 | 继承 libcurl 后端，Windows 可使用 Schannel | libcurl 的传输能力可复用；完整任务调度与恢复仍由业务实现 |
| cofetch | 只有 Client | Asio 驱动 libcurl multi，支持回调和 C++20 awaitable | 继承所链接 libcurl 的 TLS 后端，可评估 Schannel | 多请求异步传输基础合适；流式写盘、取消和封装边界需专项验证 |

这些能力及限制的固定源码依据见[技术栈选型](technology-stack.md)，其中 libhv 已做本机 MSVC 构建和同步原生 TLS 负面用例，其他库并未统一做过同条件编译和性能测试。

## 主框架综合排序

这里对能提供 Server 的 8 个库排序。应用能够同时对外提供接口和请求上游平台即可满足基本架构；线程池方案在合理并发范围内具有实际可用性。

| 排名 | 库 | 当前契合度与选择理由 | 需要接受的成本 |
| --- | --- | --- | --- |
| 1 | cpp-httplib | 暂定主方案。适合先完成本地或小规模音乐服务，接口简单、社区关注高、近期维护活跃；阻塞模型本身不妨碍多个搜索并发 | 规划工作线程、搜索和下载额度、超时与排队；Schannel 需求可由独立客户端满足 |
| 2 | Drogon | 当前备选。若重视完整应用框架、统一异步编排和后续扩展，是更系统的路线 | 框架与依赖接入成本高于简单 HTTP 接口；原生 Client 下载能力需另验 |
| 3 | cinatra | 选型参考。C++20 协程开发体验与原始方向一致，Server、Client 和下载入口集中 | 近期默认分支变动少；Windows 目标组合、TLS 校验及下载行为需要验证 |
| 4 | libhv | 选型参考。Server、Client、Windows 与分块接收覆盖较好，兼顾功能和可裁剪配置 | 当前已检查的原生 Schannel 校验问题影响出站 HTTPS；可评估 Server 配合独立 curl |
| 5 | CppServer | 补充候选。适合希望掌控底层连接、采用 Asio 并自行组织业务的人 | 需要补充更多 HTTP 应用功能与下载封装，未体现出比前四项更省工作量的优势 |
| 6 | IXWebSocket | 专项候选。主要需要 WebSocket、连接数有明确上限时更合适 | 当前业务以平台 HTTP 和音频下载为主，难以充分利用其核心优势 |
| 7 | PhotonLibOS | 条件候选。Linux 和大量网络、文件 I/O 场景值得考虑 | 当前 Windows 工具链、运行时与已有业务的适配成本较高 |
| 8 | Proxygen | 当前低优先级。维护活跃、能力强，适合复杂 HTTP 基础设施 | Windows、依赖和接入成本与当前轻量音乐工具需求不匹配 |

排名第 1 与第 2 的区别主要是工程方向：cpp-httplib 优先减少初期接入成本，Drogon 优先提供完整的异步应用骨架。如果后续明确要求大量持续连接、以 C++20 协程作为公共业务接口，Drogon、cinatra 的排序应上调。

libhv 的问题针对本轮固定提交的原生 Schannel 路径，不是对所有 TLS 后端或后续版本的判断。若选择修复、切换后端或独立客户端，需要重新完成证书与下载验证。

## 纯客户端综合排序

CPR、cofetch 不提供 HTTP Server，不能单独替代上述主框架；它们可以作为出站平台请求或下载模块的实现。

| 排名 | 库 | 当前契合度与选择理由 | 适合上调优先级的条件 |
| --- | --- | --- | --- |
| 1 | CPR | 接口直接，建立在 libcurl 上，社区规模与近期维护活动较多；当前允许线程池并发时，适合快速实现客户端调用 | 重点是简化调用、复用 curl 的 TLS 与传输能力 |
| 2 | cofetch | 原生事件驱动的 curl 封装，协程接口符合异步方向；可作为技术验证对象 | 明确要求 Asio 运行时与非阻塞客户端，并完成流式下载和生命周期验证 |

cofetch 当前 Stars 为 9、近 90 天默认分支第一父链提交为 2。这不能直接证明代码质量差，但意味着公开采用规模和近期变更样本都较小；生产接入时需自行承担更多行为验证。底层 libcurl 的成熟程度不能完全替代对封装层的验证。

如果不需要额外封装，也可以直接采用此前讨论的 libcurl multi。它不属于本次用户发过仓库链接的 10 个项目，因此没有混入 Stars 排名。

## 下一步选择方式

先围绕 cpp-httplib 实现最小搜索、下载场景，验证超时、线程额度和流式传输。出现具体能力缺口或压测调优后仍不满足目标时，再评估 Drogon 备选。cinatra、libhv 等保留研究记录，不要求同时实现多个框架或引入两套网络运行时。

若 Windows Schannel 是硬性要求，优先验证出站 curl 构建与客户端封装，服务端框架仍可独立选择。后端线程池、异步网络与 TLS 后端属于不同决策，不互相替代。

## 数据口径与复核

- Stars：GitHub 仓库公开页面的 `stargazerCount`，采集时间保存在快照中。
- 提交：默认分支的第一父链，按 committer 日期统计，不包含其他分支未合入的提交。包含合并、文档、CI 和依赖更新。
- 近 30 天：`2026-09-19T06:21:00Z` 至 `2026-10-19T06:21:00Z`。
- 近 90 天：`2026-07-21T06:21:00Z` 至 `2026-10-19T06:21:00Z`。
- 最近提交：本轮获取的默认分支 HEAD 的 committer 时间；展示日期转换为北京时间。
- Release：GitHub `releases/latest` 指向的 Release 及页面发布日期，不等同于默认分支最新提交，也不代表每个项目采用相同发布策略。

获取覆盖统计窗口的 Git 历史后，可用以下形式复核；`<HEAD_SHA>` 使用快照中的固定值：

```text
git rev-list --first-parent --count --since=2026-09-19T06:21:00Z --until=2026-10-19T06:21:00Z <HEAD_SHA>
git rev-list --first-parent --count --since=2026-07-21T06:21:00Z --until=2026-10-19T06:21:00Z <HEAD_SHA>
```

提交数已用 `git log` 与 `git rev-list --count` 交叉核对。数据、HEAD 提交、Release 链接与采集时间见[本轮指标快照](library-metrics-2026-10-19.json)。
