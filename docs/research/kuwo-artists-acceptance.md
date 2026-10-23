# 酷我歌手与专辑封面验收

日期：2026-10-23。工作区版本 **0.1.8**（`kuwo-native` 0.1.8）；Windows MSVC x64 Debug，当前工作区包含此前的 QQ 音乐改动。测试由独立临时原生服务执行，未导入网易云、QQ 或酷我账号 Cookie。

## 实现范围与结果

| 功能 | 实现范围 | 验收结果 |
| --- | --- | --- |
| ☑ 歌手搜索 | 返回歌手实体、头像和已知计数，每页 1～50，游标分页 | 搜索周杰伦，连续两页各 2 条，336 身份正确，两页 ID 不重叠 |
| ☑ 歌手详情 | 姓名、头像、简介、别名、地区、生日、出生地、性别、语言、身高、体重、星座和歌曲／专辑／MV 计数 | 336＝周杰伦，1062＝林俊杰；简介分别 31,290／3,661 UTF-8 字节；不存在 ID 返回 404 NOT_FOUND |
| ☑ 歌手歌曲分页 | 每页 1～100，沿上游完整目录继续，保留曲目身份、专辑、时长和曲号 | 336 完整遍历 **18 页、1,708 首**；1,708 个不同 ID，与 total 相同；末页正确结束 |
| ☑ 歌手专辑分页 | 每页 1～50，保留专辑 ID、标题、歌手、发行日期与官方专辑图片 | 336 以每页 10 张遍历 **5 页、45 张**；45 个不同 ID，与 total 相同 |
| ☑ 歌手链接 | 识别官方 `/singer_detail/{id}` 及 `/info`、`/album`、`/mv` | 用户提供的 `/singer_detail/336/info` 经公共 API 正确解析为 artist／336 |
| ☑ 专辑封面 | 单曲 JSON 中有可信专辑身份时使用 `albumpic`，仅官方图片目录，HTTPS | 《晴天》228908 对应专辑 1293《叶惠美》，取得 108,066 字节 JPEG；78932517 无专辑身份，封面保持 null |
| ☑ 嵌入与独立导出 | 原生媒体处理写入 MP3 APIC／FLAC PICTURE，并导出同一图片 | standard、lossless 实际完成；嵌入图片、独立图片和接口原图逐字节一致 |
| ☑ 持久闭环 | 完成结果复用、音频与封面重启回读 | 两档均复用原任务；重启后重新签发票据，音频完整 SHA-256 与封面字节均一致 |

歌曲目录与作品数量是本次上游快照，不保证未来仍为相同数量。可见目录不代表所有作品都有下载权限。

## JSON 接口与约束

用户提供的详情路径为 `/api/www/artist/artist?artistid=1062`。本轮验证 `wapi.kuwo.cn` 可匿名直连同一路径，因此请求统一使用该官方主机；不复用固定 reqId，不依赖 Secret 签名工具。单曲详情也改用 JSON，并移除原有 HTML 解析函数及样本，不设置网页回退。

| 能力 | 上游路径（均为 `https://wapi.kuwo.cn/api/www/`） | 公共 API |
| --- | --- | --- |
| 搜索 | `search/searchArtistBykeyWord?key=...&pn=...&rn=...` | `/api/v1/search/artists?sources=kuwo&q=...` |
| 歌手详情 | `artist/artist?artistid=...` | `/api/v1/artists/detail?source=kuwo&id=...` |
| 歌手歌曲 | `artist/artistMusic?artistid=...&pn=...&rn=...` | `/api/v1/artists/tracks?source=kuwo&id=...` |
| 歌手专辑 | `artist/artistAlbum?artistid=...&pn=...&rn=...` | `/api/v1/artists/albums?source=kuwo&id=...` |
| 单曲与封面 | `music/musicInfo?mid=...` | `/api/v1/tracks/detail?source=kuwo&id=...` |

- `artist_detail` 是新增公共能力，目前仅酷我声明支持；其他平台返回 CAPABILITY_UNSUPPORTED。共用模型字段缺失保持 null，不以 0 或当前页数量代替未知。
- 每次只取一个上游页。请求页码从 1 开始，搜索响应页码从 0 开始；短页仍按上游页窗口推进，不提前结束。公共游标绑定歌手和页大小；在线验证将 336 的游标用于 1062 返回 CURSOR_MISMATCH。
- 离线覆盖空结果、最后一页、短页、错误页码、异常空页、重复 ID、错误实体、坏 JSON、无效参数、429／503，以及非官方图片、头像冒用、URL 用户信息、非默认端口和时长溢出。
- 封面不从歌手头像或不明确的 `pic` 字段猜测；缺少专辑 ID 或标题时不绑定歌曲封面。元数据及独立文件要求继续由既有策略控制。

## Windows 验收证据

- MSVC x64 Debug 构建通过。全量 CTest **412 项：406 通过、6 项按配置跳过、0 失败**，耗时 75.28 秒。
- 跳过项为 5sing／酷我／网易云的显式联网探针、系统信任根联网检查，以及需外部 MP3 样本的检查。上述酷我新增能力另经真实原生服务在线验收通过，不把跳过项算作成功。
- 在线客户端：[`packaging/e2e/kuwo_artists_e2e.py`](../../packaging/e2e/kuwo_artists_e2e.py)。Python 仅启动独立服务并驱动 HTTP、读取文件验证，生产实现仍为 C++，不调用第三方下载或解密工具。
- 本地完整日志和脱敏汇总：`out/kuwo-artists-build.log`、`out/kuwo-artists-build-final.log`、`out/kuwo-artists-ctest.log`、`out/kuwo-artists-live.log`、`out/kuwo-artists-live-report.json`（均被 Git 忽略）。

```powershell
cmake --build --preset windows-msvc-debug --parallel 4
ctest --preset windows-msvc-debug --output-on-failure --parallel 2
python packaging/e2e/kuwo_artists_e2e.py --binary out/build/windows-msvc-debug/src/server/music-service.exe --report out/kuwo-artists-live-report.json
```

构建前先在当前 shell 初始化 MSVC 环境。在线客户端临时配置开启 embed_cover／export_cover／sidecars_required，关闭歌词输出，以单独验证此次封面能力。

| 产物 | 字节数 | SHA-256 |
| --- | ---: | --- |
| 原始专辑封面／导出封面 | 108,066 | `fccdd24e95bfd17e4b9a9b32f4ce0ec49ab9dcc077ed7fe642ad3b271a6577a8` |
| MP3（含封面） | 4,426,418 | `13602418cd2b0685b701d413e82107bf7ed7cf5162bd2eebd3320ff7db0762d6` |
| FLAC（含封面） | 55,501,083 | `07dfc9f0fc9654d94736a2be15183b113a380614a5263b1fcf47d47e0367cf8b` |

这些是写入元数据后的发布文件哈希，与此前 Cookie 对照中的原始音源哈希属于不同产物。

本轮未新增个人歌单、Cookie profile、MGG 解密或 Hi-Res；未扩大账号权限。Windows 以外的专项运行验收按用户要求后置。当前完成勾选见[功能表](../feature-status.md)，匿名／非 VIP 音质对照见[此前记录](kuwo-cookie-evaluation.md)。
