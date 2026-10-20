# 第三方依赖

当前未接入任何第三方库。已选方向为 cpp-httplib、nlohmann/json 和 SQLite；具体发布版本、TLS、协议加密及标签库按设计验证后确定。

首次接入时集中添加 Versions.cmake、Dependencies.cmake 和版本校验，使用 FetchContent、Git 浅克隆、固定发布 tag 并校验 commit。只获取已启用模块需要的依赖，不从 third_party 研究目录直接链接。
