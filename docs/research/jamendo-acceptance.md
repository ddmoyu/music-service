# Jamendo Windows 验收

当前实现为 20 项匿名能力；新增发现能力及音质结论见[第二阶段](#第二阶段发现能力与音质核查)。下方先保留 14 项能力的首阶段验收基线。

2026-10-24 补充：按用户要求确认“不授权、不登录能否获取歌曲和下载”。使用 Release 0.1.25、全新的隔离数据目录、空平台账号与空 app_profiles，歌曲 49292 的详情请求成功，匿名标准 MP3 下载任务 completed。文件 `Lover303 - Track 5 Nudel.mp3` 为 310,500 字节，SHA-256 `652a645bc4d08dcce3f8b4f44d4432256f64f107eb658a4ec8581090c83812b4`；本次 metadata_policy=skip。结果支持保留现有 Jamendo 匿名能力，没有为它增加 OAuth 登录要求。证据：out/oauth-release-smoke-report.json；不代表所有歌曲或其他音质均可匿名下载。

日期：2026-10-23。用户明确要求开始 Jamendo，本轮按 N01 首阶段接入，并加入已在线确认的专辑、歌手和推荐歌单目录。实现入口为 [Jamendo 适配器](../../src/providers/jamendo/README.md)，独立客户端为 [jamendo_e2e.py](../../packaging/e2e/jamendo_e2e.py)。

## 协议依据

当前官网 [客户端脚本](https://www.jamendo.com/Client/scripts/main-build.994137531.js) 和匿名 HTTPS 请求确认网站签名、`id[]` 批量读取、搜索 offset、官方 featured 推荐及集合字段。固定 SHA-1 向量对应 `/api/search` + `0.123456789`，结果为 `19b51f1a66bb9e9c133e97d1a07b795aa57a3ed2`。

官方网站将下载许可与播放区分：[开发者 tracks 文档](https://developer.jamendo.com/v3.0/tracks)说明歌曲可被作者设为不可下载。本适配器实际检查网站详情的 `isDownloadable`，仅使用 `download`，不使用 `stream` 作为备用下载。网站 API 与开发者 `/v3.0` API 是两个协议，不复制参考项目的固定 client_id。

## 验证状态

| 项目 | 实测结果 |
| --- | --- |
| Windows 构建 | MSVC x64 Debug 构建通过，Jamendo 项目代码无编译警告 |
| 全量回归 | 对增量重建后的同一份二进制运行 CTest，共 442 项：436 项通过、6 项环境门控测试跳过、0 项失败；耗时 113.12 秒 |
| Jamendo 离线回归 | 20 项用例、775 个断言通过；覆盖签名向量、身份校验、三类搜索、空结果、分页、重复／缺失位置、修订变化、限流／取消、下载许可、品质与地址边界 |
| 三类搜索 | 歌曲和专辑用 `love`、歌手用 `rock`，各验证三页；每页 2 项，分页前进且总数保持未知。中文 `周杰伦` 返回合法空结果。`love` 的歌手结果不足三页，服务正常结束分页，未用空占位制造下一页 |
| 推荐与集合 | 官方 featured 推荐验证两页；推荐歌单 `500608490` 详情和 ID 一致，完整遍历 2 页／84 首。专辑 `5987` 完整遍历 4 页／10 首，均无缺失位置；歌手 `6934` 详情与专辑关联一致 |
| 标准下载 | 从搜索结果取得 `49292`，原生 MP3 下载完成；公共音频验证的完整时长为 16,692 ms。发布文件 291,188 字节，SHA-256 `0dcefec2ebc4b45e5a237bd0022b3711fe3a9e8a3e6aefab5bb53a6f76b835f7` |
| 元数据与封面 | 标题、歌手、专辑、真实曲号 `5/10` 和封面写入并读取 ID3 帧核对。兼容原文件保留的合法 ID3 字符编码。独立封面 SHA-256 `bc34526063d72cc74e4c36af04c18f2ea4365d0932077301dee35b83dfd5dc8b` |
| 复用与重启 | 完成结果复用同一任务；进程重启后任务仍完成、同一幂等键回放一致，音频和封面均按 SHA-256 回读一致 |
| 品质与权限 | 真实严格 lossless 返回 `QUALITY_UNAVAILABLE`，显式 allow_lower 返回 standard。不可下载、缺失许可、仅有 stream 等失败分支由离线响应回归验证；未宣称高品质或无损可用 |
| 歌词与能力边界 | 样本歌词为空且正确返回 unavailable；有歌词的读取／文本保真仅做离线验证，独立 LRC 暂无在线阳性样本。未接入的歌单搜索、榜单和歌手全量曲目通过公共 HTTP 返回明确不支持 |

成功在线验收没有网络重试。生产服务全程为原生 C++，Python 仅承担外部请求、ID3 回读和报告生成。

最终日志：`out/jamendo-build-final.log`、`out/jamendo-unit.log`、`out/jamendo-ctest-final.log`、`out/jamendo-live-final.log`、`out/jamendo-live-final-report.json`。本轮同一工作区存在其他平台的并行修改；首次全量回归的一个酷我用例发现实现对象早于测试修改，增量重建后全部已运行用例通过，Jamendo 在线验收也对重建后的二进制完整复跑通过。

## 范围

14 项匿名能力：三类搜索、单曲详情、歌词、链接、标准下载，专辑详情／曲目，歌手详情／专辑，以及官方推荐歌单、歌单详情／曲目。集合按原始位置统计缺失和重复。未注册歌单搜索、分类、榜单、歌手全量歌曲、登录、个人目录和更高音质，不把参考函数或仅有流地址记成完成。

独立构建目录 `out/build/jamendo-msvc-debug` 选择既有酷我、5sing、网易云、QQ 与本轮 Jamendo，避免同一工作区其他正在实现的平台改变本轮验证范围。它只复用原构建的已固定版本依赖源码，生成文件与二进制独立。测试日志和真实报告位于 Git 忽略的 `out/`。

## 第二阶段发现能力与音质核查

日期：2026-10-23，Jamendo 协议版本 `0.1.15`。用户确认采用匿名方案：搜索官方精选歌单，并接入原生歌曲分类。新增 6 项能力后共 20 项；不需要外部开发者 client_id。

### 协议核实与实现

- 官网搜索客户端只查询 track／album／artist；参考实现的 `type=playlist` 对 `love`、`rock` 实测为空，不能作为歌单搜索阳性证明。本实现读取 `/api/playlists/featured` 的完整有界目录，对名称和简介筛选，能力标记 `catalog_kind=search_derived`。每批 100 项、最多 1,000 项，超限、重复页或上游失败都明确失败；不返回截断后伪装完整的结果。当前目录实测 156 张歌单，`best of` 匹配 54 张。`total=null`，`listed_total` 仅表示精选范围内匹配数。
- `/api/tags` 返回风格、情绪、主题，当前共 26 个；`Category.lists=track`。新增公共能力 `category_tracks` 及 `GET /api/v1/categories/tracks?source=jamendo&category_id=23`，默认 20、上限 50，沿用鉴权、缓存、游标、错误契约。歌曲按官网 `tagId` 和 `order=hotness` 查询；实测样本的原生 tags 包含 rock 的 ID 23。此接口的歌曲详情不带 `type`，已用真实裁剪响应回归这一差异。
- 榜单来自 `/api/charts/track`：Best Of、Trending 与 20 个风格 Best Of，共 22 个入口。ID 为 `bestof`、`trending`、`genre:<标签ID>`；各榜明确限定官方前 100 项。读取有序身份清单后只补当前页详情，保留顺序和缺失位置，游标绑定顺序摘要；榜单变化返回 `CURSOR_EXPIRED`。总数和更新时间保持未知，不声称是全部曲目或每日更新。
- 全站用户歌单搜索需要另行接入[官方开发者 playlists API](https://developer.jamendo.com/v3.0/playlists)及自有 client_id，本轮按用户选择不启用。原生分类返回歌曲，`category_playlists` 继续明确不支持。

### 音质结论

独立核查 6 首歌曲（`49292`、`231`、`1879171`、`1765024`、`2330946`、`2330945`），允许下载的字段均只有 `mp3`／`ogg`；MP3 地址为 `mp32`。在确认 `isDownloadable=1` 后，完整读取两首平台明确给出的 MP3，再由本机 FFprobe 独立测量：

| 歌曲 ID | 编码 | 采样率／声道 | 音频平均码率 | 原文件字节数 | 原文件 SHA-256 |
| --- | --- | --- | --- | --- | --- |
| 49292 | MP3 VBR | 44,100 Hz／2 | 122,276 bps | 310,500 | `652a645bc4d08dcce3f8b4f44d4432256f64f107eb658a4ec8581090c83812b4` |
| 1879171 | MP3 VBR | 44,100 Hz／2 | 201,204 bps | 6,248,448 | `2e1566c214dcf215956411e5db7e306dedc5a6cfa70cfb9316414726ab930531` |

`mp32` 不是 320 kbps 的保证，不提升为 `high`；OGG 扩展名也不足以证明无损。匿名网站响应未提供独立高品质或 FLAC 下载，因此服务继续只声明 standard。真实公共 API 对 high／lossless／hires 的严格请求均返回 `QUALITY_UNAVAILABLE`，显式 allow_lower 才返回 standard。

[官方开发者 tracks 文档](https://developer.jamendo.com/v3.0/tracks)列出 `audioformat`／`audiodlformat=flac`，且要求应用 client_id；这说明平台另有文档化 FLAC 途径，**不代表当前匿名网站端点已能取得 FLAC**。本轮没有用户自有开发者密钥，未验证该路径，也未复制参考项目的固定密钥或将播放流改成下载链接。

FFprobe 只用于独立验收，不进入生产依赖。原 MP3 自带标签，原文件 SHA 与服务补写标签后文件 SHA 不同属正常；最终发布文件仍经过服务内原生音频与完整时长验证。

### 第二阶段验证记录

首轮在线验收发现 `/api/tracks` 原生详情缺少搜索项的 `type` 字段，已修正解析并加入真实响应回归；后续核对又发现公共搜索转换与 HTTP 层遗漏 `listed_total`／`unavailable_count`，现已沿用列表序列化保留这两项，并覆盖普通响应及缓存命中。

| 验证 | 最终结果 |
| --- | --- |
| Windows 构建 | MSVC x64 Debug 成功，Jamendo 无编译警告；独立构建目录保留五平台选择 |
| Jamendo 离线用例 | 30 项、1,464 个断言通过，含实际缺少 type 的分类响应、精选目录跨页与重复页、未知分类、榜单身份／顺序／缺失详情和游标失效 |
| 公共集合与搜索 HTTP | 4 项、302 个断言通过，含分类歌曲鉴权、上限、跨实体／跨能力游标拒绝，以及搜索计数的普通和缓存响应 |
| 在线验收 | 最终分页修正后的完整复测通过：精选搜索三页、分类歌曲三页、三种榜单各两页，以及标准下载、标签、封面、文件复用和重启回读；报告状态为 passed |

本轮全量回归中，`tests/integration/server_test.cpp` 的 OAuth 凭据请求仍期望 `CAPABILITY_UNSUPPORTED`，当前工作区并行授权改动已取消只支持 Cookie 的分支，实际返回 `VALIDATION_FAILED`；单独复测相同。后一次全量扫描又纳入并行新增的 6 项应用令牌测试，身份构造均在 `test_support.hpp` 的 unwrap 断言失败；另有传输取消测试 `held_after_stop` 实测为 0、预期为 1。这次 459 项扫描为 445 通过、6 跳过、8 失败，耗时 175.32 秒。Jamendo 没有新增授权依赖，未将这次全仓扫描标为通过。

最终重建后串行复测上述 8 项，OAuth 接口和传输取消两项已通过；**仍有 6 项并行新增的应用令牌测试失败**。记录在 `out/jamendo-phase2-global-recheck.log`；没有据此将全仓回归改写为全部通过。

最终证据位置：`out/jamendo-phase2-build-complete.log`、`out/jamendo-phase2-unit-final.log`、`out/jamendo-phase2-collections-final.log`、`out/jamendo-phase2-ctest-final.log`、`out/jamendo-phase2-global-recheck.log`、`out/jamendo-phase2-final-report.json`、`out/jamendo-phase2-quality-report.json`；原始协议裁剪证据位于 `tests/fixtures/providers/jamendo/`。验收范围仍为 Windows MSVC x64 Debug 与独立五平台构建，未做 Release、安装包、系统服务或其他操作系统验收。
