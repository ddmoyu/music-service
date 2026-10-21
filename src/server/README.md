# 服务入口

music-service 可执行程序。`main.cpp` 只接信号；其余都在 `server_core` 静态库里，便于在测试中进程内运行。

## 命令

```text
music-service [--config FILE] [--data-dir DIR] [COMMAND]
  serve（缺省）| version | check-config | init | healthcheck
  principal create --name NAME | principal list | principal enable|disable --id ID
  token issue --principal ID --scopes A,B --output FILE | token list --principal ID | token revoke --id ID
```

- 退出码：0 成功，1 操作失败（含 INSTANCE_LOCKED），2 参数或配置错误。
- 管理命令离线执行：先取得 data_dir 实例锁，服务运行时直接失败，不绕开单实例约束改数据库。
- token 明文只写入新建的仅所有者可读文件（init 写 `secrets/bootstrap-token.txt`，issue 写 `--output`），目标已存在即失败；控制台只输出 token_id、scope 和文件位置。
- `serve` 就绪后在 stdout 输出一行 JSON（`event`=`ready`、`instance_id`、`api_base_url`、媒体／事件地址；未启用时为 null），供桌面启动器读取实际端口；日志写 stderr。
- `check-config` 校验配置、构建能力与目标根；已有数据库时以只读连接比对目录绑定，不改动持久状态。
- `healthcheck` 请求本机控制入口的 `/health/ready`，按配置端口连接；端口为 0 的动态模式下请改用启动行里的地址。

## 启动与停止（ProcessHost）

启动：实例锁 → 打开数据库并迁移 → SecretStore 与身份服务 → 目标目录绑定（新 ID 注册；同 ID 改根 DESTINATION_ROOT_CHANGED；移除仍被引用的 ID 为 DESTINATION_IN_USE；已绑定但当前离线的根保留并标记不可用）→ 平台注册表、回收孤立秘密 → 入场账本与上游传输 → 目录与资产用例、任务服务（启用 SSE 时任务仓储包上提交后发布的装饰器）→ SSE 监听（events.enabled）→ 控制监听 → ready。任一步失败按相反顺序撤销已打开的部分并以非零码退出。

停止：readiness 置否 → 关闭全部事件订阅并停止 SSE 监听 → 控制监听停止并等待在途请求 → 释放服务对象 → 关闭数据库 → 释放实例锁。媒体监听在 T19 前不启动；SSE 默认关闭（events.enabled=false），随 T20 下载闭环改为默认启用。
