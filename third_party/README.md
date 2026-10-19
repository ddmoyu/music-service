# 第三方源码

这里集中存放用于研究、对照和技术选型的外部项目源码。各子目录保留自己的 Git 仓库和历史；放入本目录不表示已经将其引入本项目的构建。

主仓库通过根目录的 `.gitignore` 忽略这些本地源码目录，只跟踪本说明文件。克隆主仓库不会自动下载这些参考项目。

## 当前目录

| 目录 | 用途 | 整理时的提交 |
| --- | --- | --- |
| [cinatra-source](cinatra-source/) | C++ HTTP 服务框架研究 | `cf095d224cb60ae7dfc623fde51c5c60db03c96d` |
| [cofetch-source](cofetch-source/) | 网络库研究 | `e25a2f6322e2b7866c22fd78364e3888efa60eef` |
| [cpr-source](cpr-source/) | C++ HTTP 客户端研究 | `cdbbd90fb01c29dd52ed3e19f606bb4980e71541` |
| [go-music-dl](go-music-dl/) | 音乐搜索、下载与应用业务实现参考 | `ddf2a4e550ca67086ff1f06edc5d15c7d4c8663d` |
| [music-lib-source](music-lib-source/) | 各音乐平台的搜索、音频地址解析与协议实现参考 | `7cc4467b4e300547edd5d3d5bf0eff1c21ffc2ec` |

## 放置约定

- 后续下载或克隆的外部项目源码统一放在本目录下。
- 本项目自己的实现和构建配置放在对应工程目录，研究与选型记录放在 [docs](../docs/README.md)。
- 运行外部项目的命令或脚本前，先进入对应子目录；保留上游源码中的模块名、导入路径和内部相对路径。

返回[工作区入口](../README.md)。
