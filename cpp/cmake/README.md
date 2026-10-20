# 构建支持

ProjectOptions.cmake 维护公共 C++20 要求。平台／编译器适配、依赖声明与安装导出规则后续按实际需要增加。

- dependencies：集中记录并获取固定版本的第三方源码。
- adapters：适配缺少标准 CMake target 的第三方构建。
- toolchains：在目标环境明确后加入交叉编译配置。

不写入开发机绝对路径；本机差异放在已忽略的 CMakeUserPresets.json。当前不获取第三方依赖、不导出尚无实现的安装包。
