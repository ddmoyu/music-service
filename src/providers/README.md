# 平台适配模块

每个平台独立成包，包含能力描述、协议构造、解析、配置与契约样本。新增平台通过构建注册项接入，不修改公共 source 枚举、业务路由或任务状态机。

- registry：后续放 ProviderBundle 工厂绑定和注册表。
- common：平台之间确实共享的编码／协议工具，不放跨平台业务策略。
- kuwo：第一个计划实现的平台，目前仅预留目录。
- 其他平台和外部 helper：在实际接入时创建，不预建空平台包。

平台需要授权能力时，在自身 auth 目录实现可选 ICredentialVerifier、ICredentialRefresher、IApplicationTokenProvider，并通过 bundle 的 auth_profiles 绑定。平台协议只返回校验结果或秘密候选；账号选择、刷新合并与条件提交属于 application/accounts。所有上游请求共用入场端口，不建立平台私有线程池或 semaphore。

music_service_providers 当前只是表示依赖方向的 INTERFACE 目标，没有平台注册或实现，也不声明任何平台已支持。后续每个平台独立 target，再由聚合目标选择链接。
