# 源码

每个模块一个目录，头文件（`.hpp`）与实现（`.cpp`）放在一起。所有模块以 `src/` 为包含根，例如 `#include "domain/result.hpp"`、`#include "application/ports/runtime/admission.hpp"`。C++ 命名空间与目录对应：`music_dl::domain`、`music_dl::application`、`music_dl::infrastructure`。

只在单个模块内部使用的头文件放进该模块的 `detail/` 或具体子目录（如 `domain/detail/`、`infrastructure/config/address.hpp`），其他模块不得包含。公共类型不暴露 httplib、JSON、SQLite 或操作系统类型。

## 模块与依赖

| 目录 | 职责 | 当前状态 |
| --- | --- | --- |
| `domain` | Result 与错误码、平台引用、曲目与分页、品质判定、账号绑定与授权身份、任务状态迁移与取消规则、下载意图与指纹 | 静态库，T02 |
| `application` | `ports/`：平台能力、上游入场与租约、时钟、上游 HTTP、任务仓储等出站端口；`config/`：生效配置 ServiceConfig；用例按任务陆续加入 | 头文件，T02／T03 |
| `infrastructure` | `config/`：配置加载与校验；`observability/`：结构化日志、脱敏、关联 ID、指标；`clock.hpp`；后续加入传输、存储、文件、秘密 | 静态库，T03 |
| `providers` | 平台注册与各平台协议包 | 待实现 |
| `downloads` | 持久任务调度、Runner、重试／续传、发布与恢复 | 待实现 |
| `audio_processing` | 格式验证、解密、标签 | 待实现 |
| `runtime` | 执行器、统一上游入场账本、取消与停机 | 待实现 |
| `api`、`media_server`、`event_server` | 控制、媒体、SSE 三个 HTTP 入口的协议适配 | 待实现 |
| `server` | music-dl-server 可执行程序：参数、生命周期、依赖组装 | 待实现（T10） |

依赖方向：application 依赖 domain；providers、downloads、audio_processing、infrastructure、runtime 和三个 HTTP 入口依赖 application 的端口与领域类型；只有 `server` 把具体实现组装在一起，业务层不反向依赖 HTTP、数据库或某个平台。

尚未实现的模块以 CMake INTERFACE 目标记录依赖，实现时改为普通库并明确 PUBLIC／PRIVATE。逐文件声明源码，不使用 GLOB；不通过空函数或空 main 伪造可运行产物。详细契约见[内部接口](../docs/design/cpp20-refactor/backend-service/internal-contracts.md)。
