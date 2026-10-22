# 服务入口

music-service 可执行程序。`main.cpp` 只接信号；其余都在 `server_core` 静态库里，便于在测试中进程内运行。

## 命令

```text
music-service [--config FILE] [--data-dir DIR] [COMMAND]
  serve（缺省）| version | check-config | init | healthcheck
  principal create --name NAME | principal list | principal enable|disable --id ID
  token issue --principal ID --scopes A,B --output FILE | token list --principal ID | token revoke --id ID
  backup --output FILE | restore --input FILE
  service                                  Windows：由服务管理器启动的 serve
```

- 退出码：0 成功，1 操作失败（含 INSTANCE_LOCKED），2 参数或配置错误。
- 管理命令离线执行：先取得 data_dir 实例锁，服务运行时直接失败，不绕开单实例约束改数据库。`backup` 例外：它用 SQLite 在线备份读取一致快照（含已提交的 WAL 内容），服务运行时也可执行，不取锁、不改数据库。
- `backup --output FILE` 写出单个独立的数据库文件（回滚日志模式），先写到 `FILE.partial`、通过 integrity_check 与迁移校验后再改名；目标已存在即失败（CONFLICT）。备份不含 `secrets/`，需与数据库一起另行保存。
- `restore --input FILE` 需要实例锁（服务须停止）：先校验备份（integrity_check、不比本程序新、迁移校验和一致），再把当前数据库一致复制为 `backups/replaced-<UTC 时间>.sqlite3`（可用它再次 restore 撤销），然后替换数据库并删除旧的 -wal／-shm。不修改 `secrets/`；较旧的 schema 由下次启动迁移。
- 打开已有数据库且有待执行的迁移时，先自动备份为 `backups/pre-migration-v<当前版本>-<UTC 时间>.sqlite3`；每个迁移在独立事务中执行，失败时数据库停留在原版本，可用该备份 restore。
- token 明文只写入新建的仅所有者可读文件（init 写 `secrets/bootstrap-token.txt`，issue 写 `--output`），目标已存在即失败；控制台只输出 token_id、scope 和文件位置。
- `serve` 就绪后在 stdout 输出一行 JSON（`event`=`ready`、`instance_id`、`api_base_url`、媒体／事件地址；未启用时为 null），供桌面启动器读取实际端口；日志写 stderr。
- `check-config` 校验配置、构建能力与目标根；已有数据库时以只读连接比对目录绑定，不改动持久状态。
- `service`（仅 Windows）把 serve 交给服务控制管理器：就绪后上报 RUNNING，停止／关机控制走与 Ctrl+C 相同的收尾；日志与就绪行写 `data_dir/logs/music-service.log`，更早的错误写应用程序事件日志；直接在控制台运行时提示改用 serve 并以 2 退出。安装脚本见 `packaging/windows/install-service.ps1`。
- `healthcheck` 请求本机控制入口的 `/health/ready`，按配置端口连接；端口为 0 的动态模式下请改用启动行里的地址。

## 启动与停止（ProcessHost）

启动：实例锁 → 打开数据库（有待执行迁移时先备份）并迁移 → SecretStore 与身份服务 → 目标目录绑定（新 ID 注册；同 ID 改根 DESTINATION_ROOT_CHANGED；移除仍被引用的 ID 为 DESTINATION_IN_USE；已绑定但当前离线的根保留并标记不可用）→ 平台注册表、回收孤立秘密 → 发布恢复与中断任务重新排队 → 清理（工作目录与过期幂等记录，之后每小时一次）→ 入场账本与上游传输 → 目录与资产用例、任务服务（启用 SSE 时任务仓储包上提交后发布的装饰器）→ SSE 监听（events.enabled）→ 控制监听 → ready。任一步失败按相反顺序撤销已打开的部分并以非零码退出。

停止：readiness 置否 → 关闭全部事件订阅并停止 SSE 监听 → 控制监听停止并等待在途请求 → 释放服务对象 → 关闭数据库 → 释放实例锁。媒体监听在 T19 前不启动；SSE 默认关闭（events.enabled=false），随 T20 下载闭环改为默认启用。
