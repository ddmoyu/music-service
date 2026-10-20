# 酷我适配器

首个开发平台。`bundle.cpp` 已向注册表登记 source `kuwo`、名称与实现标识（kuwo-native 0.0.0），但不声明任何能力：所有能力调用返回 CAPABILITY_UNSUPPORTED，providers 列表如实显示为空能力。protocol 与 parsing 目录仍为预留。

后续先完成标准品质的搜索、详情／链接解析和音源解析，复用公共下载与文件发布流程，再扩展集合和其他能力。
