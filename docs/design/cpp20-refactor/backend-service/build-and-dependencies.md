# C++20、CMake 与 FetchContent 依赖设计

用户已确定：C++20、CMake，第三方依赖通过 FetchContent 获取，Git 拉取启用浅克隆。本文落实到后端主方案；没有创建 CMakeLists、Presets、依赖脚本或执行构建。

## 构建基线

| 事项 | 设计 |
| --- | --- |
| 语言 | C++20，项目目标显式要求该标准，不启用编译器语言扩展 |
| 构建系统 | CMake + CMakePresets；建议先以 CMake 3.30+ 验证，最低版本仍以实际配置验证确定 |
| 第三方获取 | FetchContent 统一声明，Git 源统一 GIT_SHALLOW=TRUE |
| 版本 | 固定发布 tag，另记录期望 commit；不追随 main／master 或浮动分支 |
| 依赖入口 | 项目顶层集中声明，业务模块只链接 CMake target |
| 主网络库 | cpp-httplib；不同时获取 Drogon、cinatra 等历史备选 |
| 系统组件 | 编译器、Git、操作系统 SDK 和系统库与第三方源码依赖分开管理 |

不再将 vcpkg、Conan 或开发机偶然存在的库作为默认依赖解析路线。依赖内部需要的系统平台组件可以显式查找，但不能因此悄悄换用另一个版本的第三方库。

## 浅克隆与版本固定

**GIT_SHALLOW=TRUE 时，GIT_TAG 使用分支名或 tag，不能直接填 commit hash。** 当前 CMake 文档也说明浅克隆会使用 depth 1。[Git 下载参数](https://cmake.org/cmake/help/v3.30/module/ExternalProject.html)

因此本项目采用“固定发布 tag + 记录期望 commit + 校验实际 HEAD”。记录 tag 解析后的 commit，不拿 annotated tag 对象哈希与工作树 HEAD 混比。tag 被移动或源码与记录不符时，停止构建／发布并提示更新版本记录，不能自动接受。

历史研究里记录的 httplib 等 commit 只代表阅读基准，不直接照搬到浅克隆的 GIT_TAG。实施时选择验证通过的发布 tag 并记录对应 commit；这一轮不凭空填写未经构建验证的版本号。

| 获取属性 | 规则 |
| --- | --- |
| GIT_REPOSITORY | 固定上游或明确指定的镜像 URL，记录来源 |
| GIT_TAG | 固定发布 tag，集中维护 |
| GIT_SHALLOW | TRUE，所有 Git 依赖一致 |
| GIT_PROGRESS | 可启用，让首次获取有明确进度 |
| GIT_SUBMODULES | 显式只选需要项；不需要时禁用，不默认把所有递归子模块拉进来 |
| 子依赖 | 同样锁定版本与选项；可控情况下由顶层提前声明，防止重复获取不同版本 |

顶层声明优先与 MakeAvailable 的组合依据 [FetchContent 文档](https://cmake.org/cmake/help/v3.30/module/FetchContent.html)。浅克隆减少历史，不保证依赖工作树、小文件总量、LFS 内容或必要子模块都很小，发布体积仍需测量。

## 依赖清单与构建接入

| 依赖类别 | 当前状态 | 获取／接入要求 |
| --- | --- | --- |
| cpp-httplib | 主方案 | FetchContent 浅拉固定 tag，只启用所选后端和必要功能 |
| nlohmann/json | 详细设计拟采用 | 同样浅拉，关闭其测试、示例与非必要安装目标 |
| SQLite | 持久化设计基线 | 通过 FetchContent 获取选定源；若该源无可直接使用的 CMake 入口，由项目维护适配 target，并显式处理源码生成工具 |
| 压缩库 | 按平台响应／歌词协议需要 | 确有依赖才声明，避免服务运行时再下载源码 |
| TLS／协议加密 | 仍待选择具体实现 | 选定第三方后按相同策略获取；Windows 系统 SDK 组件不伪装成待下载库 |
| 标签处理 | 候选待验证 | 只在启用相应功能时获取，普通音频传输不强制拉完整 FFmpeg |
| 测试框架 | 实施阶段确定 | 仅 BUILD_TESTING 开启时获取；不为当前文档设计下载并编译 |

不能假定每个上游仓库都有根 CMakeLists、正确导出 target 或自动生成的单文件源码。SQLite、所选 TLS 等需在接入时先验证构建入口与工具要求；问题在 CMake 适配层解决，不散落到业务目录，也不静默回退到系统安装版本。

## 拟建 CMake 文件职责

```text
cpp/
  CMakeLists.txt                     # 工程、标准、依赖入口、子目标
  CMakePresets.json                   # 可共享的工具链与构建配置
  CMakeUserPresets.json               # 用户本机覆盖，不提交
  cmake/
    ProjectOptions.cmake             # 项目功能与编译选项
    dependencies/
      Versions.cmake                 # URL、tag、期望 commit 的单一来源
      Dependencies.cmake             # 统一声明与 MakeAvailable 调用次序
      VerifyDependency.cmake         # HEAD／版本来源检查
      adapters/                      # 无合适上游 CMake target 时的适配
    toolchains/                      # Windows／Linux／交叉编译设置
```

上表只是目录方案，文件尚未生成。模块目标沿用[模块与目录](modules-and-layout.md)，不通过全局 include_directories／link_directories 隐含传播依赖；以 target 为单位链接与控制编译宏。

项目功能开关与第三方选项分开命名，顶层先关闭不需要的依赖测试／示例／工具，再使依赖可用。编译器、架构、运行库和 TLS 组合纳入 Preset，不能混用不同运行库构建的静态库。

## 缓存、离线与 CI

构建目录与第三方源码默认不提交 Git。不同编译器、架构和配置使用隔离的 binary directory；共享缓存可以复用经过版本校验的源码，但不共享不同工具链的第三方二进制构建目录。

首次配置需要获取依赖；离线运行使用已准备并校验的源码缓存或 source directory 覆盖。不能在没有依赖的空目录打开 FULLY_DISCONNECTED 就声称完成离线构建。[离线获取约束](https://cmake.org/cmake/help/v3.30/module/FetchContent.html)

CI 缓存键至少包含依赖版本记录、工具链、目标架构、TLS 与功能选项。镜像或代理只改变传输位置，不能改变期望版本；版本更新由一次明确的依赖变更记录驱动。

每项依赖记录 URL、tag、实际 commit、必要子模块、构建选项、许可证与补丁。补丁独立存放且与版本绑定；不修改 FetchContent 缓存后把开发机临时状态当成可复现源码。

## 实施验证

首次干净配置能获取依赖；所有 Git 获取均浅克隆；tag 与期望 commit 匹配；缓存后可按约定离线配置；不同架构构建隔离；不启用测试时不获取测试依赖；无隐含系统第三方版本替换；SQLite／TLS 等非平凡接入路径在 Windows 和目标 Linux 工具链实际验证。
