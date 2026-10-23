# Jamendo 协议样本

2026-10-23 在当前 Windows 网络匿名读取网站 `/api/tracks?id=49295`、`/api/albums?id=5987`、`/api/artists?id=6934`。

JSON 保留实际身份、名称、关联、专辑位置、权限与元数据。移除了 `stream` 临时地址、波形、播放统计和无关社交字段；不含 Cookie、账号会话或外部开发者密钥。样本不是在线可用性的永久保证。

`jamendo_test.cpp` 另用合成响应覆盖分页、缺失、重复、身份错配、限流、下载许可、音质选择和地址边界。固定 SHA-1 向量由独立 Python hashlib 计算，生产使用现有 Mbed TLS。

第二阶段追加 `categories.json`：官网 `/api/tags` 的 26 个风格／情绪／主题标签，保留 ID、名称、原生分组和 idstr；`category-tracks.json`：`/api/tracks?tagId=23&order=hotness&limit=6&offset=0` 的前两项身份、名称、关联、时长和可用状态。此接口确实不带搜索结果的 `type` 字段，回归覆盖该差异。均未保留临时播放地址、下载文件或凭据。
