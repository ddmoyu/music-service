# music-service

一个基于 C++20 的音乐后端服务，通过 HTTP API 提供音乐搜索、音源解析、下载与任务管理，供独立前端接入。

## 主要功能

- 音乐搜索、详情查询与链接解析
- 后台下载、批量任务、进度订阅与取消
- 下载任务持久化与重启恢复
- 音频校验、标签写入，以及歌词和封面导出
- 文件访问与媒体流服务

项目仍处于开发阶段，尚未发布正式版。

## 文档

- [API 说明](docs/design/cpp20-refactor/backend-service/api-contract.md)
- [配置说明](config/README.md)
- [项目文档](docs/README.md)
