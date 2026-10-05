# x86mem (P5-100 许可记录)

本目录为上游 x86mem (https://github.com/.../x86mem) 的移植副本:
`memcpy.c / memmove.c / memcmp.c / memset.c` 及其 intrinsic 头。

- 上游许可: MIT (许可文本随上游分发; 移植时未携带 LICENSE 文件,
  列入回填清单)。
- `memops.o` 为构建产物, 不入库 (见仓库 .gitignore), 由 `make` 从源码重建。
- `*.d` 为编译器依赖文件, 属构建产物。

## 现状: 四个 .c 已全部重写 (非上游原码)

上游的"每个块函数手写一遍 + 每 4KB 重走一次 if-else 阶梯"结构已整体替换为
紧凑的分层实现 (6295 行 → 约 900 行), 对外 ABI (`AVX_mem{cpy,set,move,cmp}V0..V3`、
`AVX_memset_4BV0..V3`、`MEMOPS_SupportV1..V3`) 保持不变。重写修掉了三个实测
可触发的缺陷, 详见各文件头部注释:

- `memset.c`: 阶梯 rung 的粒度与块函数实际块宽不匹配 → 越界写 (AVX512 tier
  最多越界 2KB); 另外对齐分支返回了已推进过的 `dest`。
- `memcmp.c`: 按 16/32/64 位整单元比大小决定正负号, 小端下与 memcmp 的
  "首个差异字节"语义不等价。
- `memmove.c`: 正向重叠路径与 4× 展开的读写顺序在重叠时存在隐患 (新实现
  统一为"每组先全读再全写")。

## 非时序 (NT) 存储阈值是运行时变量

`x86mem_cache_limit` (单位字节) 取代了原来的编译期常量 `CACHESIZELIMIT`。
`x86mem_init.c` 里的 `x86mem_init_cache_limit()` 用 CPUID 探测 L3 容量
(leaf 4, 回退 AMD `Fn8000_0006_EDX[31:18]`), 取
`clamp(L3 / X86MEM_CACHE_LIMIT_L3_DIV)` 写入。启动代码负责调用:

| 链接单元 | 调用点 |
|---|---|
| 用户态 | `lib/base/arch/x86_64/init.c` → `_init_runtime_and_global_variables()` |
| 内核态 | `kernel/src/arch/x86_64/init.cpp` → `x86_64_init()` (SSE 之后, PMM/VMM/SLAB 之前) |

探测不到 (返回 0) 或还没跑到初始化时, 保持 `X86MEM_CACHE_LIMIT_DEFAULT` (3MB)。
mem* 只拿它挑路径, 取错值只影响性能。

**为什么不是直接等于 L3**: 12MB L3 的机器上实测 (见 `tests/memops` 的
"NT 开/关对照", 3 轮一致), streaming store 的 crossover 远低于 L3:

| size | memcpy 关→开 | memset 关→开 |
|---|---|---|
| 256K / 512K | 39 → 29 (NT 更慢) | 48 → 34 (NT 更慢) |
| 1M | 20 → **29** | 48 → 34 (NT 更慢) |
| 2M | 18.6 → **25.6** | 30.5 → **34.5** |
| 4M | 18.5 → **23.3** | 29.5 → **34.2** |

crossover 落在 L3/16 ~ L3/8 之间, 直接拿 L3 当阈值会让 1M~12M 的 memcpy
错过 NT (丢 40%~50%)。`X86MEM_CACHE_LIMIT_L3_DIV` 默认 8, 设成 1 就是
"阈值 = 原始 L3 容量"。

## 性能调参的硬性约束

以下三项都是**实测敏感**的, 改之前先跑一次 `tests/memops` 基准作基线:

1. **主循环展开度**: 目标机器 (Alder Lake) 上 4× 最稳, 8× 反而掉约 15%
   (store buffer / 调度压力)。
2. **memmove 的三条路径不能全内联**: 全部内联会让主入口膨胀到 ~1.3KB,
   64B memmove 吞吐掉近一半 (I-cache 命中率)。`xm_copy_fwd_seq` 与
   `xm_copy_bwd` 必须保持 `noinline`。
3. **快慢路径分界按"能否摊薄循环开销"定**: 快路径门槛是
   `n >= 4*XM_VEC && gap >= XM_VEC`。只用 `n >= XM_VEC` 会让 64B~128B 档
   掉到 9.8 GB/s (改对后 14.6 GB/s)。

## 测试

`tests/memops/` (宿主正确性 + 基准 + 旧实现对照), 见 `tests/memops/README.md`。
