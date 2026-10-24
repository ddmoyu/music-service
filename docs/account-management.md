# 平台账号与 Cookie 接入指南

适用于当前 0.1.29 接口。通过同一个服务用户（principal）接入的前端共享账号选择；不同服务用户相互隔离。平台 Cookie 用来访问音乐平台，服务访问 token 用来调用本服务，二者分别管理。

同一用户在同一平台可以保存多个账号记录，但最多启用一个，也可以全部停用。启用新账号会停用该平台原账号并清除原 Cookie。停用保留账号 ID 和名称，再次启用必须重新提交 Cookie；永久撤销的账号不能重新启用。

## 请求与接口

账号管理接口均要求服务 token 具有 `credentials:write` 权限。先用具有 `catalog:read` 权限的 token 查询 `GET /api/v1/providers`，根据 `auth_methods` 判断平台支持的 kind、auth_profile、输入字段及校验能力，不硬编码平台清单。

以下 HTTP 示例使用控制服务地址，路径已经包含 `/api/v1`。每个请求都带 `Authorization: Bearer <service-token>`；有 JSON body 时再带 `Content-Type: application/json`。示例中的 Cookie 和 ID 是占位符，使用时替换。除特别说明外，响应示例只展示成功信封中的 `data`，完整信封见 [API 契约](design/cpp20-refactor/backend-service/api-contract.md#3-json-响应与错误)。

| 操作 | 方法与路径 | 成功结果 |
| --- | --- | --- |
| 新建账号 | `POST /api/v1/credentials` | 201，返回账号描述 |
| 查看列表／单个账号 | `GET /api/v1/credentials?source=qq`／`GET /api/v1/credentials/{id}` | 200，列表的 data 为 `{items:[...]}`；单个返回账号描述 |
| 修改名称或已启用账号的 Cookie | `PUT /api/v1/credentials/{id}` | 200，返回更新后的描述 |
| 启用／切换账号 | `PUT /api/v1/credential-defaults/{source}` | 200，返回平台选择与 selection_version |
| 查看当前启用账号 | `GET /api/v1/credential-defaults/{source}` | 200，credential_id=null 表示全部停用 |
| 停用指定账号 | `POST /api/v1/credentials/{id}/disable` | 200，返回描述；空 body 或 `{}` |
| 全部停用该平台账号 | `DELETE /api/v1/credential-defaults/{source}` | 200，返回空选择 |
| 永久撤销账号 | `DELETE /api/v1/credentials/{id}` | 200，返回 status=revoked 的描述 |
| 手动校验／取消校验 | `POST /api/v1/credentials/{id}/verify`／`POST /api/v1/credentials/{id}/verify/{check_id}/cancel` | 校验受理为 202，已完成结果或取消为 200；空 body 或 `{}` |

## 两个 QQ 账号的完整操作

### 1. 新建并启用账号 A

```http
POST /api/v1/credentials
Authorization: Bearer <service-token>
Content-Type: application/json

{"source":"qq","label":"QQ 主账号","kind":"cookie","auth_profile":"web_cookie","secret":{"cookie":"<cookie-A>"}}
```

`enabled` 默认 true。成功后使用响应的 credential_id 作为账号 A 的 ID，例如 `cre_account_a`；后端自动选中 A 并开始后台校验。201 表示已保存，不表示已经确认登录或会员有效。若该平台已有启用账号，它会被停用并清除 Cookie。

`kind`、`auth_profile` 必须显式提供，Cookie 放在 `secret.cookie`。顶层 `cookie` 字段会被拒绝；Cookie 不作为 URL 查询参数提交，响应也不会回显 Cookie 或 secret_ref。

### 2. 创建停用的账号 B

```http
POST /api/v1/credentials
Authorization: Bearer <service-token>
Content-Type: application/json

{"source":"qq","label":"QQ 备用账号","kind":"cookie","auth_profile":"web_cookie","enabled":false}
```

此时必须省略整个 secret 字段，连空对象或 null 也不能提交。返回新的 credential_id，例如 `cre_account_b`。B 不保存 Cookie、不启动校验，A 保持启用。`label` 是调用方的备注名；credential_id 是本服务生成的账号记录 ID，不是 QQ 号。

### 3. 选择并启用 B

```http
PUT /api/v1/credential-defaults/qq
Authorization: Bearer <service-token>
Content-Type: application/json

{"credential_id":"cre_account_b","secret":{"cookie":"<fresh-cookie-B>"}}
```

成功返回平台选择；下例的版本号仅为示意：

```json
{"source":"qq","credential_id":"cre_account_b","selection_version":2}
```

B 启用并自动校验新 Cookie，A 停用并删除原 Cookie。若稍后改回 A，仍通过同一 PUT 接口提交 `cre_account_a` 和新的 `secret.cookie`；后端不恢复之前删除的 Cookie。

重复选择当前已启用且可用的账号时可省略 secret，此时不增加版本。只要提交 secret，就按秘密替换处理，推进账号 version 并重新校验，即使 Cookie 文本相同。version 和 selection_version 由后端生成，不放进请求体。

### 4. 更新 Cookie 或名称

```http
PUT /api/v1/credentials/cre_account_b
Authorization: Bearer <service-token>
Content-Type: application/json

{"label":"QQ 下载账号","secret":{"cookie":"<renewed-cookie-B>"}}
```

已启用账号可以替换 Cookie，新 Cookie 保存后重新校验。只改名称可仅提交 `{"label":"新名称"}`，保留已完成的会员结果；如果旧校验仍在进行，会按新版本重新排队。已停用账号只能改名称，更新 secret 会被拒绝；启用必须使用平台选择接口。

### 5. 停用一个或全部账号

停用 B：

```http
POST /api/v1/credentials/cre_account_b/disable
Authorization: Bearer <service-token>
```

全部停用自己的 QQ 账号：

```http
DELETE /api/v1/credential-defaults/qq
Authorization: Bearer <service-token>
```

成功后启用账号的 Cookie 文件已清除，enabled=false、secret_cleanup_pending=false，平台选择的 credential_id=null。列表仍保留账号描述，重启后仍为停用。重复停用或全部停用保持版本不变；停用一个已经停用的账号不会影响另一个已启用账号。

前端的“清空 Cookie”操作应调用停用接口。向更新接口提交空字符串会返回参数错误，不能用它表示停用。

永久撤销使用 `DELETE /api/v1/credentials/{id}`，列表保留 status=revoked、enabled=false 的记录；即使提供新 Cookie 也不能重新启用，需重新创建账号记录。仅想暂时不用账号时使用停用。

## 状态与会员显示

账号列表返回每条记录的 enabled、status、login_status、verification、membership 和 secret_cleanup_pending。用 `GET /api/v1/credential-defaults/qq` 获取当前选择；列表和选择是独立查询，并发操作后应重新读取两者。

| 字段 | 前端处理 |
| --- | --- |
| `enabled` | 展示“启用／停用”；不能从 status=active 推断已启用或已登录 |
| `status` | active／invalid／revoked；revoked 禁止再次启用 |
| `secret_cleanup_pending` | true 显示“Cookie 清理未完成”，按错误处理流程重试；不能报告清理成功 |
| `login_status` | valid 显示已登录，invalid 显示登录失效，unknown 显示未知 |
| `verification.state` | pending 显示校验中；只在 pending 时轮询，idle／complete 停止轮询 |
| `membership.status` | unknown 显示未知，none 显示非会员；active／expired 结合 name 展示会员名称和有效状态 |
| `membership.type`／`name` | type 为 unknown／none／vip／svip／music_package，优先展示平台返回的 name；未知 name 为 null |
| `membership.expiry_precision` | datetime 读取 expires_at 并按用户时区显示；date 原样显示 expires_on；unknown 显示到期时间未知 |

创建、替换或重新启用 Cookie 后，查询 `GET /api/v1/credentials/{id}` 获取后台校验结果；平台选择 PUT 返回的是选择对象，需要另查账号描述取得 verification.id。以当前账号 version 和 verification.id 为准，丢弃旧轮询结果。查不到登录、会员或到期信息时显示未知；会员查询失败不抹掉已确认的登录成功，也不能推断为非会员。

以下为 QQ 日期精度的会员示例：

```json
{
  "status":"active",
  "type":"vip",
  "name":"豪华绿钻",
  "expires_at":null,
  "expires_on":"2026-10-27",
  "expiry_precision":"date",
  "checked_at":"2026-10-24T12:18:05.091Z",
  "error_code":null
}
```

expires_at 与 expires_on 互斥，日期精度不补成午夜或当天末尾。会员有效期与账号顶层 expires_at（凭证本身的有效期）独立。会员类型不保证某首歌或音质可用，播放／下载以实际接口结果为准。平台字段解析与真实账号验证范围见 [Cookie 与会员验收](research/credential-membership-acceptance.md)。

停用会将登录和会员结果重置为 unknown，并记录 ACCOUNT_DISABLED，旧校验不能恢复它们。新建的停用记录则保持 idle／unknown。unknown 或登录 invalid 本身不会自动停用账号或删除 Cookie；删除秘密由停用、切换或撤销操作完成。

## 错误与重试

| 场景 | 结果与处理 |
| --- | --- |
| 缺少／无效服务 token，或权限不足 | 401／403；先处理本服务的认证和 scope，不把它当成平台 Cookie 失效 |
| 记录不存在或属于其他服务用户 | 404 NOT_FOUND |
| 创建时 enabled 不是布尔值、停用记录仍传 secret、Cookie 为空；停用后直接更新秘密或请求校验 | 400 VALIDATION_FAILED；修正请求，启用操作改用平台选择接口 |
| 选择账号时缺少新 secret、账号已撤销、source 不匹配，或未提交新 secret 且账号已失效／过期 | 422 VALIDATION_FAILED；重新读取账号，按需提交新 Cookie |
| 并发修改冲突 | 409 VERSION_CONFLICT；重新查询账号和平台选择后再决定操作 |
| 自动校验过载 | 保存可返回 201／200，但 verification.error_code=SERVICE_BUSY；按 retry_after_seconds 等待后手动 verify；手动校验过载为 503 |
| 账号操作返回 503 STORAGE_UNAVAILABLE／SERVICE_BUSY | 状态可能已经提交，先查列表及当前选择；若 secret_cleanup_pending=true，继续下面的清理恢复流程 |

秘密文件清理失败时，停用状态已经生效，账号不能继续授权；失败不会恢复旧 Cookie 引用。可再次调用该账号的 disable，或全部停用接口以继续清理。重复停用不会再次增加账号版本。启动时也会继续清理；底层存储故障仍存在时需要先恢复存储访问。

切换或替换秘密后发生清理错误，新账号或新 Cookie 可能已经保存；不要仅凭失败响应将界面还原成旧选择。重新查询后核对哪个账号的 secret_cleanup_pending=true：切换时，重试停用已经停用的旧账号可以继续清理，不会停用已选中的另一个账号。同一账号替换秘密时，调用 disable 会连新 Cookie 一起删除；需要保持启用时，可先恢复存储访问，再重启服务继续清理。选择“全部停用”会连当前启用账号一起停用。

账号创建没有下载任务使用的 Idempotency-Key 去重协议。网络中断或收到存储错误后先查列表，不能直接循环 POST 创建；名称不是唯一键。Cookie 清理待处理时也不要重复创建账号来规避错误。

## 对播放、下载与重启的影响

播放和下载使用已启用且可用的账号。显式指定已停用的 credential_id 也会被拒绝。全部停用后，平台能力允许匿名时可匿名访问，需要账号的能力返回 CREDENTIALS_REQUIRED。

排队下载保留受理时的账号绑定。切换会停用原账号，任务后续获取原账号 Cookie 时失败，不会转用新账号或匿名身份。重新启用原账号并提交新 Cookie 后，可以创建新任务或调用任务 retry。已经取得秘密快照的在途请求可能继续完成，需要停止下载时使用任务取消接口。账号切换、停用或秘密版本变化后，应丢弃旧游标并重新请求目录页。

当前数据库为 schema 7，只接受当前结构。旧开发库需要指定新的 data_dir 初始化，已有数据不会自动迁移或删除；当前结构内正常重启保留账号选择、停用状态和待清理项。初始化与备份说明见 [配置说明](../config/README.md#oauth-应用配置与秘密)和[服务入口](../src/server/README.md#命令)。停用不修改用户另行保存的历史备份。

接口定义以 [API 契约](design/cpp20-refactor/backend-service/api-contract.md#11-凭证管理)为准；存储事务见 [内部契约](design/cpp20-refactor/backend-service/internal-contracts.md#62-凭证和秘密提交)，复现命令见 [测试说明](../tests/README.md#账号启停与会员校验)。
