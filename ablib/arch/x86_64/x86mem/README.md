# x86mem (P5-100 许可记录)

本目录为上游 x86mem (https://github.com/.../x86mem) 的移植副本:
`memcpy.c / memmove.c / memcmp.c / memset.c` 及其 intrinsic 头。

- 上游许可: MIT (许可文本随上游分发; 移植时未携带 LICENSE 文件,
  列入回填清单)。
- `memops.o` (1.37MB) 为构建产物, 不入库 (见仓库 .gitignore),
  由 `make` 从源码重建。
- `*.d` 为编译器依赖文件, 属构建产物。
