# 第三方前端接入

后端以独立程序或容器运行，前端通过 HTTP/JSON 操作、SSE 接收任务进度、媒体 URL 播放或保存文件。
本目录与示例随每个平台发行包交付，也提供单独的 frontend-integration.zip。无需安装 C++ 工具链或新增 JS 依赖。

## 选择部署方式

- 远程服务：在服务器部署发行包或容器；前端使用配置的 HTTPS 服务地址和用户自己的 Token。
- 桌面应用：携带对应系统和架构的完整后端包，由启动器运行。监听端口可为 0；读取 stdout 的一行 ready JSON 获取实际地址，日志在 stderr。只用返回地址，不推算相邻端口。需要绑定启动器生命周期时传 --parent-pid。
- 浏览器页面：由 HTTP/HTTPS 托管，配置准确的 CORS origin；不直接双击 file:// 页面。HTTPS 页面优先连接同域 HTTPS 反向代理，浏览器对访问本机/私网服务还有自身权限限制。

程序包和镜像的具体下载状态以 [Releases](https://github.com/ddmoyu/music-service/releases) 与工作流为准。
Actions 构建环境不会替用户长期运行服务。尚未发布的版本可在成功的 CI 中下载保存 14 天的包。

## 本机运行示例

将以下 JSON 保存为 config.json，创建同目录的 downloads 文件夹。相对路径相对于配置文件目录。
这里启用在线媒体代理以支持浏览器试听，直链仍关闭。端口 0 仅用于本机；远程部署需固定监听及 public_urls。

~~~json
{
  "config_version": 1,
  "data_dir": "./data",
  "control": {"host": "127.0.0.1", "port": 0},
  "media": {"enabled": true, "host": "127.0.0.1", "port": 0},
  "events": {"enabled": true, "host": "127.0.0.1", "port": 0},
  "cors": {"allowed_origins": ["http://127.0.0.1:5173"]},
  "features": {"file_access": true, "media_proxy": true, "direct_media_urls": false},
  "destinations": [
    {"id": "dst_default", "name": "下载", "root": "./downloads", "principals": ["prn_local"]}
  ]
}
~~~

从包的 bin 目录运行（Windows 使用 music-service.exe）：

~~~text
music-service --config /absolute/path/config.json check-config
music-service --config /absolute/path/config.json init
music-service --config /absolute/path/config.json serve
~~~

init 将首次访问 Token 写入 data/secrets/bootstrap-token.txt，只在首次初始化时创建。
桌面启动器可以读取这个本机文件，通过自己的受控通道交给前端；远程用户由服务管理员分配身份和 Token。
不把 Token 编译进网页、提交到仓库或拼入 URL，示例只在内存持有它。
服务 Token 与音乐平台 Cookie 是不同凭证；需要平台账号时使用
[账号 API](https://github.com/ddmoyu/music-service/blob/main/docs/account-management.md)。

启动器逐行读取 stdout，等到以下记录再交付地址；退出前没有 ready 视为启动失败，记录 stderr。
接入包不负责启动或下载可执行程序，宿主应用应为启动等待设置超时并正常收尾子进程。

~~~json
{"event":"ready","instance_id":"inst_example","api_base_url":"http://127.0.0.1:43001","media_base_url":"http://127.0.0.1:43005","events_base_url":"http://127.0.0.1:43009"}
~~~

## 核心协议与可运行示例

- [核心 OpenAPI 3.1 描述](api/openapi.json)：能力发现、四类搜索、歌曲详情、解析/换源、下载/批次、文件/媒体与 SSE；可导入已有 API 工具。账号、OAuth、管理和完整目录接口仍见[详细契约](https://github.com/ddmoyu/music-service/blob/main/docs/design/cpp20-refactor/backend-service/api-contract.md)。
- [JS 客户端](../examples/frontend/client.mjs)：原生 fetch；返回完整信封，包含 meta.partial/source_results。
- [TypeScript 类型](../examples/frontend/client.d.mts)：与 .mjs 放在一起导入；不要求 TS 编译器才能使用。
- [调用示例](../examples/frontend/usage.mjs)：搜索、播放、提交下载和进度视图。现代浏览器可直接作为 ES module 引入；Node.js 22+ 可验证 HTTP 与 SSE，播放示例需要浏览器 audio 元素。

~~~javascript
import { MusicServiceClient } from './examples/frontend/client.mjs';

const client = new MusicServiceClient(serviceAddress, userToken);
const info = await client.info();
if (info.data.version !== expectedBackendVersion) {
  throw new Error('请使用此客户端配套的后端版本');
}
const providers = await client.request('/api/v1/providers');
const destinations = await client.request('/api/v1/destinations');
const result = await client.search({ q: '歌曲名称', sources: 'kuwo', limit: 20 });
// 平台与能力以 providers 为准；示例来源不表示始终可用。
// 显示 result.meta.partial 和逐源失败，不能把超时显示成“没有歌曲”。
~~~

前端从 info.features 判断功能，从 providers 判断具体平台能力；服务返回的媒体、事件地址可能在不同端口，
也可能通过反向代理合并到同一 origin。客户端始终使用这些地址。远程部署的代理须同时路由
/api/v1、/media/v1、/events/v1 与健康路径；SSE 关闭缓冲和压缩，媒体保留 Range/条件请求。
CORS 按 scheme、主机和端口精确配置，localhost 与 127.0.0.1 是两个 origin；生产环境不要保留开发 origin。

## 搜索、播放和下载

1. 搜索保留完整 TrackRef 与 context_ref。分页游标属于各来源，读取 meta.source_results；不要把它当作全局页码。
2. 试听先 POST assets/resolve，得到 asset_id，再 POST media-access 请求 mode=proxy、auth=ticket。
   将返回的 url 赋给 audio.src；它是资源专用短期票据，浏览器无需添加服务 Bearer。按 expires_at 刷新访问地址。
   需要后端 features.media_proxy=true。解码支持由浏览器决定；播放失败可显式调用 switch-source，
   始终保留原始 TrackRef 和累积 attempted_sources，显示 actual_track/actual_quality。
3. 下载点击时生成一次 crypto.randomUUID()，传作 Idempotency-Key。网络不确定时用原键和原 body 重发；
   不要每次 HTTP 重试都生成新键。响应 202 是已受理，最终成功由任务 completed 判断。
4. 取消调用 POST downloads/{task_id}/cancel 并等待终态。关闭视图、断开 SSE、AbortController.abort()
   只停止前端请求，不取消后台任务。手动 retry 是新任务，使用新 Idempotency-Key。
5. completed 表示文件已存到后端。取 result.file_id 后请求 file 的 media-access，
   浏览器再读取该 URL 保存到自己的设备；这是另一次传输，不等于后端下载任务的进度。

## SSE 与轮询

watchDownloads 使用 Bearer fetch 流式订阅，不使用无法设置 Authorization 的原生 EventSource，也不把长期 Token 放入查询参数。
解析器处理网络分块、UTF-8、CR/LF 与多行 data，单个事件有大小上限。
收到 stream.ready 后先同步 HTTP 快照，再按 task.version 合并 task.updated；每次重连重复同步。
SSE 是精简投影，收到终态后再 GET downloads/{task_id} 获取完整 result/error。

示例连接空闲 45 秒会重连，退避从约 1 秒到 30 秒，优先遵守 Retry-After，一轮最多重试 8 次；
连接持续 30 秒后重置次数。401/403 停止自动重试，调用方处理凭证；回调异常会退出订阅。
向 watchDownloads 传 AbortSignal，页面卸载或切换连接时取消并等待返回，不重复叠加订阅。
usage.mjs 的视图最多缓存 10000 个任务；更大历史使用分页/筛选视图。

info.features.progress_sse=false 或订阅不可用时使用条件查询：活动任务约每秒一次、后台列表约每 5 秒一次，
终态停止；保存 ETag，发送 If-None-Match，304 保留已有状态。不要同时持续轮询和订阅同一任务。
错误使用 HTTP 状态、error.code、meta.request_id 定位，429/503 遵守 Retry-After，避免无限立即重试。

## 版本和验证

此接入包按标注版本的 API 生成，前端记录已经验收的后端版本及配套接入包。
升级前查阅对应发行说明；不要把 info.api_version=v1 当作全部功能和数据格式均相同的依据，
也不要因接入包更新自行删除用户数据。本次发布流程不改变数据库、配置或任务快照格式。

从仓库运行：

~~~text
python -I packaging/generate_openapi.py
python -I packaging/validate_integration.py
node --test examples/frontend/client.test.mjs
node packaging/frontend_smoke.mjs /absolute/path/music-service
~~~

前两项验证生成结果、引用和路由；JS 测试验证分块、错误、取消及边界；
最后一项启动独立临时实例验证真实 HTTP/CORS/SSE，不访问在线音乐源、不读取用户已有数据。
浏览器播放兼容性和真实音乐源可用性需在目标前端单独验收。
