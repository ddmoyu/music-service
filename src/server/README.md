# 服务入口

music-dl-server 可执行程序的入口：命令行参数、配置加载、ProcessHost 生命周期和依赖组装（CompositionRoot），以及 init、check-config、healthcheck 与本机身份管理等子命令。这里是后端自身的进程入口，不包含任何前端代码。

平台协议、下载编排和 HTTP 路由实现放在各自模块中，本目录只负责把具体实现连接起来。可执行目标随 T10 的真实入口一起加入，不提供模拟启动程序。
