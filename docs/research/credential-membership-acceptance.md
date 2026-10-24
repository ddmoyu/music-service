# Cookie 自动校验与会员识别验收

日期：2026-10-24；Windows x64 MSVC Debug；服务版本 0.1.27，SQLite schema 5。实现和验收均在当前工作区完成，没有新增第三方依赖。

## 行为与接口

保存或替换 Cookie 自动触发后台校验，立即返回凭证与校验编号。调用方查询凭证获得登录状态、会员类型、平台名称与到期时间；字段无法确认时返回 unknown／null，按 [API 契约](../design/cpp20-refactor/backend-service/api-contract.md#cookie-自动校验与会员) 显示“未知”。本仓库是后端服务，页面展示由调用方使用这些字段完成。

登录状态和会员状态独立，会员有效期不冒充 Cookie 有效期。支持查询、手动刷新与取消，结果持久化；更新或撤销凭证后，旧任务不能覆盖新版本。校验使用独立的 2 个线程、32 项队列和包含排队的 15 秒预算，每个调用方最多 4 项在途。旧数据库明确拒绝启动，使用新的 data_dir 初始化；本轮在线验收只使用独立临时目录，没有迁移或删除用户的既有数据。

## 平台依据

| 平台 | 登录／会员协议 | 归一化 |
| --- | --- | --- |
| 网易云 | 现有 account/get；music-vip-membership/front/vip/info | redplus 为黑胶 SVIP，associator 为黑胶 VIP，musicPackage 为音乐包；使用对应 expireTime |
| QQ 音乐 | GetLoginUserInfo；VipLogin.VipLoginInter.vip_login_base | identity.HugeVip 为超级会员；旧字段 svip 是豪华绿钻；identity.vip 为绿钻；日期按北京时间转换为 UTC |
| 酷我 | vip1.kuwo.cn 的 POST /vip/v2/sysinfo，op=gui；POST /vip/v2/user/vip，op=ui | 登录 UID 必须匹配；svipExpire／vipLuxuryExpire／vipExpire／vipmExpire 分别对应超级 VIP／豪华 VIP／VIP／音乐包 |
| Bilibili | 现有 /x/web-interface/nav | vipType／vipStatus 识别月度或年度大会员，vipDueDate 缺失时到期时间未知，不虚构 SVIP |
| 咪咕 | 已登记 Cookie profile，但没有验证实现 | 登录与会员未知，原因 CAPABILITY_UNSUPPORTED |

QQ 官方网页 [会员脚本](https://y.qq.com/portal/vipportal/js/vip/common_9ff5f66.js) 将旧 svip 字段显示为“豪华绿钻”，因此不能仅凭字段名称输出超级会员。协议定位另参考 [QQMusicApi 用户模块](https://github.com/luren-dc/QQMusicApi/blob/main/qqmusic_api/modules/user.py)、[用户模型](https://github.com/luren-dc/QQMusicApi/blob/main/qqmusic_api/models/user.py) 与 [网易云 API 项目的会员模块](https://github.com/NeteaseCloudMusicApiEnhanced/api-enhanced/blob/main/module/vip_info.js)，只核对协议，没有集成这些项目或引入其依赖。酷我的字段也与[此前账号复测](kuwo-membership-quality-retest.md)一致。

## 真实 Cookie 联调

通过本次构建的服务运行 `packaging/e2e/credential_membership_e2e.py`，只读取本机忽略目录中的 Cookie，报告不包含 Cookie、账号标识或原始响应。2026-10-24 本次实际结果如下：

| 平台 | 登录 | 会员 | 到期时间（北京时间） | 保存至完成 |
| --- | --- | --- | --- | --- |
| 网易云 | 有效 | 黑胶 VIP | 2026-10-27 23:59:59 | 约 0.40 秒 |
| QQ 音乐 | 有效 | 非会员 | 未提供 | 约 0.52 秒 |
| 酷我 | 有效 | 豪华 VIP | 2026-11-23 11:15:18 | 约 0.37 秒 |

三个结果均在服务重启后原样读回。四个平台缺失登录会话的测试 Cookie 均返回 invalid，会员保持 unknown；咪咕返回 unknown／CAPABILITY_UNSUPPORTED。真实在线测试未覆盖 SVIP 账号或 Bilibili 已登录的大会员账号，这些分支使用构造响应验证，不能等同于真实账号验收。耗时仅为本机这次采样，不作为延迟承诺。

本地脱敏报告：`out/membership-live-report.json`；运行日志：`out/membership-live.log`。该目录不进入 Git。

## 自动化覆盖

- 平台响应：VIP／SVIP 优先级、到期与非会员、缺字段、未知枚举、互相矛盾的字段、时间范围与时区、过大／过深响应。
- 服务与存储：保存即入队、重复校验合并、Cookie 替换、旧结果失效、取消、撤销、排队超时、过载、崩溃恢复、结果持久化失败与重试。
- HTTP：慢上游期间保存立即返回，健康检查与凭证查询可响应；过载返回明确状态，取消与重启可观察。

MSVC Debug 构建通过，运行 `music-service version` 返回 0.1.27；7 个受影响的 Python 验收脚本语法检查通过，`git diff --check` 通过。

全量并行 CTest 共 628 项：621 项通过、6 项依配置跳过、1 项既有 SecretStore 用例因共享临时目录被另一进程占用而失败。该用例 `secret references cannot address other files` 单独串行复跑通过；它与另外两个用例使用相同的 `music-service-秘密-test` 临时目录，此次未改动该测试夹具。Cookie 会员、平台注册、API、超时、取消、并发和持久化故障用例均通过。6 项跳过涉及可选在线探测或外部样本，不作为本轮验收通过项；上文三平台真实 Cookie 验收单独执行。

构建、全量回归和失败项复跑日志分别保存在 `out/membership-build.log`、`out/membership-ctest.log`、`out/membership-ctest-rerun.log`；Linux、macOS 与发行包未在本轮实机验证。

## 0.1.28：QQ 会员日期精度补齐

2026-10-24 用户开通豪华绿钻后，上游 `send` 返回 `2026-10-27`，而普通绿钻 `identity.overdate` 返回带时分秒的值。之前只接受完整时间，导致豪华绿钻到期信息为 null。本次保留不同权益的独立有效期，新增 `membership.expires_on` 和 `expiry_precision`：仅有日期返回 date，完整时刻返回 datetime，无法确认返回 unknown；expires_on 与 expires_at 互斥。

日期严格校验日历有效性，按北京时间日期边界检查会员状态；到期当天允许有效或已过期，由 QQ 的状态标记决定。日期不补成午夜或当天末尾，也不借用另一种会员的精确时刻。日期会随凭证持久化；改标签保留，替换 Cookie、重新校验或撤销时按原有证据失效规则清除。

本次真实 Cookie 经 0.1.28 服务的自动校验后，凭证详情和列表均返回：

```json
{
  "name": "豪华绿钻",
  "status": "active",
  "type": "vip",
  "expires_at": null,
  "expires_on": "2026-10-27",
  "expiry_precision": "date"
}
```

重启后会员字段完整读回，日期与同次上游查询一致。本地脱敏报告为 `out/qq-membership-date-report.json`，日志为 `out/qq-membership-date-live.log`。测试用 Cookie 临时文件已删除，未进入 Git。

MSVC Debug 构建通过，版本命令返回 0.1.28；定向单元测试 24 项／1536 个断言、账号／存储／OAuth／HTTP／备份集成测试 70 项／4123 个断言通过。覆盖真实日期字段形态、不同权益精度、闰年与非法日期、北京时间换日、日期和时刻切换、列表、改名、撤销及重启。验收脚本语法检查和 diff 检查通过。本次未重复全量回归或其他系统实机验证。

新增日期列后 SQLite 当前结构为 schema 6，旧开发数据库须使用新的 data_dir 初始化；本次没有迁移或删除旧数据。

## 账号启用、停用与 Cookie 清理（0.1.29）

2026-10-24，新增 `enabled` 和 `secret_cleanup_pending`。同一调用方、同一平台最多启用一个账号；新建账号并保存 Cookie 时自动启用和选中，也可用 `enabled=false` 创建不含秘密的账号记录。选择另一个账号时清除原账号 Cookie；停用保留 ID 和名称，再次启用必须重新提交 Cookie。永久撤销的账号不能重新启用。

接口为 `POST /api/v1/credentials/{id}/disable`（停用单个）、`PUT /api/v1/credential-defaults/{source}`（选择并启用，已停用账号必须带新 secret）、`DELETE /api/v1/credential-defaults/{source}`（全部停用）；现有详情、列表和默认选择查询返回对应状态。完整参数、错误和调用示例见 [API 契约](../design/cpp20-refactor/backend-service/api-contract.md#111-默认平台凭证)。

数据库在单事务内更新账号状态、版本、选择及待清理引用，提交后移除秘密文件；清理失败明确返回 STORAGE_UNAVAILABLE，保留待清理标记，重试或重启继续完成。停用阻止显式选择、新租约和校验，取消旧后台校验，迟到结果不能恢复旧状态。排队下载保持原账号绑定，该账号停用后失败，不会改用新账号；已经取得秘密快照的在途请求仍遵循任务取消流程。

独立 `music-service.exe` 进程以两个合成 QQ Cookie 完成创建、切换、停用、全部停用、重新启用及重启测试。最终保留两条账号描述，enabled 数量为 0、Cookie 文件数量为 0、SQLite 秘密引用和待清理项均为 0；缺少新 Cookie 和重新启用已撤销账号都被拒绝。使用独立临时目录并清理，不需要真实账号，不将此测试作为 QQ 会员查询或歌曲权益验收。

复现命令（Python 标准库，无新增依赖）：

```powershell
python packaging/e2e/credential_activation_e2e.py --binary out/build/windows-msvc-debug/src/server/music-service.exe --report out/account-activation-e2e-report.json
```

本地脱敏报告为 `out/account-activation-e2e-report.json`，运行日志为 `out/account-activation-e2e.log`。MSVC Debug 构建和版本命令已确认 0.1.29。当前 SQLite 结构为 schema 7，旧开发库需使用新的 data_dir 初始化，不迁移或删除已有数据。

完整 CTest 串行回归：共 646 项，640 项通过、6 项按条件跳过，0 失败，耗时 174.43 秒。跳过项为未启用的在线协议／系统信任根探测及缺少真实 MP3 样本的检查。日志为 `out/account-activation-ctest-final.log`。覆盖多账号切换、全部停用、跨用户隔离、Cookie 文件移除、失败清理重试与重启回收、事务回滚、并发切换、旧校验取消、目录缓存／游标失效，以及停用账号的排队下载拒绝使用其他账号。验收脚本语法与差异格式检查通过；本轮没有进行 Linux／macOS 实机验证。
