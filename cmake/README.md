# 构建支持

ProjectOptions.cmake 维护公共 C++20 要求（MSVC 另加 /utf-8、/permissive- 等）与项目代码的警告选项；警告只作用于项目目标，不作用于第三方源码。MUSIC_SERVICE_WARNINGS_AS_ERRORS 可把警告升级为错误。

- dependencies：集中声明、获取并校验固定版本的第三方源码，见[依赖说明](dependencies/README.md)。
- adapters：为没有合适 CMake 目标的第三方源码提供目标，目前有 cpp-httplib（启用 Mbed TLS）与 SQLite。
- toolchains：在目标环境明确后加入交叉编译配置。

不写入开发机绝对路径；本机差异放在已忽略的 CMakeUserPresets.json。尚无实现的安装包不导出。
