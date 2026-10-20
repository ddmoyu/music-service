# 公共头文件

只有跨模块使用的项目类型放在此目录；私有头文件跟随 src 下的实现。不在公共接口暴露 httplib、JSON、SQLite 或操作系统类型。

- domain：Result 与错误、平台引用（SourceId、PlatformId、TrackRef）、曲目与分页、品质与资源判定、AccountBinding／AuthIdentityKey、任务状态迁移与取消规则、下载意图及指纹。实现在 `src/domain`，无 I/O、无第三方依赖。
- application/ports：出站端口。providers（平台描述、搜索与音源解析能力）、runtime（上游入场租约、时钟、操作截止与取消）、transport（上游 HTTP）、storage（任务仓储）。端口只使用项目类型，具体实现由 apps/server 组装。
- application/config：生效配置的纯数据类型 ServiceConfig。
- infrastructure：配置加载与校验（config）、SystemClock／ManualClock、结构化日志与脱敏、关联 ID、指标接口（observability）。
