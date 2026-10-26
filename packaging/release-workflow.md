# 自动构建与预览发布

工作流在 [.github/workflows/release.yml](../.github/workflows/release.yml)，复用
[ci.yml](../.github/workflows/ci.yml) 的七项原生验证。工作流代码存在不代表已经公开发布，
实际产物以 [Releases](https://github.com/ddmoyu/music-service/releases) 和对应 Actions 运行结果为准。

## 普通 CI

main 推送、PR 或手动选择 all 时运行完整矩阵。Windows x64、Linux x86_64/ARM64、
macOS Intel/ARM64 在包运行验证通过后上传压缩包和 SHA256SUMS，保留 14 天。
macos-package 仍只验证两个 macOS Release 包，不替代完整测试。
Windows 额外运行前端契约/客户端测试，五个原生任务均启动临时实例验证 HTTP、CORS 与 SSE。

zlib 仍固定 1.3.2 和原 SHA-256，优先使用官方 GitHub Release，其次使用官方 fossils。
下载失败不能通过更换哈希或跳过验证解决；Windows 失败时上传 CMake 与 CTest 诊断。

## 手动准备发行包

工作流合入默认分支后，在 Actions 选择 release，选择待验证的分支或 tag，保留 publish=false。
这会完整构建、测试和打包，产出 release-bundle，不创建 Release、不推送 GHCR。
普通 ci 手动打开 export_images 也可保存通过运行检查的两个容器镜像，镜像 tar 保留 3 天。

release-bundle 包含：

- 五个原生程序包，每包附第三方许可、配置、服务脚本和同版前端接入材料。
- frontend-integration.zip：接入指南、核心 OpenAPI、JS 模块与 TypeScript 类型。
- release-manifest.json：源码提交、版本、文件大小和 SHA-256。
- SHA256SUMS：上述文件的校验和。

每个平台的原始校验和先验证，再汇总；缺少任一平台、版本命名不符或哈希不符就停止。
镜像导出的是本轮已验证的镜像，发布作业不会重新构建另一个镜像。

## 公开预览发布

版本读取根 CMakeLists.txt 的 project VERSION。发布 tag 必须精确等于 v 加该版本；
例如版本 0.1.30 对应 v0.1.30。显式推送该 tag，或手动在此 tag 上选择 publish=true，才会公开发布。
不要为了验证工作流随意创建发布 tag。

1. 校验 tag 与版本、拒绝覆盖已公开的同版本 Release。
2. 在该提交重新执行完整 CI，汇总五个平台的已验证包。
3. 加载两份已验证镜像，核对架构和源码提交；推送带提交标识的候选标签，按 digest 拉取并运行检查。
4. 合并同版本 amd64/arm64 manifest，不创建 latest；两个原生环境匿名拉取版本镜像并运行检查。
5. 创建或恢复同版本草稿 Release，上传程序包、接入包、清单与校验和。
6. 从草稿下载全部资产，复核哈希与原清单完全一致，再转为 Pre-release。

使用 GitHub 自带 GITHUB_TOKEN；构建只需 contents:read，推送镜像作业需要 packages:write，
发布 Release 的作业需要 contents:write。仓库 Actions 策略必须允许工作流声明这些权限。
无需新增个人访问令牌。

首次创建 GHCR 包时，GitHub 可能默认设为私有；在包的 Package settings 中将其设为 public，
并确认仓库有写入权限。匿名检查失败时工作流会明确提示，调整后重跑失败作业即可；
不会在匿名镜像不可用时把 Release 发布为完成状态。
镜像仓库与 Release 无跨服务事务：中途失败可能已留下候选镜像或版本 manifest，
但不会把未验证的 Release 标为已发布。重跑只更新草稿；已发布版本使用新版本号。

macOS 默认 ad-hoc 签名，尚无 Developer ID、公证与目标机器 Gatekeeper 验收。
Linux 以 Ubuntu 24.04 为基线，不推定旧发行版及所有 NAS 均可运行。
在线音源诊断允许失败，绿色 CI 不代表所有来源均可播放或下载。

## 验证与后续维护

~~~text
python -I packaging/generate_openapi.py
python -I packaging/validate_integration.py
python -I packaging/test_release.py
node --test examples/frontend/client.test.mjs
~~~

修改版本或接口时同步更新 OpenAPI 生成定义及生成文件。
发布工作流不替代账号、历史任务和持久化格式变化所需的数据升级验收；此项遵循
[项目约定](../AGENTS.md) 与[版本切换要求](RELEASING.md#数据与版本切换)。
下载前端接入包后按[指南](../docs/frontend-integration.md)连接。
