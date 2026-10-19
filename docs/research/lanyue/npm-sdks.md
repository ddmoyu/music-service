# Lanyue 搜索使用的 NPM SDK

整理日期：2026-10-19。应用基准仍为 `53309e73c73316b7dd2ce721be4d2b0aede327a3`。

Lanyue 的网易云、QQ 搜索分别依赖 `NeteaseCloudMusicApi` 和 `qq-music-api`。这两者封装平台请求、凭证和响应解析；多平台聚合与歌曲模型转换仍由 Lanyue 完成，其余六个平台的搜索不使用这两个 SDK。

## 版本与源码来源

| 平台 | 锁定的包版本 | 项目入口 | 本次实际分析对象 |
| --- | --- | --- | --- |
| 网易云 | `NeteaseCloudMusicApi@4.32.0` | [原 GitHub 项目](https://github.com/Binaryify/NeteaseCloudMusicApi)，该仓库已归档 | [4.32.0 发布包][ncm-tar] |
| QQ | `qq-music-api@1.1.2` | [jsososo/QQMusicApi](https://github.com/jsososo/QQMusicApi) | [1.1.2 发布包][qq-tar] |

版本依据是 Lanyue 的[锁文件][lanyue-lock]，不是今天安装浮动版本得到的结果。本次下载发布包后，均通过锁文件记录的 SHA-512 integrity 校验；只解包阅读，没有安装依赖或执行包脚本。

NPM 元数据记录的 `gitHead` 分别为 `4d63e5562199115915f2ee855460ef8999873b53` 和 `323917cdacab6ba945fbaef009ffdff242f64932`。不能仅凭这些值断定当前 GitHub 默认分支与发布包完全相同，本文以发布包文件为准；网易云原仓库当前也不再展示这些实现文件。

## 网易云：搜索模块加通用加密请求器

Lanyue 在 `src/api/platforms/netease.js` 中调用：[应用适配器][lanyue-ncm]

```javascript
ncm.search({
  keywords: keyword,
  limit: 30,
  offset: (page - 1) * 30,
  ...(cookie ? { cookie } : {}),
})
```

发布包内的调用链是：

```text
main.js：扫描 module/，导出 search 等函数，转换 Cookie
  → module/search.js：关键词、类型、分页转换
  → util/option.js：请求选项
  → util/request.js：Cookie、客户端信息、加密方式、POST
  → util/crypto.js：协议加密
  → Axios → 网易云接口
  → {status, body, cookie}
  → Lanyue 提取 body.result.songs 并转换歌曲模型
```

### 搜索参数和默认协议

`module/search.js` 把参数转换为 `s`、`type`、`limit`、`offset`，交给逻辑路径 `/api/search/get`。类型 `1` 是单曲，`10` 是专辑，`100` 是歌手，`1000` 是歌单；另有特殊类型分支。

**4.32.0 的这条默认路径使用 eapi。** 判断依据是：Lanyue 未传 `crypto` → `createOption()` 产出空字符串 → 请求器根据 `APP_CONF.encrypt=true` 选择 eapi → 默认请求到 `https://interface.music.163.com/eapi/search/get`。

不能依据旧版本经验直接把它写成 weapi。SDK 同时保留 weapi、linuxapi、api 等分支，不代表这次搜索全部都会使用。

### eapi 做了什么

`util/crypto.js` 中的 eapi 流程为：

1. 将包含客户端头信息的参数对象序列化为 JSON。
2. 根据逻辑 URL、JSON 文本和固定协议字符串生成 MD5 摘要。
3. 将 URL、参数文本和摘要按协议格式拼接。
4. 使用 AES-128-ECB、PKCS#7 填充加密，输出大写十六进制。
5. 生成 `params` 字段，按 URL 编码表单 POST；解析响应为 SDK 结果。

请求器还处理 Cookie、UA、客户端版本、代理、返回 Cookie 和可选加密响应。默认 `encryptResponse=false`，不能把请求加密与响应加密混为一谈。

weapi 的备用分支使用双层 AES-CBC 和 RSA 处理随机密钥；这些不是上述默认搜索路径的必需步骤。实际原生迁移应按所用端点选择算法。

SDK 的 `song_url()` 则在 `module/song_url.js` 中组装歌曲 ID 列表与码率，调用 `/api/song/enhance/player/url`，再由同一请求器处理。它获取音源地址，不负责 Lanyue 的任务队列和文件落盘。

## QQ：复用路由函数，Axios 请求搜索接口

Lanyue 在 `src/api/platforms/qq.js` 中调用：[应用适配器][lanyue-qq]

```javascript
qqMusic.api('search', {
  key: keyword,
  pageNo: page,
  pageSize: 30,
  t: 0,
})
```

发布包内的调用链是：

```text
node/index.js：api('search') 查找路由，构造 req/res 对象
  → routes/search.js：读取搜索类型与分页，查询内存缓存
  → util/request.js：组装 GET 查询、Cookie 和 Referer
  → Axios → QQ 搜索接口
  → JSON／JSONP 响应解析
  → routes/search.js：整理 list、total、pageNo 等
  → res.send → SDK Promise 返回 data
  → Lanyue 把 list 转成统一歌曲数组
```

这里构造的 `req/res` 是内存对象，用来复用路由函数；**直接调用 SDK 不需要先启动一个 Express HTTP Server**。[SDK 入口][qq-node]

### 单曲搜索请求

`t=0` 时，1.1.2 发布包使用 `http://c.y.qq.com/soso/fcgi-bin/client_search_cp`，以 GET 传递 `w=关键词`、`n=每页数量`、`p=页码`、`t=0`、`format=json`、`g_tk=5381` 等参数，并设置 Referer。[关联提交中的搜索实现][qq-search]

该搜索路径没有调用包里的 `util/sign.js`；存在签名工具不表示每个接口都要签名。其他取流、登录或刷新路径应分别研究，不能把单曲搜索请求当成全套 QQ 协议。

`util/request.js` 用 Axios 发请求，必要时去掉响应中的 JSONP 包装后解析 JSON。搜索路由根据类型挑出结果，再规范化列表、总数和页码。包里还有内存缓存，但不能把某个缓存参数直接当成网络协议要求。[请求器][qq-request]

### 歌单、专辑搜索与 Lanyue 的使用范围

| SDK 类型 | 对应功能 | Lanyue 当前情况 |
| --- | --- | --- |
| `t=0` | 单曲 | 普通 QQ 搜索实际使用 |
| `t=2` | 歌单 | SDK 切换至 `client_music_search_songlist`；Lanyue 未接入通用歌单搜索入口 |
| `t=8` | 专辑 | SDK 有此类型；Lanyue 专辑搜索实际采用 smartbox 加歌手专辑列表 |
| `t=9` | 歌手 | SDK 有相应类型，具体应用调用见原拆解 |

SDK 暴露类型不保证对应平台端点现在仍然可用。扩展 C++ 歌单搜索可以参考这些请求，但仍要分别验证分页、响应与实际可用性。

### Cookie 核验：修正应用注释造成的误判

`node/index.js` 的 `setCookie()` 更新 SDK 单例；`api()` 构造 `req.query.ownCookie=1` 和 `req.cookies=this.cookie`。`util/request.js` 检查 ownCookie 后取 req.cookies，序列化到出站 `Cookie` 请求头。[SDK 入口][qq-node]、[请求器][qq-request]

因此，**锁定的 1.1.2 客户端模式具备发送 Cookie 的路径**。Lanyue 网关中“SDK 客户端模式没有 Cookie”的相关注释不符合该发布包实现，不能继续作为结论依据。Cookie 被发送仍不等于必定拥有某首歌或私有歌单的权限。

SDK 以单例持有 Cookie，这一点在多用户 C++ 服务中应改为每用户／每请求的显式凭证上下文。

### 异步错误处理的边界

SDK `api()` 直接调用 async 路由函数，没有 await 或把返回的 Promise 接入外层 reject；同步 try/catch 捕获不到该路由后续的异步异常。Lanyue 因此另加了 `unhandledRejection` 监听尝试兜底。[SDK 入口][qq-node]、[Lanyue 包装][lanyue-qq]

这是源码可以确认的异常传播缺口；不应把应用注释中的“永不崩溃”等描述当成隔离保证。C++ 迁移应让每个任务明确返回结果或错误，不复制这种进程级补丁。

## 对 C++20 实现的意义

两套 SDK 的核心价值是平台协议封装。若做原生实现，可以保留它们的参数映射和响应结构，改用我们自己的 HTTP、JSON、Cookie 与协议加密模块。

- 网易云当前搜索路径需要重点对照 eapi 的 JSON 字节序列、MD5、AES-ECB、填充、十六进制和表单编码；SDK 其他路径另行确认。
- QQ 当前单曲搜索路径较简单，主要是 GET、查询编码、Referer／Cookie、JSON／JSONP 和模型转换；其他功能的签名不能省略研究。
- 这两套搜索均由 Axios 的异步调用实现；迁移至 cpp-httplib 后由有界线程池安排阻塞请求，业务流程仍可复用。
- 原生迁移不要求引入 Node、Express 或 NPM。首版只移植实际需要的协议模块，并用固定请求向量与响应样本核对。

本次只做源码分析，没有在线搜索歌曲或测试这些端点的当前成功率。更完整的业务关系见 [Lanyue 主拆解](README.md)。

[lanyue-lock]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/package-lock.json
[lanyue-ncm]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/api/platforms/netease.js#L21
[lanyue-qq]: https://github.com/maliang04110233-dot/Lanyue/blob/53309e73c73316b7dd2ce721be4d2b0aede327a3/src/api/platforms/qq.js#L205
[ncm-tar]: https://registry.npmjs.org/NeteaseCloudMusicApi/-/NeteaseCloudMusicApi-4.32.0.tgz
[qq-tar]: https://registry.npmjs.org/qq-music-api/-/qq-music-api-1.1.2.tgz
[qq-node]: https://github.com/jsososo/QQMusicApi/blob/323917cdacab6ba945fbaef009ffdff242f64932/node/index.js
[qq-search]: https://github.com/jsososo/QQMusicApi/blob/323917cdacab6ba945fbaef009ffdff242f64932/routes/search.js
[qq-request]: https://github.com/jsososo/QQMusicApi/blob/323917cdacab6ba945fbaef009ffdff242f64932/util/request.js
