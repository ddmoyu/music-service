"""Generate release notes without claiming unperformed platform checks."""
import os
import re
import sys

version, commit = sys.argv[1:]
if not re.fullmatch(r"\d+\.\d+\.\d+", version) or not re.fullmatch(r"[a-f0-9]{40}", commit):
    raise SystemExit("invalid version or commit")
repo = os.environ["GITHUB_REPOSITORY"]
url = f"https://github.com/{repo}/blob/v{version}"
baseline_note = ("0.1.30 为后续数据兼容基线，不迁移更早开发数据；格式不符时保留旧数据，使用新的数据目录初始化。"
                 if version == "0.1.30" else "数据兼容范围及必要的迁移验收以本版本发布说明为准。")
print(f"""开发预览版，源码提交：{commit}。

本次提供独立音乐后端、五个平台程序包和同版前端接入材料：
- HTTP/JSON 搜索、音源解析、媒体访问，下载任务与 SSE 进度订阅。
- 批量下载、严格匹配换源、任务持久化与重启恢复。
- 账号启停、Cookie 校验与会员信息查询。
- 核心 OpenAPI、无依赖 JS 客户端、TypeScript 类型与调用示例。

本版本发布流程已通过 Windows x64、Linux x86_64/ARM64、macOS Intel/Apple Silicon
的构建、测试和发行包检查，以及两个 Linux 架构的容器运行检查。
在线音源诊断允许失败，以上结果不代表所有真实音源均可用。

- 程序包：下载匹配系统和架构的压缩包，保留完整目录结构。
- 前端接入包：frontend-integration.zip，包含 OpenAPI、JS 客户端和 TypeScript 类型。
- 校验：SHA256SUMS；文件大小和源码提交见 release-manifest.json。
- 容器：ghcr.io/{repo.lower()}:{version}（linux/amd64、linux/arm64）。

Linux 原生包以 Ubuntu 24.04 为验证基线；macOS 最低 15.0，使用 ad-hoc 签名，
尚无 Developer ID 签名或公证。其他旧系统及 NAS 型号未逐一验收。
{baseline_note}
切换版本前停止服务并备份配置、数据库、秘密及下载文件，按对应版本的升级要求操作。
本发布流水线验证构建、打包与运行，不替代数据格式变更所需的升级和迁移验收。
前端应固定配套后端版本，读取 info/providers 声明的实际能力。

[运行说明]({url}/packaging/package/README.md) ·
[Docker 教程]({url}/packaging/docker/README.md) ·
[第三方前端接入]({url}/docs/frontend-integration.md) ·
[API 契约]({url}/docs/design/cpp20-refactor/backend-service/api-contract.md)
""")
