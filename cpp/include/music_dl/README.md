# 公共头文件

domain 保存领域值对象与规则，包括下载意图、AccountBinding 和 AuthIdentityKey；application 保存用例契约和出站端口。平台能力与可选授权端口规划在 application/ports/providers，上游入场端口规划在 application/ports/runtime；按首次实现需要创建头文件。

当前没有头文件。只有跨模块使用的项目类型放在此目录；私有头文件跟随 src 下的实现。不在公共接口暴露 httplib、JSON、SQLite 或操作系统类型。
